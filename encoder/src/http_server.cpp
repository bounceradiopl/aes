// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder REST API and embedded panel, on the pattern of engine/src/http_server.cpp.
#include "bam/encoder/http_server.hpp"

#include <httplib.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <thread>

#include "bam/auth.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/encoder/version.hpp"
#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "codec_impl.hpp"
#include "login_throttle.hpp"

namespace bam::encoder {

namespace {
using json = nlohmann::ordered_json;

// Not "bam_session" (BounceAES Master): browsers keep cookies per host, not per port, and both
// panels run on the same computer.
constexpr const char* kCookie = "bounce_encoder_session";
constexpr int kHttpThreads = 32;  // every open panel tab holds one thread for its live feed
constexpr int64_t kLiveIntervalNs = 200'000'000;

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, int status, const std::string& message) { send_json(res, {{"error", message}}, status); }

std::string session_cookie(const httplib::Request& req) {
  const std::string cookies = req.get_header_value("Cookie");
  const std::string key = std::string(kCookie) + "=";
  size_t pos = 0;
  while (pos < cookies.size()) {
    while (pos < cookies.size() && (cookies[pos] == ' ' || cookies[pos] == ';')) ++pos;
    if (cookies.compare(pos, key.size(), key) == 0) {
      const size_t end = cookies.find(';', pos);
      return cookies.substr(pos + key.size(), end == std::string::npos ? std::string::npos : end - pos - key.size());
    }
    const size_t next = cookies.find(';', pos);
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  return {};
}

bool is_local(const httplib::Request& req) {
  return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1" || req.remote_addr == "::ffff:127.0.0.1";
}

std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res) {
  try {
    return json::parse(req.body);
  } catch (const std::exception&) {
    send_error(res, 400, "nieprawidłowy JSON w treści żądania");
    return std::nullopt;
  }
}

// A text field of a request body; "" when missing or not text.
std::string text(const json& body, const char* key) {
  if (!body.is_object()) return {};
  const auto it = body.find(key);
  return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
}

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
  const int64_t now = platform::monotonic_ns();
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
                         {"uptime_s", uptime_s(str.started_ns, now)}});
    }
    stations.push_back({{"id", st.id},
                        {"name", st.name},
                        {"enabled", st.enabled},
                        {"running", st.running},
                        {"uptime_s", uptime_s(st.started_ns, now)},
                        {"peak_db", peaks(st.peak_db)},
                        {"skipped_frames", st.skipped_frames},
                        {"inputs", std::move(inputs)},
                        {"streams", std::move(streams)}});
  }
  return {{"product", kEncoderProductName},
          {"version", kEncoderVersion},
          {"running", s.running},
          {"uptime_s", s.uptime_s},
          {"interface_ip", s.interface_ip},
          {"ptp", ptp_json(s)},
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
  detail::LoginThrottle throttle;
  std::mutex change_mutex;  // one configuration change (build, validate, save, apply) at a time
  std::thread thread;
  std::atomic<bool> stopping{false};
  uint16_t port = 0;

  Impl(EncoderEngine& e, std::string path) : engine(e), config_path(std::move(path)) { make_server(); }

  void make_server() {
    server = std::make_unique<httplib::Server>();
    server_used = false;
    server->new_task_queue = [] { return new httplib::ThreadPool(kHttpThreads); };
    routes(*server);
  }

  std::shared_ptr<SessionStore> sessions() {
    std::lock_guard lock(sessions_mutex);
    return sessions_;
  }

  // Every session ends except a new one, returned (its token), for whoever changed the password.
  std::string restart_sessions() {
    auto fresh = std::make_shared<SessionStore>();
    std::string token = fresh->create(platform::monotonic_ns());
    std::lock_guard lock(sessions_mutex);
    sessions_ = std::move(fresh);
    return token;
  }

  void set_session_cookie(httplib::Response& res, const std::string& token, bool clear = false) {
    std::string c = std::string(kCookie) + "=" + (clear ? "" : token) + "; Path=/; HttpOnly; SameSite=Strict";
    if (clear) c += "; Max-Age=0";
    res.set_header("Set-Cookie", c);
  }

  bool authorized(const httplib::Request& req) {
    const std::string token = session_cookie(req);
    return !token.empty() && sessions()->valid(token, platform::monotonic_ns());
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
      send_json(res, {{"ok", false}, {"error", err}, {"issues", json::array({{{"field", ""}, {"message", err}}})}, {"notices", json::array()}}, 400);
      return;
    }
    merge_passwords(*next, prev);
    std::vector<std::string> notices = normalize_config(*next);
    std::vector<EncoderConfigIssue> issues = validate_config(*next);
    if (!issues.empty()) {
      explain_dropped_passwords(issues, *next, prev);
      send_json(res, {{"ok", false}, {"error", issues.front().message}, {"issues", issues_json(issues)}, {"notices", notices}}, 400);
      return;
    }
    if (!store(*next, &err)) {
      log_error("Nie można zapisać konfiguracji: " + err);
      send_json(res, {{"ok", false}, {"error", "nie można zapisać konfiguracji: " + err}, {"issues", json::array()}, {"notices", notices}}, 500);
      return;
    }
    if (next->http_bind != prev.http_bind || next->http_port != prev.http_port)
      notices.push_back("Nowy adres panelu (" + next->http_bind + ":" + std::to_string(next->http_port) +
                        ") zadziała po ponownym uruchomieniu usługi");
    json out = {{"ok", true}, {"issues", json::array()}, {"notices", notices}, {"config", api_config(*next)}};
    if (done) done(out, *next);
    send_json(res, out);
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

  // httplib reads chunked and length-less bodies without any limit, so only bodies with a
  // Content-Length within kMaxApiBodyBytes are accepted, and only as JSON (a form or text/plain
  // POST is what another web site could send cross-origin without a CORS preflight).
  // true: refused (answered), the body is never read into memory. "Connection: close" asks the
  // client to drop the connection; bytes it still sends are parsed line by line as requests (and
  // refused), never buffered as a body.
  static bool refuse_body(const httplib::Request& req, httplib::Response& res) {
    auto refuse = [&](int status, const std::string& message) {
      send_error(res, status, message);
      res.set_header("Connection", "close");
      return true;
    };
    if (req.has_header("Transfer-Encoding"))
      return refuse(411, "treść żądania musi mieć nagłówek Content-Length (przesyłanie w częściach nie jest obsługiwane)");
    const bool with_body = req.method == "POST" || req.method == "PUT" || req.method == "PATCH";
    if (!req.has_header("Content-Length")) {
      if (with_body) return refuse(411, "treść żądania musi mieć nagłówek Content-Length");
      return false;
    }
    const std::string value = req.get_header_value("Content-Length");
    uint64_t length = 0;
    if (value.empty() || value.size() > 19 || value.find_first_not_of("0123456789") != std::string::npos)
      return refuse(400, "nieprawidłowy nagłówek Content-Length");
    length = std::stoull(value);
    if (length > kMaxApiBodyBytes)
      return refuse(413, "treść żądania jest za duża (najwyżej " + std::to_string(kMaxApiBodyBytes / (1024 * 1024)) + " MB)");
    if (length > 0 || req.path == "/api/setup") {
      std::string type = req.get_header_value("Content-Type");
      type = type.substr(0, type.find(';'));
      while (!type.empty() && type.back() == ' ') type.pop_back();
      for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (type != "application/json") return refuse(415, "oczekiwano treści typu application/json");
    }
    return false;
  }

  static std::optional<size_t> find_station(const json& doc, const std::string& id) {
    const json& list = doc["stations"];
    for (size_t i = 0; i < list.size(); ++i)
      if (text(list[i], "id") == id) return i;
    return std::nullopt;
  }

  void routes(httplib::Server& server) {
    server.set_payload_max_length(kMaxApiBodyBytes);  // a second line of defence: checked before routing below
    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
      if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
      if (res.status == 413) {
        send_error(res, 413, "treść żądania jest za duża (najwyżej " + std::to_string(kMaxApiBodyBytes / (1024 * 1024)) + " MB)");
      } else if (res.status == 400) {
        send_error(res, 400, "nieprawidłowe żądanie");
      } else {
        return httplib::Server::HandlerResponse::Unhandled;
      }
      return httplib::Server::HandlerResponse::Handled;
    });

    // Runs on the headers, before any body is read: the body checks, then authorization.
    server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
      res.set_header("X-Frame-Options", "DENY");
      res.set_header("X-Content-Type-Options", "nosniff");
      res.set_header("Cache-Control", "no-store");
      if (refuse_body(req, res)) return httplib::Server::HandlerResponse::Handled;
      const bool open = req.path == "/api/health" || req.path == "/api/login" || req.path == "/api/setup" ||
                        req.path == "/api/auth-state" || req.path.rfind("/api/", 0) != 0;
      if (open || authorized(req)) return httplib::Server::HandlerResponse::Unhandled;
      send_error(res, 401, "wymagane logowanie");
      return httplib::Server::HandlerResponse::Handled;
    });

    // ---- public ----
    server.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) { send_json(res, health_json(engine.status())); });

    server.Get("/api/auth-state", [this](const httplib::Request& req, httplib::Response& res) {
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

    server.Post("/api/setup", [this](const httplib::Request& req, httplib::Response& res) {
      if (!engine.config().password_hash.empty()) return send_error(res, 409, "hasło jest już ustawione");
      if (!is_local(req)) return send_error(res, 403, "pierwsze hasło można ustawić tylko z tego komputera (127.0.0.1)");
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
      set_session_cookie(res, sessions()->create(platform::monotonic_ns()));
      send_json(res, {{"ok", true}});
    });

    server.Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      const std::string stored = engine.config().password_hash;
      if (stored.empty()) return send_error(res, 409, "hasło nie jest jeszcze ustawione");
      // guessing is limited by counting failures (429), not by holding a request thread
      if (!throttle.allowed(req.remote_addr, platform::monotonic_ns()))
        return send_error(res, 429, "zbyt wiele nieudanych prób, spróbuj za chwilę");
      if (!verify_password(text(*body, "password"), stored)) {
        throttle.failed(req.remote_addr, platform::monotonic_ns());
        log_warning("Nieudane logowanie do panelu z " + req.remote_addr);
        return send_error(res, 401, "nieprawidłowe hasło");
      }
      throttle.succeeded(req.remote_addr);
      set_session_cookie(res, sessions()->create(platform::monotonic_ns()));
      send_json(res, {{"ok", true}});
    });

    // ---- authorized ----
    server.Post("/api/logout", [this](const httplib::Request& req, httplib::Response& res) {
      sessions()->remove(session_cookie(req));
      set_session_cookie(res, "", true);
      send_json(res, {{"ok", true}});
    });

    server.Post("/api/password", [this](const httplib::Request& req, httplib::Response& res) {
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

    server.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) { send_json(res, api_config(engine.config())); });

    // The whole configuration or some of its top-level keys ("stations" replaces every station);
    // keys left out keep their value. Stream passwords sent as "" keep the stored ones (E7).
    server.Put("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
      auto body = parse_body(req, res);
      if (!body) return;
      if (!body->is_object()) return send_error(res, 400, "oczekiwano obiektu JSON");
      change(res, [&](json& doc) {
        for (const auto& [key, value] : body->items())
          if (key != "password_hash") doc[key] = value;  // the panel password changes only through /api/password
        return true;
      });
    });

    // A new station (the station object of the configuration); it always gets a new id.
    server.Post("/api/stations", [this](const httplib::Request& req, httplib::Response& res) {
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
    server.Put(R"(/api/stations/([A-Za-z0-9_-]+))", [this](const httplib::Request& req, httplib::Response& res) {
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
            doc["stations"][*i] = *body;
            return true;
          },
          [&](json& out, const EncoderConfig&) { out["id"] = id; });
    });

    server.Delete(R"(/api/stations/([A-Za-z0-9_-]+))", [this](const httplib::Request& req, httplib::Response& res) {
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

    server.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) { send_json(res, status_json(engine.status())); });

    server.Get("/api/codecs", [](const httplib::Request&, httplib::Response& res) { send_json(res, codecs_json()); });

    server.Get("/api/logs", [](const httplib::Request&, httplib::Response& res) {
      json out = json::array();
      for (const auto& e : Logger::instance().recent()) {
        const char* level = e.level == LogLevel::Error ? "error" : e.level == LogLevel::Warning ? "warning" : "info";
        out.push_back({{"time_ms", e.unix_ms}, {"level", level}, {"message", e.message}});
      }
      send_json(res, out);
    });

    server.Get("/api/interfaces", [](const httplib::Request&, httplib::Response& res) {
      json out = json::array();
      for (const auto& i : platform::list_interfaces()) {
        if (i.loopback) continue;
        out.push_back({{"name", i.name}, {"description", i.description}, {"ipv4", i.ipv4}, {"up", i.up}});
      }
      send_json(res, out);
    });

    // Server-sent events: the /api/status document 5 times a second (meters, stream states).
    // The session is checked before every event: logout, expiry or a password change ends it.
    server.Get("/api/live", [this](const httplib::Request& req, httplib::Response& res) {
      res.set_header("X-Accel-Buffering", "no");
      res.set_chunked_content_provider("text/event-stream", [this, token = session_cookie(req)](size_t, httplib::DataSink& sink) {
        if (stopping || !sessions()->valid(token, platform::monotonic_ns())) return false;
        const std::string msg = "event: status\ndata: " + status_json(engine.status()).dump() + "\n\n";
        if (!sink.write(msg.data(), msg.size())) return false;
        platform::sleep_ns(kLiveIntervalNs);
        return !stopping.load();
      });
    });

    // ---- embedded panel ----
    server.Get(R"(/(.*))", [](const httplib::Request& req, httplib::Response& res) {
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
  m.thread = std::thread([&m] { m.server->listen_after_bind(); });
  log_info("Panel Bounce Encoder dostępny pod http://" + (bind == "0.0.0.0" ? std::string("127.0.0.1") : bind) + ":" +
           std::to_string(m.port));
  return true;
}

void EncoderHttpServer::stop() {
  auto& m = *impl_;
  if (!m.thread.joinable()) return;
  m.stopping = true;
  m.server->stop();
  m.thread.join();
}

uint16_t EncoderHttpServer::port() const { return impl_->port; }

// ---- startup configuration ----

namespace {
// Where an issue is: "stations[3].streams[1].mount" -> station 3, streams, 1.
struct IssuePlace {
  size_t station;
  std::string list;  // "inputs", "streams" or "" (the station itself)
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
}

std::string station_name(const StationConfig& st, size_t i) {
  return st.name.empty() ? "nr " + std::to_string(i + 1) : "„" + st.name + "”";
}
}  // namespace

EncoderConfig load_startup_config(const std::string& path) {
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
  for (const auto& n : normalize_config(cfg)) log_info(n);

  bool backed_up = false;
  for (int pass = 0; pass < 2; ++pass) {
    const auto issues = validate_config(cfg);
    if (issues.empty()) return cfg;
    std::set<size_t> bad;
    for (const auto& i : issues) {
      if (pass == 0) log_warning("Konfiguracja: " + i.message);
      const auto place = issue_place(i.field);
      if (!place) {
        reset_global(cfg, i.field);
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
    if (pass == 0) {
      // a station with an error does not start; it stays in the panel to be fixed
      for (size_t i : bad) {
        if (i >= cfg.stations.size() || !cfg.stations[i].enabled) continue;
        cfg.stations[i].enabled = false;
        log_warning("Stacja " + station_name(cfg.stations[i], i) + " nie została uruchomiona: popraw jej ustawienia w panelu");
      }
    } else {
      if (!backed_up) backed_up = platform::copy_file(path, path + ".bad");
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
  fallback.password_hash = cfg.password_hash;
  return fallback;
}

bool write_panel_file(const std::string& path, const std::string& bind, int port, std::string* error) {
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
