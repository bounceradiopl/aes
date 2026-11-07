// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder REST API and embedded panel, on the pattern of engine/src/http_server.cpp.
#include "bam/encoder/http_server.hpp"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <thread>

#include "bam/auth.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/encoder/version.hpp"
#include "bam/http/guard.hpp"
#include "bam/http/login_throttle.hpp"
#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "codec_impl.hpp"

namespace bam::encoder {

namespace {
using json = nlohmann::ordered_json;

// Not "bam_session" (BounceAES Master): browsers keep cookies per host, not per port, and both
// panels run on the same computer.
constexpr const char* kCookie = "bounce_encoder_session";
constexpr int kHttpThreads = 32;  // every open panel tab holds one thread for its live feed
constexpr int64_t kLiveIntervalNs = 200'000'000;

using http::is_local;  // the address and a loopback Host (bam/http/guard.hpp)
using http::is_local_address;
using http::is_loopback_host;
using http::send_error;
using http::send_json;

std::string session_cookie(const httplib::Request& req) { return http::session_cookie(req, kCookie); }

// Every request body: depth- and size-checked before it is parsed (bam/http/guard.hpp).
std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res) { return http::parse_json_body(req, res); }

// A text field of a request body; "" when missing or not text.
std::string text(const json& body, const char* key) {
  if (!body.is_object()) return {};
  const auto it = body.find(key);
  return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::atomic<int64_t> g_clock_offset_ns{0};  // testing::set_http_clock_offset_ns
std::mutex g_hook_mutex;
std::function<void()> g_login_verified_hook;  // testing::set_login_verified_hook

int64_t now_ns() { return platform::monotonic_ns() + g_clock_offset_ns.load(); }

double round1(double v) { return std::round(v * 10.0) / 10.0; }

json peaks(const float (&p)[2]) { return json::array({round1(p[0]), round1(p[1])}); }

int64_t uptime_s(int64_t started_ns, int64_t now_ns) { return started_ns > 0 ? (now_ns - started_ns) / 1'000'000'000 : 0; }

// "locked" | "holdover" | "listening" (acquiring a master) | "freerun" (no master) | "disabled"
const char* ptp_state_code(const EncoderStatus& s) {
  if (!s.ptp_enabled) return "disabled";
  switch (s.ptp.state) {
    case PtpState::Locked: return "locked";
    case PtpState::Holdover: return "holdover";
    case PtpState::Listening: return s.ptp.steps > 0 ? "listening" : "freerun";
    case PtpState::Master: return "master";  // not used: the encoder is slave only
  }
  return "freerun";
}

const char* icecast_state_code(IcecastState s) {
  switch (s) {
    case IcecastState::Idle: return "idle";
    case IcecastState::Connecting: return "connecting";
    case IcecastState::Streaming: return "streaming";
    case IcecastState::Error: return "error";
  }
  return "idle";
}

// RFC 4648 base64 with padding (the preset files travel inside JSON).
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
    for (int k = 18; k >= 0; k -= 6) out += kBase64[(v >> k) & 63];
  }
  if (i < in.size()) {
    uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kBase64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// Strict: the standard alphabet, padded to a multiple of 4, '=' only at the end (no spaces, no
// line breaks, no "data:" prefix). nullopt when it is not such a text.
std::optional<std::string> base64_decode(std::string_view in) {
  if (in.size() % 4 != 0) return std::nullopt;
  int8_t value[256];
  std::fill(std::begin(value), std::end(value), int8_t(-1));
  for (int k = 0; k < 64; ++k) value[uint8_t(kBase64[k])] = int8_t(k);
  size_t pad = 0;
  if (!in.empty() && in.back() == '=') pad = in.size() >= 2 && in[in.size() - 2] == '=' ? 2 : 1;
  std::string out;
  out.reserve(in.size() / 4 * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    uint32_t v = 0;
    const bool last = i + 4 == in.size();
    for (size_t k = 0; k < 4; ++k) {
      const char ch = in[i + k];
      if (last && ch == '=' && k >= 4 - pad) {
        v <<= 6;
        continue;
      }
      const int8_t d = value[uint8_t(ch)];
      if (d < 0) return std::nullopt;
      v = v << 6 | uint32_t(d);
    }
    if (last && ((pad == 2 && (v & 0xFFFF) != 0) || (pad == 1 && (v & 0xFF) != 0))) return std::nullopt;  // bits past the data
    out += char((v >> 16) & 0xFF);
    if (!last || pad < 2) out += char((v >> 8) & 0xFF);
    if (!last || pad < 1) out += char(v & 0xFF);
  }
  return out;
}

// "11.07" for 11070
std::string stereotool_version_text(int version) {
  if (version <= 0) return {};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d.%02d", version / 1000, (version % 1000) / 10);
  return buf;
}

// The library's error for the status (every panel, the live feed): the folders it was looked for
// in become "folder programu" (GET /api/stereotool keeps them, with the path).
std::string without_program_folder(std::string text) {
  std::vector<std::string> dirs;
  for (const std::string& c : stereotool_library_candidates())
    if (const size_t sep = c.find_last_of("/\\"); sep != std::string::npos && sep > 0) dirs.push_back(c.substr(0, sep));
  std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  const std::string folder = "folder programu";
  for (const std::string& d : dirs)
    for (size_t at = text.find(d); at != std::string::npos; at = text.find(d, at + folder.size())) text.replace(at, d.size(), folder);
  for (const std::string& twice : {folder + "; " + folder, "w folderze programu: " + folder})
    for (size_t at = text.find(twice); at != std::string::npos; at = text.find(twice, at))
      text.replace(at, twice.size(), twice.rfind("w ", 0) == 0 ? "w folderze programu" : folder);
  return text;
}

json stereotool_library_json(const StereoToolInfo& i, bool with_path) {
  json j = {{"checked", i.checked},
            {"loading", i.loading},
            {"available", i.available},
            {"version", i.version},
            {"version_text", stereotool_version_text(i.version)},
            {"error", with_path ? i.error : without_program_folder(i.error)},
            {"abandoned", i.abandoned}};
  if (with_path) j["path"] = i.path;
  return j;
}

std::string web_url(int port) { return port > 0 ? "http://127.0.0.1:" + std::to_string(port) + "/" : std::string(); }

json processing_json(const ProcessingStatus& p) {
  return {{"enabled", p.enabled},
          {"preset", p.preset},
          {"state", processing_state_name(p.state)},
          {"reason_code", processing_reason_name(p.reason_code)},
          {"reason", p.reason},
          {"latency_ms", round1(p.latency_ms)},
          {"cpu_pct", round1(p.cpu_pct)},
          {"license_valid", p.license_valid},
          {"unlicensed_features", p.unlicensed_features},
          {"web_port", p.web_port},
          {"web_url", web_url(p.web_port)},
          {"retries", p.retries},
          {"retry_in_s", p.retry_in_s},
          {"queue_dropped_frames", p.queue_dropped_frames}};
}

json ptp_json(const EncoderStatus& s) {
  return {{"enabled", s.ptp_enabled},
          {"state", ptp_state_code(s)},
          {"free_run", s.ptp_free_run},
          {"error", s.ptp_error},
          {"has_master", s.ptp.has_master},
          {"grandmaster", s.ptp.has_master ? s.ptp.grandmaster.to_string() : ""},
          {"master", s.ptp.has_master ? s.ptp.master.clock.to_string() : ""},
          {"domain", s.ptp.domain},
          {"offset_us", round1(s.ptp.offset_ns / 1000.0)},
          {"path_delay_us", round1(s.ptp.path_delay_ns / 1000.0)},
          {"rate_ppm", std::round(s.ptp.rate_ppm * 1000.0) / 1000.0},
          {"sync_lost", s.ptp.sync_lost},
          {"steps", s.ptp.steps}};
}

json status_json(const EncoderStatus& s) {
  const int64_t now = platform::monotonic_ns();  // the engine's times are monotonic
  json stations = json::array();
  for (const StationStatus& st : s.stations) {
    json inputs = json::array();
    for (const InputStatus& in : st.inputs) {
      inputs.push_back({{"index", in.index},
                        {"enabled", in.enabled},
                        {"name", in.name},
                        {"address", in.address},
                        {"signal", in.signal},
                        {"error", in.error},
                        {"latency_us", in.latency_us},
                        {"gain_db", in.gain_db},
                        {"delay_s", in.delay_s},
                        {"peak_db", peaks(in.peak_db)},
                        {"receiver_id", in.receiver_id},
                        {"rx",
                         {{"packets", in.rx.packets},
                          {"bytes", in.rx.bytes},
                          {"underruns", in.rx.underruns},
                          {"overruns", in.rx.overruns},
                          {"seq_errors", in.rx.seq_errors},
                          {"jitter_ms", std::round(in.rx.jitter_ms * 1000.0) / 1000.0}}}});
    }
    json streams = json::array();
    for (const StreamStatus& str : st.streams) {
      std::string library = str.codec_library;
      if (!str.codec_available) library = "niedostępny";
      else if (library.empty()) library = "wbudowany";  // FLAC: linked in
      streams.push_back({{"id", str.id},
                         {"enabled", str.enabled},
                         {"codec", codec_name(str.codec)},
                         {"codec_label", detail::codec_label(str.codec)},
                         {"bitrate_kbps", str.bitrate_kbps},
                         {"sample_rate", str.sample_rate},
                         {"host", str.host},
                         {"port", str.port},
                         {"mount", str.mount},
                         {"codec_available", str.codec_available},
                         {"codec_library", library},
                         {"state", icecast_state_code(str.icecast.state)},
                         {"error", str.icecast.error},
                         {"bytes_sent", str.icecast.bytes_sent},
                         {"kbps", round1(str.icecast.kbps)},
                         {"reconnects", str.icecast.reconnects},
                         {"dropped_bytes", str.icecast.dropped_bytes},
                         {"encoder_failed", !str.encoder_error.empty()},
                         {"encoder_error", str.encoder_error},
                         {"queue_dropped_frames", str.queue_dropped_frames},
                         {"conditioner_dropped_frames", str.conditioner_dropped_frames},
                         {"peak_db", peaks(str.peak_db)},
                         {"uptime_s", uptime_s(str.started_ns, now)},  // since the stream (worker) started
                         {"connected_s", uptime_s(str.icecast.connected_since_ns, now)}});  // since the current connection
    }
    stations.push_back({{"id", st.id},
                        {"name", st.name},
                        {"enabled", st.enabled},
                        {"running", st.running},
                        {"uptime_s", uptime_s(st.started_ns, now)},
                        {"peak_db", peaks(st.peak_db)},
                        {"skipped_frames", st.skipped_frames},
                        {"inputs", std::move(inputs)},
                        {"streams", std::move(streams)},
                        {"processing", processing_json(st.processing)}});
  }
  return {{"product", kEncoderProductName},
          {"version", kEncoderVersion},
          {"running", s.running},
          {"uptime_s", s.uptime_s},
          {"interface_ip", s.interface_ip},
          {"ptp", ptp_json(s)},
          {"stereotool", stereotool_library_json(s.stereotool, false)},
          {"stations", std::move(stations)}};
}

// The public summary the tray polls (no names, no addresses).
json health_json(const EncoderStatus& s) {
  int stations_running = 0, inputs_enabled = 0, inputs_signal = 0, streams_enabled = 0, streams_streaming = 0;
  for (const StationStatus& st : s.stations) {
    if (!st.running) continue;
    ++stations_running;
    for (const InputStatus& in : st.inputs) {
      inputs_enabled += in.enabled;
      inputs_signal += in.enabled && in.signal;
    }
    for (const StreamStatus& str : st.streams) {
      streams_enabled += str.enabled;
      streams_streaming += str.enabled && str.icecast.state == IcecastState::Streaming;
    }
  }
  return {{"service", "ok"},
          {"product", kEncoderProductName},
          {"version", kEncoderVersion},
          {"ptp", ptp_state_code(s)},
          {"ptp_free_run", s.ptp_free_run},
          {"stations_running", stations_running},
          {"inputs_enabled", inputs_enabled},
          {"inputs_signal", inputs_signal},
          {"streams_enabled", streams_enabled},
          {"streams_streaming", streams_streaming}};
}

// Spec §8: what an AAC bitrate sounds like in MP3 terms, and which AAC profile suits a bitrate.
// (Guidance for the user, not a rule: the allowed values come from validate_encoder_settings.)
struct Mp3Equivalent {
  Codec codec;
  uint32_t min_kbps;
  std::optional<uint32_t> max_kbps;  // nullopt: and above
  const char* mp3;
};
constexpr Mp3Equivalent kMp3Equivalents[] = {
    {Codec::HeAacV2, 32, 32, "~96 kb/s"},     {Codec::HeAac, 48, 64, "~128 kb/s"},
    {Codec::AacLc, 96, 96, "~128–160 kb/s"},  {Codec::AacLc, 128, 128, "~192 kb/s"},
    {Codec::AacLc, 192, std::nullopt, "~256–320 kb/s"},
};
struct Recommendation {
  Codec codec;
  std::optional<uint32_t> min_kbps, max_kbps;
};
constexpr Recommendation kRecommendations[] = {
    {Codec::HeAacV2, std::nullopt, 48}, {Codec::HeAac, 48, 96}, {Codec::AacLc, 96, std::nullopt}};
constexpr const char* kRecommendationText = "HE-AACv2 do 48 kb/s, HE-AAC 48–96 kb/s, AAC-LC powyżej 96 kb/s";

json opt(const std::optional<uint32_t>& v) { return v ? json(*v) : json(nullptr); }

json codecs_json() {
  json codecs = json::array();
  for (Codec c : kCodecs) {
    const CodecLimits& lim = codec_limits(c);
    json rates = json::array();
    for (uint32_t rate : lim.rates) {
      json r = {{"sample_rate", rate}};
      if (lim.lossless) {
        r["min_kbps"] = nullptr;
        r["max_kbps"] = nullptr;
        r["steps"] = nullptr;
      } else {
        // the allowed bitrates, straight from the settings check the configuration uses
        std::vector<uint32_t> allowed;
        for (uint32_t kbps = lim.min_kbps; kbps <= lim.max_kbps; ++kbps)
          if (validate_encoder_settings({c, kbps, rate, 2}, nullptr)) allowed.push_back(kbps);
        if (allowed.empty()) continue;
        const bool contiguous = allowed.back() - allowed.front() + 1 == allowed.size();
        r["min_kbps"] = allowed.front();
        r["max_kbps"] = allowed.back();
        r["steps"] = contiguous ? json(nullptr) : json(allowed);  // null: any value from min to max
      }
      rates.push_back(std::move(r));
    }
    const bool available = codec_available(c);
    std::string reason;
    if (!available)
      reason = std::string("kodek ") + detail::codec_label(c) + " niedostępny: brak biblioteki " +
               (c == Codec::Mp3 ? "libmp3lame" : "libfdk-aac") + " obok programu";
    codecs.push_back({{"id", codec_name(c)},
                      {"label", detail::codec_label(c)},
                      {"content_type", codec_content_type(c)},
                      {"available", available},
                      {"library", codec_library_path(c)},
                      {"reason", reason},
                      {"lossless", lim.lossless},
                      {"min_kbps", lim.lossless ? json(nullptr) : json(lim.min_kbps)},
                      {"max_kbps", lim.lossless ? json(nullptr) : json(lim.max_kbps)},
                      {"rates", std::move(rates)}});
  }
  json equivalents = json::array();
  for (const auto& e : kMp3Equivalents)
    equivalents.push_back({{"codec", codec_name(e.codec)}, {"min_kbps", e.min_kbps}, {"max_kbps", opt(e.max_kbps)}, {"mp3", e.mp3}});
  json recommendations = json::array();
  for (const auto& r : kRecommendations)
    recommendations.push_back({{"codec", codec_name(r.codec)}, {"min_kbps", opt(r.min_kbps)}, {"max_kbps", opt(r.max_kbps)}});
  json rates = json::array();
  for (uint32_t r : kStreamRates) rates.push_back(r);
  return {{"rates", std::move(rates)},
          {"codecs", std::move(codecs)},
          {"mp3_equivalents", std::move(equivalents)},
          {"recommendations", std::move(recommendations)},
          {"recommendation_text", kRecommendationText}};
}

json issues_json(const std::vector<EncoderConfigIssue>& issues) {
  json out = json::array();
  for (const auto& i : issues) out.push_back({{"field", i.field}, {"message", i.message}});
  return out;
}

json api_config(const EncoderConfig& c) { return json::parse(config_to_api_json(c)); }

const char* preset_error_code(PresetError e) {
  switch (e) {
    case PresetError::BadName: return "bad_name";
    case PresetError::Empty: return "empty";
    case PresetError::TooLarge: return "too_large";
    case PresetError::TooMany: return "too_many";
    case PresetError::Exists: return "exists";
    case PresetError::NotFound: return "not_found";
    case PresetError::InUse: return "in_use";
    case PresetError::NotRunning: return "not_running";
    case PresetError::Busy: return "busy";
    case PresetError::Failed: return "failed";
    case PresetError::None: break;
  }
  return "";
}

int preset_error_status(PresetError e) {
  switch (e) {
    case PresetError::BadName:
    case PresetError::Empty: return 400;
    case PresetError::TooLarge: return 413;
    case PresetError::TooMany:
    case PresetError::Exists:
    case PresetError::InUse:
    case PresetError::NotRunning:
    case PresetError::Busy: return 409;
    case PresetError::NotFound: return 404;
    case PresetError::Failed:
    case PresetError::None: break;
  }
  return 500;
}

// {"ok": false, "error": "<Polish>", "code": "<preset_error_code or another>", "field": "<body key>"}
void send_refusal(httplib::Response& res, int status, const std::string& code, const std::string& message,
                  const std::string& field = "") {
  send_json(res, {{"ok", false}, {"error", message}, {"code", code}, {"field", field}}, status);
}

void send_preset_error(httplib::Response& res, const PresetResult& r, const std::string& field) {
  const bool about_name = r.error == PresetError::BadName || r.error == PresetError::Exists;
  const bool about_data = r.error == PresetError::Empty || r.error == PresetError::TooLarge;
  send_refusal(res, preset_error_status(r.error), preset_error_code(r.error), r.message,
               about_name ? field : about_data ? std::string("data_base64") : std::string());
}

// Every stored preset with the stations that use it (any case), sorted as the store lists them.
json presets_json(const std::vector<PresetInfo>& presets, const EncoderConfig& c) {
  json out = json::array();
  for (const PresetInfo& p : presets) {
    json users = json::array();
    for (const StationConfig& st : c.stations)
      if (same_preset_name(st.processing.preset, p.name)) users.push_back({{"id", st.id}, {"name", st.name}});
    out.push_back({{"name", p.name}, {"bytes", p.bytes}, {"used_by", std::move(users)}});
  }
  return out;
}

json preset_info_json(const std::vector<PresetInfo>& presets, std::string_view name) {
  for (const PresetInfo& p : presets)
    if (same_preset_name(p.name, name)) return {{"name", p.name}, {"bytes", p.bytes}};
  return nullptr;
}

// "„Radio X”" / "nr 2": the station in messages
std::string station_name(const StationConfig& st, size_t i) {
  return st.name.empty() ? "nr " + std::to_string(i + 1) : "„" + st.name + "”";
}

// A station update without "processing" keeps the stored block; a partial one changes only the
// keys it has (e.g. {"enabled": false} keeps the preset, the slot and the key). `stored` is the
// station's object of the API form.
void keep_processing(json& station, const json& stored) {
  if (!stored.is_object() || !stored.contains("processing")) return;
  if (!station.contains("processing")) {
    station["processing"] = stored["processing"];
    return;
  }
  json& p = station["processing"];
  if (!p.is_object()) return;  // config_from_json names the field
  json merged = stored["processing"];
  for (const auto& [key, value] : p.items()) merged[key] = value;
  p = std::move(merged);
}

std::string station_path(size_t i) { return "stations[" + std::to_string(i) + "]"; }

// Ruling E7: a stored password is not carried to another server. When an enabled stream lost its
// stored password that way, the "give a password" issue says so.
void explain_dropped_passwords(std::vector<EncoderConfigIssue>& issues, const EncoderConfig& next, const EncoderConfig& prev) {
  for (size_t i = 0; i < next.stations.size(); ++i) {
    const StationConfig& st = next.stations[i];
    for (size_t j = 0; j < st.streams.size(); ++j) {
      const StreamConfig& s = st.streams[j];
      if (!s.password.empty()) continue;
      bool had_password = false;
      for (const StationConfig& ps : prev.stations)
        if (ps.id == st.id)
          for (const StreamConfig& p : ps.streams) had_password |= p.id == s.id && !p.password.empty();
      if (!had_password) continue;
      const std::string field = station_path(i) + ".streams[" + std::to_string(j) + "].password";
      for (auto& issue : issues)
        if (issue.field == field)
          issue.message += " (zapisane hasło nie jest wysyłane do innego serwera: po zmianie serwera lub portu wpisz hasło ponownie)";
    }
  }
}
}  // namespace

struct EncoderHttpServer::Impl {
  EncoderEngine& engine;
  const std::string config_path;
  // A failed bind leaves an httplib::Server unusable, so start() builds a fresh one (with its
  // routes, registered once per server) when the previous attempt failed.
  std::unique_ptr<httplib::Server> server;
  bool server_used = false;  // a bind was tried on `server`
  std::mutex sessions_mutex;
  std::shared_ptr<SessionStore> sessions_ = std::make_shared<SessionStore>();  // replaced on a password change
  uint64_t sessions_generation_ = 0;  // counts the replacements (sessions_mutex)
  http::LoginThrottle throttle;
  std::mutex change_mutex;  // one configuration change (build, validate, save, apply) at a time
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::atomic<bool> listen_returned{false};  // the listen thread is past listen_after_bind()
  uint16_t port = 0;

  // A stop() right after start() must reach a running server (bam/http/guard.hpp).
  void wait_listening() const { http::wait_listening(*server, listen_returned); }

  Impl(EncoderEngine& e, std::string path) : engine(e), config_path(std::move(path)) { make_server(); }

  void make_server() {
    server = std::make_unique<httplib::Server>();
    server_used = false;
    server->new_task_queue = [] { return new httplib::ThreadPool(kHttpThreads); };
    // Ruling E11: one request per connection, the request allowlist before any body is read, JSON
    // answers for httplib's own refusals (bam/http/guard.hpp). The body limit is 1 MB for every
    // request except a preset upload (6 MB: base64 of a 4 MB preset), and an upload over 1 MB
    // without a valid session is answered 401 on its headers, unread: nobody can make the server
    // read more than 1 MB before logging in.
    const auto upload = [](const httplib::Request& req) { return req.method == "POST" && req.path == kPresetUploadPath; };
    http::install_request_guard(
        *server, kMaxPresetUploadBytes,
        [upload](const httplib::Request& req) { return upload(req) ? kMaxPresetUploadBytes : kMaxApiBodyBytes; },
        [this, upload](const httplib::Request& req, httplib::Response& res) {
          if (!upload(req) || req.get_header_value_u64("Content-Length") <= kMaxApiBodyBytes || authorized(req)) return false;
          send_error(res, 401, "wymagane logowanie");
          return true;
        });
    routes(*server);
  }

  std::shared_ptr<SessionStore> sessions() {
    std::lock_guard lock(sessions_mutex);
    return sessions_;
  }

  uint64_t sessions_generation() {
    std::lock_guard lock(sessions_mutex);
    return sessions_generation_;
  }

  // A new session, unless the sessions were restarted (password change) since `generation` was
  // read: a login that checked the old password must not outlive the change. Empty = refused.
  std::string create_session(uint64_t generation) {
    std::lock_guard lock(sessions_mutex);
    if (generation != sessions_generation_) return {};
    return sessions_->create(now_ns());
  }

  // Every session ends except a new one, returned (its token), for whoever changed the password.
  std::string restart_sessions() {
    auto fresh = std::make_shared<SessionStore>();
    std::string token = fresh->create(now_ns());
    std::lock_guard lock(sessions_mutex);
    sessions_ = std::move(fresh);
    ++sessions_generation_;
    return token;
  }

  void set_session_cookie(httplib::Response& res, const std::string& token, bool clear = false) {
    std::string c = std::string(kCookie) + "=" + (clear ? "" : token) + "; Path=/; HttpOnly; SameSite=Strict";
    if (clear) c += "; Max-Age=0";
    res.set_header("Set-Cookie", c);
  }

  bool authorized(const httplib::Request& req) {
    const std::string token = session_cookie(req);
    return !token.empty() && sessions()->valid(token, now_ns());
  }

  // Saves `next` and hands it to the engine. Caller holds change_mutex.
  bool store(const EncoderConfig& next, std::string* error) {
    if (!config_path.empty() && !save_config_file(config_path, next, error)) return false;
    engine.apply_config(next);
    return true;
  }

  // A configuration change: `edit` turns the current configuration (API form, no secrets) into
  // the requested one, or answers the request itself (returns false). Then config_from_json →
  // merge_passwords → normalize_config → validate_config; refused with 400 and nothing changed
  // when invalid, else saved and applied. `done` may add to the answer.
  void change(httplib::Response& res, const std::function<bool(json& doc)>& edit,
              const std::function<void(json& out, const EncoderConfig& next)>& done = {}) {
    std::lock_guard lock(change_mutex);
    const EncoderConfig prev = engine.config();
    json doc = api_config(prev);
    if (!edit(doc)) return;
    std::string err;
    auto next = config_from_json(doc.dump(), &err);
    if (!next) {
      send_json(res,
                {{"ok", false},
                 {"error", err},
                 {"issues", json::array({{{"field", ""}, {"message", err}}})},
                 {"notices", json::array()},
                 {"warnings", json::array()}},
                400);
      return;
    }
    retire_reused_ids(*next, prev);  // before the merge: such a station is a new one
    merge_passwords(*next, prev);
    std::vector<std::string> notices = normalize_config(*next);
    std::vector<EncoderConfigIssue> issues = validate_config(*next);
    if (issues.empty()) issues = missing_presets(*next, prev);
    if (!issues.empty()) {
      explain_dropped_passwords(issues, *next, prev);
      send_json(res,
                {{"ok", false}, {"error", issues.front().message}, {"issues", issues_json(issues)}, {"notices", notices}, {"warnings", json::array()}},
                400);
      return;
    }
    if (!store(*next, &err)) {
      log_error("Nie można zapisać konfiguracji: " + err);
      send_json(res,
                {{"ok", false},
                 {"error", "nie można zapisać konfiguracji: " + err},
                 {"issues", json::array()},
                 {"notices", notices},
                 {"warnings", json::array()}},
                500);
      return;
    }
    if (next->http_bind != prev.http_bind || next->http_port != prev.http_port)
      notices.push_back("Nowy adres panelu (" + next->http_bind + ":" + std::to_string(next->http_port) +
                        ") zadziała po ponownym uruchomieniu usługi");
    json out = {{"ok", true},
                {"issues", json::array()},
                {"notices", notices},
                {"warnings", issues_json(config_warnings(*next))},  // never quote a key
                {"config", api_config(*next)}};
    if (done) done(out, *next);
    send_json(res, out);
  }

  // Processing switched on (or onto another preset) on an enabled station with a preset that is not
  // uploaded: refused, as Stereo Tool could not load it (the station would run unprocessed). A
  // station that already ran with it keeps it (a preset removed from the folder by hand does not
  // block other changes); a disabled station may name one still to be uploaded.
  std::vector<EncoderConfigIssue> missing_presets(const EncoderConfig& next, const EncoderConfig& prev) const {
    std::vector<EncoderConfigIssue> issues;
    std::vector<PresetInfo> stored;
    bool listed = false;
    for (size_t i = 0; i < next.stations.size(); ++i) {
      const StationConfig& st = next.stations[i];
      const ProcessingConfig& p = st.processing;
      if (!st.enabled || !p.enabled || p.preset.empty()) continue;
      bool had = false;
      for (const StationConfig& o : prev.stations)
        had |= o.id == st.id && o.enabled && o.processing.enabled && same_preset_name(o.processing.preset, p.preset);
      if (had) continue;
      if (!listed) {
        stored = engine.list_presets();
        listed = true;
      }
      if (std::any_of(stored.begin(), stored.end(), [&](const PresetInfo& s) { return same_preset_name(s.name, p.preset); })) continue;
      issues.push_back({station_path(i) + ".processing.preset", "Stacja " + station_name(st, i) + ": preset Stereo Tool „" + p.preset +
                                                                     "” nie jest wgrany: wgraj go w oknie presetów albo wybierz inny"});
    }
    return issues;
  }

  enum class PasswordResult { Saved, AlreadySet, SaveFailed };
  // The panel password: saved like any other change, nothing else is touched. `first`: only when
  // no password is set yet (checked under the change lock: two first-time setups cannot both win).
  PasswordResult set_password_hash(const std::string& hash, bool first) {
    std::lock_guard lock(change_mutex);
    EncoderConfig next = engine.config();
    if (first && !next.password_hash.empty()) return PasswordResult::AlreadySet;
    next.password_hash = hash;
    std::string err;
    if (!store(next, &err)) {
      log_error("Nie można zapisać hasła panelu: " + err);
      return PasswordResult::SaveFailed;
    }
    return PasswordResult::Saved;
  }

  using Access = http::Access;
  static constexpr Access kPublic = Access::Public;
  static constexpr Access kSession = Access::Session;

  // The body type (415) and the session (kSession, 401), checked after the body was read; the
  // handler runs only when both pass (bam/http/guard.hpp).
  httplib::Server::Handler guard(Access access, httplib::Server::Handler handler) {
    return http::guard(access, [this](const httplib::Request& req) { return authorized(req); }, std::move(handler));
  }

  bool has_station(const std::string& id) const {
    const EncoderConfig c = engine.config();
    return std::any_of(c.stations.begin(), c.stations.end(), [&](const StationConfig& st) { return st.id == id; });
  }

  static std::optional<size_t> find_station(const json& doc, const std::string& id) {
    const json& list = doc["stations"];
    for (size_t i = 0; i < list.size(); ++i)
      if (text(list[i], "id") == id) return i;
    return std::nullopt;
  }

  void routes(httplib::Server& server) {
    // Every route goes through guard(): the body type (415) and the session (kSession, 401), both
    // checked after the body was read.
    using Handler = httplib::Server::Handler;
    auto get = [&](const std::string& pattern, Access a, Handler h) { server.Get(pattern, guard(a, std::move(h))); };
    auto post = [&](const std::string& pattern, Access a, Handler h) { server.Post(pattern, guard(a, std::move(h))); };
    auto put = [&](const std::string& pattern, Access a, Handler h) { server.Put(pattern, guard(a, std::move(h))); };
    auto del = [&](const std::string& pattern, Access a, Handler h) { server.Delete(pattern, guard(a, std::move(h))); };

    // ---- public ----
    get("/api/health", kPublic, [this](const httplib::Request&, httplib::Response& res) { send_json(res, health_json(engine.status())); });

    get("/api/auth-state", kPublic, [this](const httplib::Request& req, httplib::Response& res) {
      json logo = nullptr;
      for (size_t i = 0; i < kEncoderPanelAssetCount; ++i) {
        const std::string path = kEncoderPanelAssets[i].path;
        if (path == "/logo.svg" || (path == "/logo.png" && logo.is_null())) logo = path.substr(1);
      }
      send_json(res, {{"password_set", !engine.config().password_hash.empty()},
                      {"logged_in", authorized(req)},
                      {"local", is_local(req)},
                      {"logo", logo}});
    });

    post("/api/setup", kPublic, [this](const httplib::Request& req, httplib::Response& res) {
      if (!engine.config().password_hash.empty()) return send_error(res, 409, "hasło jest już ustawione");
      if (!is_local_address(req))
        return send_error(res, 403, "pierwsze hasło można ustawić tylko z tego komputera (127.0.0.1)");
      if (!is_loopback_host(req))
        return send_error(res, 403,
                          "pierwsze hasło można ustawić tylko z panelu otwartego pod adresem http://127.0.0.1 lub http://localhost "
                          "(nie przez nazwę komputera ani inną nazwę domeny)");
      auto body = parse_body(req, res);
      if (!body) return;
      const std::string pw = text(*body, "password");
      if (pw.size() < 8) return send_error(res, 400, "hasło musi mieć co najmniej 8 znaków");
      switch (set_password_hash(make_password_hash(pw), true)) {
        case PasswordResult::AlreadySet: return send_error(res, 409, "hasło jest już ustawione");
        case PasswordResult::SaveFailed: return send_error(res, 500, "nie można zapisać hasła");
        case PasswordResult::Saved: break;
      }
      log_info("Ustawiono hasło panelu");
      set_session_cookie(res, sessions()->create(now_ns()));
      send_json(res, {{"ok", true}});
    });

    post("/api/login", kPublic, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      const uint64_t generation = sessions_generation();  // before the hash: see create_session
      const std::string stored = engine.config().password_hash;
      if (stored.empty()) return send_error(res, 409, "hasło nie jest jeszcze ustawione");
      // guessing is limited by counting failures (429), not by holding a request thread; the ticket
      // counts this attempt until it ends, so attempts sent at once cannot all get past the limit
      auto attempt = throttle.attempt(req.remote_addr, now_ns());
      if (!attempt) return send_error(res, 429, "zbyt wiele nieudanych prób, spróbuj za chwilę");
      if (!verify_password(text(*body, "password"), stored)) {
        attempt.failed(now_ns());
        log_warning("Nieudane logowanie do panelu z " + req.remote_addr);
        return send_error(res, 401, "nieprawidłowe hasło");
      }
      attempt.succeeded();
      {
        std::lock_guard hook(g_hook_mutex);
        if (g_login_verified_hook) g_login_verified_hook();
      }
      const std::string token = create_session(generation);
      if (token.empty()) return send_error(res, 401, "hasło zostało właśnie zmienione, zaloguj się ponownie");
      set_session_cookie(res, token);
      send_json(res, {{"ok", true}});
    });

    // ---- authorized ----
    post("/api/logout", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      sessions()->remove(session_cookie(req));
      set_session_cookie(res, "", true);
      send_json(res, {{"ok", true}});
    });

    post("/api/password", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      if (!verify_password(text(*body, "current"), engine.config().password_hash))
        return send_error(res, 403, "obecne hasło jest nieprawidłowe");
      const std::string pw = text(*body, "password");
      if (pw.size() < 8) return send_error(res, 400, "hasło musi mieć co najmniej 8 znaków");
      if (set_password_hash(make_password_hash(pw), false) != PasswordResult::Saved)
        return send_error(res, 500, "nie można zapisać hasła");
      // every other session (other tabs, other computers) ends; this one goes on with a new cookie
      set_session_cookie(res, restart_sessions());
      log_info("Zmieniono hasło panelu");
      send_json(res, {{"ok", true}});
    });

    get("/api/config", kSession, [this](const httplib::Request&, httplib::Response& res) { send_json(res, api_config(engine.config())); });

    // The whole configuration or some of its top-level keys ("stations" replaces every station);
    // keys left out keep their value. Stream passwords sent as "" keep the stored ones (E7).
    put("/api/config", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      change(res, [&](json& doc) {
        const json stored = doc["stations"];
        for (const auto& [key, value] : body->items())
          if (key != "password_hash") doc[key] = value;  // the panel password changes only through /api/password
        // a station (matched by id) sent without its "processing" keeps the stored one
        if (doc["stations"].is_array())
          for (json& st : doc["stations"]) {
            if (!st.is_object() || text(st, "id").empty()) continue;
            for (const json& old : stored)
              if (text(old, "id") == text(st, "id")) keep_processing(st, old);
          }
        return true;
      });
    });

    // A new station (the station object of the configuration); it always gets a new id.
    post("/api/stations", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON stacji");
      body->erase("id");
      change(
          res,
          [&](json& doc) {
            doc["stations"].push_back(*body);
            return true;
          },
          [](json& out, const EncoderConfig& next) { out["id"] = next.stations.back().id; });
    });

    // One station replaced (the id comes from the path); the others stay as they are.
    put(R"(/api/stations/([A-Za-z0-9_-]+))", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const std::string id = req.matches[1];
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON stacji");
      (*body)["id"] = id;
      change(
          res,
          [&](json& doc) {
            const auto i = find_station(doc, id);
            if (!i) {
              send_error(res, 404, "brak stacji „" + id + "”");
              return false;
            }
            keep_processing(*body, doc["stations"][*i]);
            doc["stations"][*i] = *body;
            return true;
          },
          [&](json& out, const EncoderConfig&) { out["id"] = id; });
    });

    del(R"(/api/stations/([A-Za-z0-9_-]+))", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const std::string id = req.matches[1];
      change(res, [&](json& doc) {
        const auto i = find_station(doc, id);
        if (!i) {
          send_error(res, 404, "brak stacji „" + id + "”");
          return false;
        }
        doc["stations"].erase(*i);
        return true;
      });
    });

    get("/api/status", kSession, [this](const httplib::Request&, httplib::Response& res) { send_json(res, status_json(engine.status())); });

    get("/api/codecs", kSession, [](const httplib::Request&, httplib::Response& res) { send_json(res, codecs_json()); });

    // ---- Stereo Tool processing (Bounce Encoder 1.1) ----

    // The library (looked for and loaded here when nothing has asked for it yet: the panel asks
    // only when it shows Stereo Tool; /api/status never loads it), the presets with the stations
    // using them, each station's web interface and the configuration's warnings. No key.
    get("/api/stereotool", kSession, [this](const httplib::Request&, httplib::Response& res) {
      const StereoToolInfo library = stereotool_info();
      const EncoderConfig c = engine.config();
      const EncoderStatus status = engine.status();
      json stations = json::array();
      for (const StationConfig& sc : c.stations) {
        const int slot = sc.processing.web_port_slot;
        int active = 0;
        for (const StationStatus& ss : status.stations)
          if (ss.id == sc.id) active = ss.processing.web_port;
        stations.push_back({{"id", sc.id},
                            {"name", sc.name},
                            {"enabled", sc.enabled},
                            {"processing_enabled", sc.processing.enabled},
                            {"preset", sc.processing.preset},
                            {"license_key_set", !sc.processing.license_key.empty()},
                            {"web_port_slot", slot},
                            {"web_port", slot >= 0 && slot < kMaxStations ? c.stereotool_web_port_base + slot : 0},
                            {"web_port_active", active},
                            {"web_url", web_url(active)}});
      }
      send_json(res, {{"library", stereotool_library_json(library, true)},
                      {"presets", presets_json(engine.list_presets(), c)},
                      {"limits",
                       {{"max_preset_bytes", kMaxPresetBytes},
                        {"max_presets", kMaxPresets},
                        {"max_name_chars", kMaxPresetNameChars},
                        {"max_upload_bytes", kMaxPresetUploadBytes}}},
                      {"web_port_base", c.stereotool_web_port_base},
                      {"default_key_set", !c.stereotool_default_key.empty()},
                      {"stations", std::move(stations)},
                      {"warnings", issues_json(config_warnings(c))}});
    });

    get("/api/stereotool/presets", kSession, [this](const httplib::Request&, httplib::Response& res) {
      send_json(res, {{"presets", presets_json(engine.list_presets(), engine.config())}});
    });

    // An upload: {"name": "FM Rock.sts", "data_base64": "<the file, RFC 4648 base64>"}. This route
    // alone takes a body of up to 6 MB (with a session; see make_server).
    post(kPresetUploadPath, kSession, [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_refusal(res, 400, "bad_request", "oczekiwano obiektu JSON z polami name i data_base64");
      const auto name = body->find("name");
      if (name == body->end() || !name->is_string() || name->get_ref<const std::string&>().empty())
        return send_refusal(res, 400, "bad_name", "podaj nazwę presetu (plik .sts)", "name");
      const auto data = body->find("data_base64");
      if (data == body->end() || !data->is_string())
        return send_refusal(res, 400, "bad_data", "brak treści presetu (pole data_base64)", "data_base64");
      const std::string& encoded = data->get_ref<const std::string&>();
      if (encoded.size() > (kMaxPresetBytes + 2) / 3 * 4)
        return send_refusal(res, 413, "too_large", "preset większy niż " + std::to_string(kMaxPresetBytes / (1024 * 1024)) + " MB",
                            "data_base64");
      const auto bytes = base64_decode(encoded);
      if (!bytes) return send_refusal(res, 400, "bad_data", "treść presetu nie jest poprawnym tekstem base64", "data_base64");
      const std::string preset = name->get<std::string>();
      const PresetResult r = engine.put_preset(preset, *bytes);
      if (!r.ok()) return send_preset_error(res, r, "name");
      const auto presets = engine.list_presets();
      send_json(res, {{"ok", true}, {"preset", preset_info_json(presets, preset)}, {"presets", presets_json(presets, engine.config())}});
    });

    // A download, as JSON like the upload: {"name", "bytes", "data_base64"}.
    get(R"(/api/stereotool/presets/(.+))", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const std::string name = req.matches[1];
      std::string data;
      const PresetResult r = engine.get_preset(name, &data);
      if (!r.ok()) return send_preset_error(res, r, "");
      const json info = preset_info_json(engine.list_presets(), name);
      send_json(res, {{"name", info.is_object() ? info["name"] : json(name)}, {"bytes", data.size()}, {"data_base64", base64_encode(data)}});
    });

    // Refused (409, naming the stations) while a station of the configuration uses it.
    del(R"(/api/stereotool/presets/(.+))", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const PresetResult r = engine.remove_preset(std::string(req.matches[1]));
      if (!r.ok()) return send_preset_error(res, r, "");
      send_json(res, {{"ok", true}, {"presets", presets_json(engine.list_presets(), engine.config())}});
    });

    // The station's current Stereo Tool settings (e.g. tuned in its web interface) as a new preset
    // {"name": "..."}; waits for Stereo Tool up to 10 s.
    post(R"(/api/stations/([A-Za-z0-9_-]+)/processing/save-preset)", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const std::string id = req.matches[1];
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_refusal(res, 400, "bad_request", "oczekiwano obiektu JSON z polem name");
      const std::string name = text(*body, "name");
      if (name.empty()) return send_refusal(res, 400, "bad_name", "podaj nazwę presetu (plik .sts)", "name");
      if (!has_station(id)) return send_refusal(res, 404, "no_station", "brak stacji „" + id + "”");
      const PresetResult r = engine.save_processing_preset(id, name);
      if (!r.ok()) return send_preset_error(res, r, "name");
      const auto presets = engine.list_presets();
      send_json(res, {{"ok", true}, {"preset", preset_info_json(presets, name)}, {"presets", presets_json(presets, engine.config())}});
    });

    // The manual "Uruchom ponownie processing" (no body needed).
    post(R"(/api/stations/([A-Za-z0-9_-]+)/processing/restart)", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      const std::string id = req.matches[1];
      if (!has_station(id)) return send_refusal(res, 404, "no_station", "brak stacji „" + id + "”");
      std::string err;
      if (!engine.restart_processing(id, &err)) return send_refusal(res, 409, "not_running", err);
      send_json(res, {{"ok", true}});
    });

    get("/api/logs", kSession, [](const httplib::Request&, httplib::Response& res) {
      json out = json::array();
      for (const auto& e : Logger::instance().recent()) {
        const char* level = e.level == LogLevel::Error ? "error" : e.level == LogLevel::Warning ? "warning" : "info";
        out.push_back({{"time_ms", e.unix_ms}, {"level", level}, {"message", e.message}});
      }
      send_json(res, out);
    });

    get("/api/interfaces", kSession, [](const httplib::Request&, httplib::Response& res) {
      json out = json::array();
      for (const auto& i : platform::list_interfaces()) {
        if (i.loopback) continue;
        out.push_back({{"name", i.name}, {"description", i.description}, {"ipv4", i.ipv4}, {"up", i.up}});
      }
      send_json(res, out);
    });

    // Server-sent events: the /api/status document 5 times a second (meters, stream states).
    // The session is checked before every event: logout, expiry or a password change ends it.
    get("/api/live", kSession, [this](const httplib::Request& req, httplib::Response& res) {
      res.set_header("X-Accel-Buffering", "no");
      res.set_chunked_content_provider("text/event-stream", [this, token = session_cookie(req)](size_t, httplib::DataSink& sink) {
        if (stopping || !sessions()->alive(token, now_ns())) return false;  // alive: an open feed does not extend the session
        const std::string msg = "event: status\ndata: " + status_json(engine.status()).dump() + "\n\n";
        if (!sink.write(msg.data(), msg.size())) return false;
        platform::sleep_ns(kLiveIntervalNs);
        return !stopping.load();
      });
    });

    // ---- embedded panel ----
    get(R"(/(.*))", kPublic, [](const httplib::Request& req, httplib::Response& res) {
      const std::string path = req.path == "/" ? "/index.html" : req.path;
      for (size_t i = 0; i < kEncoderPanelAssetCount; ++i) {
        if (path == kEncoderPanelAssets[i].path) {
          res.set_header("Content-Security-Policy",
                         "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'");
          res.set_content(reinterpret_cast<const char*>(kEncoderPanelAssets[i].data), kEncoderPanelAssets[i].size,
                          kEncoderPanelAssets[i].mime);
          return;
        }
      }
      res.status = 404;
      res.set_content("Nie znaleziono", "text/plain; charset=utf-8");
    });
  }
};

EncoderHttpServer::EncoderHttpServer(EncoderEngine& engine, std::string config_path)
    : impl_(std::make_unique<Impl>(engine, std::move(config_path))) {}

EncoderHttpServer::~EncoderHttpServer() { stop(); }

bool EncoderHttpServer::start(const std::string& bind, uint16_t port, std::string* error) {
  auto& m = *impl_;
  if (m.thread.joinable()) {
    if (error) *error = "panel już działa";
    return false;
  }
  if (m.server_used) m.make_server();  // the last attempt failed (or the server was stopped)
  m.server_used = true;
  m.stopping = false;
  const int bound = port == 0 ? m.server->bind_to_any_port(bind) : (m.server->bind_to_port(bind, port) ? port : -1);
  if (bound <= 0) {
    if (error) *error = "nie można otworzyć portu panelu " + bind + ":" + std::to_string(port);
    return false;
  }
  m.port = static_cast<uint16_t>(bound);
  m.listen_returned = false;
  m.thread = std::thread([&m] {
    m.server->listen_after_bind();
    m.listen_returned = true;
  });
  m.wait_listening();  // a stop() right after this start must reach a running server
  log_info("Panel Bounce Encoder dostępny pod http://" + (bind == "0.0.0.0" ? std::string("127.0.0.1") : bind) + ":" +
           std::to_string(m.port));
  return true;
}

void EncoderHttpServer::stop() {
  auto& m = *impl_;
  if (!m.thread.joinable()) return;
  m.stopping = true;
  m.wait_listening();
  m.server->stop();
  m.thread.join();
}

uint16_t EncoderHttpServer::port() const { return impl_->port; }

// ---- startup configuration ----

namespace {
// Where an issue is: "stations[3].streams[1].mount" -> station 3, streams, 1;
// "stations[2].processing.preset" -> station 2, processing.
struct IssuePlace {
  size_t station;
  std::string list;  // "inputs", "streams", "processing" or "" (the station itself)
  size_t item = 0;
};
std::optional<IssuePlace> issue_place(const std::string& field) {
  if (field.rfind("stations[", 0) != 0) return std::nullopt;
  try {
    size_t used = 0;
    IssuePlace p{static_cast<size_t>(std::stoul(field.substr(9), &used)), ""};
    const std::string rest = field.substr(9 + used);  // "].streams[1].mount"
    for (const char* list : {"inputs", "streams"}) {
      const std::string prefix = std::string("].") + list + "[";
      if (rest.rfind(prefix, 0) == 0) {
        p.list = list;
        p.item = static_cast<size_t>(std::stoul(rest.substr(prefix.size())));
      }
    }
    if (rest.rfind("].processing.", 0) == 0) p.list = "processing";
    return p;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void reset_global(EncoderConfig& c, const std::string& field) {
  const EncoderConfig d = default_encoder_config();
  if (field == "interface_ip") c.interface_ip = d.interface_ip;
  if (field == "ptp_domain") c.ptp_domain = d.ptp_domain;
  if (field == "http_bind") {
    // fail closed: a mistyped address must not open the panel on every interface
    c.http_bind = "127.0.0.1";
    log_error("Nieprawidłowy adres panelu w konfiguracji: panel dostępny tylko z tego komputera (127.0.0.1)");
  }
  if (field == "http_port") c.http_port = d.http_port;
  if (field == "stations" && c.stations.size() > static_cast<size_t>(kMaxStations)) c.stations.resize(kMaxStations);
  // a bad default key is dropped by startup_config (after keeping a copy of the file)
  if (field == "stereotool_web_port_base") {
    // the default, or the next hundred when the panel port is among the default's ports
    c.stereotool_web_port_base = d.stereotool_web_port_base;
    if (c.http_port >= c.stereotool_web_port_base && c.http_port < c.stereotool_web_port_base + kMaxStations)
      c.stereotool_web_port_base += 100;
  }
}

// Never silence because of processing: a station whose processing settings are wrong keeps
// running without processing. The value the issue is about is dropped (a preset name or a key of
// the wrong shape; for a missing preset or key the field is empty already); the rest is kept.
// A key is dropped only through `drop_key`, which keeps a copy of the file first (the key may be
// a real licence with a typo) and logs it, never the key itself.
void disable_processing(EncoderConfig& c, size_t station, const std::string& field,
                        const std::function<void(std::string&, const std::string&)>& drop_key) {
  if (station >= c.stations.size()) return;
  StationConfig& st = c.stations[station];
  if (field.ends_with(".processing.preset")) st.processing.preset.clear();
  if (field.ends_with(".processing.license_key") && !st.processing.license_key.empty())
    drop_key(st.processing.license_key, "Klucz licencji Stereo Tool stacji " + station_name(st, station));
  if (!st.processing.enabled) return;
  st.processing.enabled = false;
  log_warning("Stacja " + station_name(st, station) +
              ": processing Stereo Tool wyłączony, stacja działa bez niego: popraw ustawienia processingu w panelu");
}

// load_startup_config without the warnings.
EncoderConfig startup_config(const std::string& path) {
  std::string err;
  bool missing = false;
  EncoderConfig cfg = default_encoder_config();
  if (auto loaded = load_config_file(path, &err, &missing)) {
    cfg = std::move(*loaded);
  } else {
    if (missing) {
      log_info("Brak pliku konfiguracji, tworzę domyślny: " + path);
    } else {
      log_error("Plik konfiguracji uszkodzony (" + err + "), zapisano kopię .bad i użyto ustawień domyślnych");
      platform::replace_file(path, path + ".bad");
    }
    if (!save_config_file(path, cfg, &err)) log_error("Nie można zapisać konfiguracji: " + err);
  }
  // Bounce Encoder 1.0 took ids that differ only in case ("s1", "S1") as two; 1.1 names a folder
  // after each id (and Windows ignores case there), so the later station gets a new id from
  // normalize_config instead of being disabled or left out
  struct Renamed {
    size_t station;
    std::string old_id, other_id;
  };
  std::vector<Renamed> renamed;
  {
    std::map<std::string, std::string> seen;  // id in lower case -> the first station's id
    for (size_t i = 0; i < cfg.stations.size(); ++i) {
      std::string& id = cfg.stations[i].id;
      if (id.empty()) continue;
      if (auto [it, inserted] = seen.emplace(ascii_lower(id), id); !inserted) {
        renamed.push_back({i, id, it->second});
        id.clear();
      }
    }
  }
  for (const auto& n : normalize_config(cfg)) log_info(n);
  for (const Renamed& r : renamed)
    log_warning("Stacja " + station_name(cfg.stations[r.station], r.station) + ": identyfikator „" + r.old_id + "” różni się od „" +
                r.other_id + "” tylko wielkością liter; nowy identyfikator „" + cfg.stations[r.station].id + "”");

  bool backed_up = false;
  // A dropped Stereo Tool key is never lost: config.json is copied to .bad (the same admin-only
  // folder) before the key is removed from the running configuration.
  const auto drop_key = [&](std::string& key, const std::string& what) {
    if (!backed_up) backed_up = platform::copy_file(path, path + ".bad");
    key.clear();
    log_warning(what + " usunięty z konfiguracji (kopia pliku: " + path + ".bad)");
  };
  for (int pass = 0; pass < 2; ++pass) {
    const auto issues = validate_config(cfg);
    if (issues.empty()) return cfg;
    std::set<size_t> bad;
    bool reslot = false;
    for (const auto& i : issues) {
      if (pass == 0) log_warning("Konfiguracja: " + i.message);
      const auto place = issue_place(i.field);
      if (!place) {
        if (i.field == "stereotool_default_key" && !cfg.stereotool_default_key.empty())
          drop_key(cfg.stereotool_default_key, "Domyślny klucz Stereo Tool");
        reset_global(cfg, i.field);
        continue;
      }
      if (i.field.ends_with(".processing.web_port_slot") && place->station < cfg.stations.size()) {
        // a duplicate or out-of-range slot (edited by hand): the station gets a free one below
        if (pass == 1) log_warning("Konfiguracja: " + i.message);
        cfg.stations[place->station].processing.web_port_slot = -1;
        reslot = true;
        continue;
      }
      if (place->list == "processing") {  // also in pass 1: a default key dropped in pass 0 shows here
        if (pass == 1) log_warning("Konfiguracja: " + i.message);
        disable_processing(cfg, place->station, i.field, drop_key);
        continue;
      }
      bad.insert(place->station);
      // the input or stream with the error stays, disabled (disabled ones are checked less)
      if (pass == 0 && place->station < cfg.stations.size()) {
        auto& st = cfg.stations[place->station];
        if (place->list == "inputs" && place->item < st.inputs.size()) st.inputs[place->item].enabled = false;
        if (place->list == "streams" && place->item < st.streams.size()) st.streams[place->item].enabled = false;
      }
    }
    if (reslot) {
      normalize_config(cfg);  // the lowest free slots; every other station keeps its own
      for (size_t k = 0; k < cfg.stations.size(); ++k)
        for (const auto& i : issues)
          if (i.field == "stations[" + std::to_string(k) + "].processing.web_port_slot")
            log_warning("Stacja " + station_name(cfg.stations[k], k) + ": nowy port interfejsu WWW Stereo Tool " +
                        std::to_string(cfg.stereotool_web_port_base + cfg.stations[k].processing.web_port_slot));
    }
    if (pass == 0) {
      // a station with an error does not start; it stays in the panel to be fixed
      for (size_t i : bad) {
        if (i >= cfg.stations.size() || !cfg.stations[i].enabled) continue;
        cfg.stations[i].enabled = false;
        log_warning("Stacja " + station_name(cfg.stations[i], i) + " nie została uruchomiona: popraw jej ustawienia w panelu");
      }
    } else {
      if (!backed_up && !bad.empty()) backed_up = platform::copy_file(path, path + ".bad");  // stations are left out
      for (auto it = bad.rbegin(); it != bad.rend(); ++it) {
        if (*it >= cfg.stations.size()) continue;
        log_error("Stacja " + station_name(cfg.stations[*it], *it) + " pominięta: jej ustawienia są nieprawidłowe (kopia pliku: " +
                  path + ".bad)");
        cfg.stations.erase(cfg.stations.begin() + static_cast<std::ptrdiff_t>(*it));
      }
    }
  }
  if (validate_config(cfg).empty()) return cfg;
  log_error("Konfiguracja nieprawidłowa, użyto ustawień domyślnych (kopia pliku: " + path + ".bad)");
  if (!backed_up) platform::copy_file(path, path + ".bad");
  EncoderConfig fallback = default_encoder_config();
  fallback.http_bind = "127.0.0.1";  // fail closed: the panel only on this computer
  fallback.password_hash = cfg.password_hash;
  return fallback;
}
}  // namespace

EncoderConfig load_startup_config(const std::string& path) {
  EncoderConfig cfg = startup_config(path);
  for (const auto& w : config_warnings(cfg)) log_warning("Konfiguracja: " + w.message);  // never quote a key
  return cfg;
}

namespace testing {
void set_http_clock_offset_ns(int64_t offset_ns) { g_clock_offset_ns = offset_ns; }
void set_login_verified_hook(std::function<void()> hook) {
  std::lock_guard lock(g_hook_mutex);
  g_login_verified_hook = std::move(hook);
}
}  // namespace testing

bool write_panel_file(const std::string& path, const std::string& bind, int port, std::string* error) {
  // the public folder (paths.hpp): created by the installer with its ACL, here only when missing
  const auto sep = path.find_last_of("/\\");
  if (sep != std::string::npos && sep > 0 && !platform::ensure_directory(path.substr(0, sep))) {
    if (error) *error = "nie można utworzyć folderu " + path.substr(0, sep);
    return false;
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << json{{"bind", bind}, {"port", port}}.dump(2) << "\n";
    out.flush();
    if (!out) {
      if (error) *error = "nie można zapisać pliku " + tmp;
      out.close();
      std::remove(tmp.c_str());
      return false;
    }
  }
  if (!platform::replace_file(tmp, path)) {
    if (error) *error = "nie można zastąpić pliku " + path;
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace bam::encoder
