// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder configuration: JSON, validation, normalisation, passwords, the file.
#include "bam/encoder/config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <io.h>  // _commit, _fileno
#else
#include <unistd.h>  // fsync, fileno
#endif

#include "bam/config.hpp"  // min_rx_latency_us, kMinRxLatencyUs, kMaxRxLatencyUs
#include "bam/encoder/dsp.hpp"
#include "bam/ipv4.hpp"
#include "bam/platform/process.hpp"
#include "bam/sdp.hpp"

namespace bam::encoder {

static_assert(kMaxInputsPerStation == kStationInputs, "the station mixer sums kStationInputs inputs");

namespace {
using json = nlohmann::ordered_json;

constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxIdChars = 32;
constexpr size_t kMaxHostBytes = 253;
constexpr size_t kMaxFieldBytes = 255;  // mount, user, password
constexpr size_t kMaxSdpBytes = 64 * 1024;
// config_from_json refuses arrays longer than 4x the limit (an API body could otherwise make
// normalize_config work through thousands of entries); validate_config explains smaller overshoots.
constexpr size_t kParseLimitFactor = 4;
constexpr const char* kStationIdPrefix = "s";
constexpr const char* kStreamIdPrefix = "str";

// ---- labels for the messages ----

std::string station_label(const StationConfig& s, size_t i) {  // "Stacja „Radio Bounce”" / "Stacja 3"
  return s.name.empty() ? "Stacja " + std::to_string(i + 1) : "Stacja „" + s.name + "”";
}
std::string station_ref(const StationConfig& s, size_t i) {  // "stację „Radio Bounce”" (accusative)
  return s.name.empty() ? "stację " + std::to_string(i + 1) : "stację „" + s.name + "”";
}
std::string station_mention(const StationConfig& s, size_t i) {  // "stacja „Radio Bounce”"
  return s.name.empty() ? "stacja " + std::to_string(i + 1) : "stacja „" + s.name + "”";
}
std::string input_prefix(const StationConfig& s, size_t i, size_t j) {
  return station_label(s, i) + ", wejście " + std::to_string(j + 1) + ": ";
}
std::string stream_prefix(const StationConfig& s, size_t i, size_t k) {
  return station_label(s, i) + ", strumień " + std::to_string(k + 1) + ": ";
}
std::string station_path(size_t i) { return "stations[" + std::to_string(i) + "]"; }
std::string input_path(size_t i, size_t j) { return station_path(i) + ".inputs[" + std::to_string(j) + "]"; }
std::string stream_path(size_t i, size_t k) { return station_path(i) + ".streams[" + std::to_string(k) + "]"; }

// ---- small checks ----

bool has_control(std::string_view s) {
  for (unsigned char ch : s)
    if (ch < 0x20 || ch == 0x7f) return true;
  return false;
}

bool valid_id(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdChars) return false;
  for (char ch : id) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!ok) return false;
  }
  return true;
}

// A host name (ASCII letters, digits, '.', '-', '_') or a dotted IPv4 address.
bool valid_host(std::string_view h) {
  if (h.empty()) return false;
  for (char ch : h) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' ||
                    ch == '-' || ch == '_';
    if (!ok) return false;
  }
  return true;
}

// Text taken from an SDP for a name field: no control characters, at most kMaxNameBytes,
// cut on a whole UTF-8 character.
std::string sanitize_name(std::string_view in) {
  std::string out;
  for (unsigned char ch : in)
    if (ch >= 0x20 && ch != 0x7f) out += static_cast<char>(ch);
  if (out.size() > kMaxNameBytes) {
    size_t n = kMaxNameBytes;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  return out;
}

bool valid_mount(std::string_view m) {
  if (m.size() < 2 || m[0] != '/') return false;
  for (unsigned char ch : m)
    if (ch <= 0x20 || ch == 0x7f || ch == '?' || ch == '#') return false;
  return true;
}

std::string lower(std::string s) {
  for (auto& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return s;
}

bool same_server(const StreamConfig& a, const StreamConfig& b) { return a.port == b.port && lower(a.host) == lower(b.host); }

bool in_gain_range(double db) { return db >= kGainMinDb && db <= kGainMaxDb; }

double round_to(double v, double step) { return std::round(v / step) * step; }

// The smallest "<prefix><n>" with n >= from not in `used`; it is added to `used` and `from` moves
// past it, so assigning k ids costs O((k + |used|) log |used|) in total, not a rescan from 1 each.
std::string next_id(const char* prefix, std::set<std::string>& used, int& from) {
  for (;; ++from) {
    std::string id = prefix + std::to_string(from);
    if (used.insert(id).second) {
      ++from;
      return id;
    }
  }
}

// ---- JSON ----

[[noreturn]] void bad_field(const std::string& path, const char* what) {
  throw std::runtime_error("pole " + path + ": " + what);
}

// Reads j[key] into out when present; a value of the wrong type or out of the range of T is an
// error naming the field (`path` is the JSON path of j, with a trailing '.' or empty).
template <typename T>
void read_field(const json& j, const char* key, T& out, const std::string& path) {
  if (!j.contains(key)) return;
  const json& v = j.at(key);
  const std::string where = path + key;
  if constexpr (std::is_same_v<T, bool>) {
    if (!v.is_boolean()) bad_field(where, "oczekiwano true lub false");
    out = v.get<bool>();
  } else if constexpr (std::is_integral_v<T>) {
    if (!v.is_number_integer()) bad_field(where, "oczekiwano liczby całkowitej");
    if (v.is_number_unsigned()) {
      const uint64_t u = v.get<uint64_t>();
      if (u > static_cast<uint64_t>(std::numeric_limits<T>::max())) bad_field(where, "wartość poza zakresem");
      out = static_cast<T>(u);
    } else {
      const int64_t s = v.get<int64_t>();
      if constexpr (std::is_unsigned_v<T>) {
        if (s < 0) bad_field(where, "wartość nie może być ujemna");
      } else {
        if (s < static_cast<int64_t>(std::numeric_limits<T>::min())) bad_field(where, "wartość poza zakresem");
      }
      if (s > static_cast<int64_t>(std::numeric_limits<T>::max())) bad_field(where, "wartość poza zakresem");
      out = static_cast<T>(s);
    }
  } else if constexpr (std::is_floating_point_v<T>) {
    if (!v.is_number()) bad_field(where, "oczekiwano liczby");
    out = v.get<T>();
  } else {
    if (!v.is_string()) bad_field(where, "oczekiwano tekstu");
    out = v.get<std::string>();
  }
}

// Calls fn(index, element) for each element of j[key] when present (must be an array of at most
// max_items objects).
template <typename Fn>
void read_array(const json& j, const char* key, const std::string& path, size_t max_items, Fn&& fn) {
  if (!j.contains(key)) return;
  const json& arr = j.at(key);
  if (!arr.is_array()) bad_field(path + key, "oczekiwano tablicy");
  if (arr.size() > max_items)
    throw std::runtime_error("pole " + path + key + ": za dużo elementów (jest " + std::to_string(arr.size()) +
                             ", dopuszczalne najwyżej " + std::to_string(max_items) + ")");
  for (size_t i = 0; i < arr.size(); ++i) {
    if (!arr[i].is_object()) bad_field(path + key + "[" + std::to_string(i) + "]", "oczekiwano obiektu");
    fn(i, arr[i]);
  }
}

json to_json(const EncoderConfig& c, bool api) {
  json j;
  j["version"] = kEncoderConfigVersion;
  j["interface_ip"] = c.interface_ip;
  j["ptp_domain"] = c.ptp_domain;
  j["http_bind"] = c.http_bind;
  j["http_port"] = c.http_port;
  if (!api) j["password_hash"] = c.password_hash;
  json stations = json::array();
  for (const auto& st : c.stations) {
    json inputs = json::array(), streams = json::array();
    for (const auto& in : st.inputs)
      inputs.push_back({{"enabled", in.enabled},
                        {"name", in.name},
                        {"sdp", in.sdp},
                        {"gain_db", in.gain_db},
                        {"delay_s", in.delay_s},
                        {"latency_us", in.latency_us}});
    for (const auto& s : st.streams) {
      json sj = {{"id", s.id},
                 {"enabled", s.enabled},
                 {"codec", codec_name(s.codec)},
                 {"bitrate_kbps", s.bitrate_kbps},
                 {"sample_rate", s.sample_rate},
                 {"gain_db", s.gain_db},
                 {"host", s.host},
                 {"port", s.port},
                 {"mount", s.mount},
                 {"user", s.user},
                 {"password", api ? std::string() : s.password}};
      if (api) sj["password_set"] = !s.password.empty();
      streams.push_back(std::move(sj));
    }
    stations.push_back({{"id", st.id},
                        {"name", st.name},
                        {"enabled", st.enabled},
                        {"inputs", std::move(inputs)},
                        {"streams", std::move(streams)}});
  }
  j["stations"] = std::move(stations);
  return j;
}

}  // namespace

EncoderConfig default_encoder_config() { return EncoderConfig{}; }

std::string config_to_json(const EncoderConfig& c) { return to_json(c, false).dump(2); }

std::string config_to_api_json(const EncoderConfig& c) { return to_json(c, true).dump(2); }

std::optional<EncoderConfig> config_from_json(std::string_view text, std::string* error) {
  try {
    const json j = json::parse(text);
    if (!j.is_object()) throw std::runtime_error("oczekiwano obiektu JSON");
    int version = kEncoderConfigVersion;
    read_field(j, "version", version, "");
    if (version > kEncoderConfigVersion) throw std::runtime_error("plik z nowszej wersji programu");
    if (version < 1) throw std::runtime_error("pole version: nieprawidłowa wersja pliku " + std::to_string(version));

    EncoderConfig c = default_encoder_config();
    read_field(j, "interface_ip", c.interface_ip, "");
    read_field(j, "ptp_domain", c.ptp_domain, "");
    read_field(j, "http_bind", c.http_bind, "");
    read_field(j, "http_port", c.http_port, "");
    read_field(j, "password_hash", c.password_hash, "");
    read_array(j, "stations", "", kMaxStations * kParseLimitFactor, [&](size_t i, const json& sj) {
      StationConfig st;
      const std::string sp = station_path(i) + ".";
      read_field(sj, "id", st.id, sp);
      read_field(sj, "name", st.name, sp);
      read_field(sj, "enabled", st.enabled, sp);
      read_array(sj, "inputs", sp, kMaxInputsPerStation * kParseLimitFactor, [&](size_t k, const json& ij) {
        InputConfig in;
        const std::string ip = input_path(i, k) + ".";
        read_field(ij, "enabled", in.enabled, ip);
        read_field(ij, "name", in.name, ip);
        read_field(ij, "sdp", in.sdp, ip);
        read_field(ij, "gain_db", in.gain_db, ip);
        read_field(ij, "delay_s", in.delay_s, ip);
        read_field(ij, "latency_us", in.latency_us, ip);
        st.inputs.push_back(std::move(in));
      });
      read_array(sj, "streams", sp, kMaxStreamsPerStation * kParseLimitFactor, [&](size_t k, const json& tj) {
        StreamConfig s;
        const std::string tp = stream_path(i, k) + ".";
        read_field(tj, "id", s.id, tp);
        read_field(tj, "enabled", s.enabled, tp);
        if (tj.contains("codec")) {
          std::string name;
          read_field(tj, "codec", name, tp);
          auto codec = parse_codec(name);
          if (!codec) {
            std::string allowed;
            for (Codec known : kCodecs) allowed += std::string(allowed.empty() ? "" : ", ") + codec_name(known);
            throw std::runtime_error(stream_prefix(st, i, k) + "nieznany kodek „" + name + "” (dozwolone: " + allowed + ")");
          }
          s.codec = *codec;
        }
        read_field(tj, "bitrate_kbps", s.bitrate_kbps, tp);
        read_field(tj, "sample_rate", s.sample_rate, tp);
        read_field(tj, "gain_db", s.gain_db, tp);
        read_field(tj, "host", s.host, tp);
        read_field(tj, "port", s.port, tp);
        read_field(tj, "mount", s.mount, tp);
        read_field(tj, "user", s.user, tp);
        read_field(tj, "password", s.password, tp);
        st.streams.push_back(std::move(s));
      });
      c.stations.push_back(std::move(st));
    });
    return c;
  } catch (const std::exception& e) {
    if (error) *error = std::string("nieprawidłowa konfiguracja: ") + e.what();
    return std::nullopt;
  }
}

std::vector<EncoderConfigIssue> validate_config(const EncoderConfig& c) {
  std::vector<EncoderConfigIssue> issues;
  auto add = [&](std::string field, std::string msg) { issues.push_back({std::move(field), std::move(msg)}); };

  if (!c.interface_ip.empty() && !parse_ipv4(c.interface_ip))
    add("interface_ip", "Nieprawidłowy adres karty sieciowej: „" + c.interface_ip + "”");
  if (c.ptp_domain < 0 || c.ptp_domain > 127) add("ptp_domain", "Domena PTP musi być w zakresie 0–127");
  if (!parse_ipv4(c.http_bind)) add("http_bind", "Nieprawidłowy adres panelu: „" + c.http_bind + "”");
  if (c.http_port < 1 || c.http_port > 65535) add("http_port", "Port panelu musi być w zakresie 1–65535");
  if (c.stations.size() > static_cast<size_t>(kMaxStations))
    add("stations", "Najwyżej " + std::to_string(kMaxStations) + " stacji (jest " + std::to_string(c.stations.size()) + ")");

  std::map<std::string, size_t> station_ids;                       // id -> station index
  std::map<std::string, std::pair<size_t, size_t>> targets;        // server + mount -> (station, stream)
  for (size_t i = 0; i < c.stations.size(); ++i) {
    const auto& st = c.stations[i];
    const std::string sp = station_path(i);
    const std::string label = station_label(st, i) + ": ";

    if (st.id.empty()) {
      add(sp + ".id", label + "brak identyfikatora");
    } else if (!valid_id(st.id)) {
      add(sp + ".id", label + "nieprawidłowy identyfikator „" + st.id + "” (dozwolone litery, cyfry, „-” i „_”, najwyżej 32 znaki)");
    } else if (auto [it, inserted] = station_ids.emplace(st.id, i); !inserted) {
      add(sp + ".id", label + "identyfikator „" + st.id + "” jest już używany przez " + station_ref(c.stations[it->second], it->second));
    }
    if (st.name.size() > kMaxNameBytes) add(sp + ".name", label + "nazwa może mieć najwyżej 64 bajty");
    if (has_control(st.name)) add(sp + ".name", label + "nazwa nie może zawierać znaków sterujących");
    if (st.inputs.size() > static_cast<size_t>(kMaxInputsPerStation))
      add(sp + ".inputs", label + "najwyżej " + std::to_string(kMaxInputsPerStation) + " wejścia (jest " +
                              std::to_string(st.inputs.size()) + ")");
    if (st.streams.size() > static_cast<size_t>(kMaxStreamsPerStation))
      add(sp + ".streams", label + "najwyżej " + std::to_string(kMaxStreamsPerStation) + " strumieni (jest " +
                               std::to_string(st.streams.size()) + ")");

    // group:port -> (sender filter, input index) of the inputs on it: one source received twice
    // would be mixed twice (+6 dB). An input without a filter receives every sender on its group
    // and port, so it duplicates any input there; two different filters are two senders.
    std::map<std::string, std::vector<std::pair<std::string, size_t>>> sources;
    for (size_t j = 0; j < st.inputs.size(); ++j) {
      const auto& in = st.inputs[j];
      const std::string f = input_path(i, j) + ".";
      const std::string p = input_prefix(st, i, j);
      if (in.name.size() > kMaxNameBytes) add(f + "name", p + "nazwa może mieć najwyżej 64 bajty");
      if (has_control(in.name)) add(f + "name", p + "nazwa nie może zawierać znaków sterujących");
      if (!in_gain_range(in.gain_db)) add(f + "gain_db", p + "gain musi być w zakresie −60…+12 dB");
      if (!(in.delay_s >= 0 && in.delay_s <= kMaxDelaySeconds)) add(f + "delay_s", p + "opóźnienie musi być w zakresie 0–60 s");
      const bool latency_in_range = in.latency_us >= kMinRxLatencyUs && in.latency_us <= kMaxRxLatencyUs;
      if (!latency_in_range) add(f + "latency_us", p + "opóźnienie odbioru musi być w zakresie 1–1000 ms");
      if (in.sdp.size() > kMaxSdpBytes) {
        add(f + "sdp", p + "SDP może mieć najwyżej 64 KB");
        continue;
      }
      if (!in.enabled) continue;
      if (in.sdp.empty()) {
        add(f + "sdp", p + "wejście wymaga SDP źródła");
        continue;
      }
      const auto r = parse_sdp(in.sdp);
      if (!r.stream) {
        add(f + "sdp", p + "SDP: " + r.error);
        continue;
      }
      auto& on_group = sources[lower(r.stream->multicast_address) + ":" + std::to_string(r.stream->port)];
      const std::string filter = lower(r.stream->source_filter);
      const auto same = std::find_if(on_group.begin(), on_group.end(), [&](const auto& other) {
        return other.first == filter || other.first.empty() || filter.empty();
      });
      if (same != on_group.end()) {
        add(f + "sdp", p + "ten sam strumień źródła (" + r.stream->multicast_address + ":" + std::to_string(r.stream->port) +
                           ") co wejście " + std::to_string(same->second + 1) +
                           " tej stacji: dźwięk byłby zmiksowany dwa razy (+6 dB); wyłącz lub usuń jedno z wejść");
      } else {
        on_group.emplace_back(filter, j);
      }
      if (latency_in_range && in.latency_us < min_rx_latency_us(*r.stream)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.3f ms", min_rx_latency_us(*r.stream) / 1000.0);
        add(f + "latency_us", p + "opóźnienie odbioru musi być co najmniej ptime źródła + 1 ms (" + buf + ")");
      }
    }

    std::map<std::string, size_t> stream_ids;  // id -> stream index
    for (size_t k = 0; k < st.streams.size(); ++k) {
      const auto& s = st.streams[k];
      const std::string f = stream_path(i, k) + ".";
      const std::string p = stream_prefix(st, i, k);
      if (s.id.empty()) {
        add(f + "id", p + "brak identyfikatora");
      } else if (!valid_id(s.id)) {
        add(f + "id", p + "nieprawidłowy identyfikator „" + s.id + "” (dozwolone litery, cyfry, „-” i „_”, najwyżej 32 znaki)");
      } else if (auto [it, inserted] = stream_ids.emplace(s.id, k); !inserted) {
        add(f + "id", p + "identyfikator „" + s.id + "” jest już używany przez strumień " + std::to_string(it->second + 1));
      }
      if (!in_gain_range(s.gain_db)) add(f + "gain_db", p + "gain musi być w zakresie −60…+12 dB");
      const bool host_short = s.host.size() <= kMaxHostBytes;
      if (!host_short) add(f + "host", p + "nazwa serwera może mieć najwyżej 253 znaki");
      const bool mount_short = s.mount.size() <= kMaxFieldBytes;
      if (!mount_short) add(f + "mount", p + "mountpoint może mieć najwyżej 255 bajtów");
      if (s.user.size() > kMaxFieldBytes) add(f + "user", p + "użytkownik może mieć najwyżej 255 bajtów");
      if (s.password.size() > kMaxFieldBytes) add(f + "password", p + "hasło może mieć najwyżej 255 bajtów");
      if (!s.enabled) continue;

      std::string codec_error;
      if (!validate_encoder_settings({s.codec, s.bitrate_kbps, s.sample_rate, 2}, &codec_error))
        add(stream_path(i, k), p + codec_error);
      const bool host_ok = host_short && valid_host(s.host);
      if (s.host.empty())
        add(f + "host", p + "podaj serwer Icecast (nazwę lub adres IP)");
      else if (host_short && s.host.find(':') != std::string::npos)
        add(f + "host", p + "nieprawidłowy serwer „" + s.host + "”: port wpisz w osobnym polu");
      else if (host_short && !host_ok)
        add(f + "host", p + "nieprawidłowy serwer „" + s.host + "” (nazwa z liter, cyfr, „.”, „-” i „_” albo adres IP, bez ścieżki)");
      const bool port_ok = s.port >= 1 && s.port <= 65535;
      if (!port_ok) add(f + "port", p + "port musi być w zakresie 1–65535");
      const bool mount_ok = mount_short && valid_mount(s.mount);
      if (mount_short && !mount_ok)
        add(f + "mount", p + "mountpoint musi zaczynać się od „/”, mieć nazwę i nie zawierać spacji, „?” ani „#” (np. /radio.mp3)");
      if (s.user.find(':') != std::string::npos || has_control(s.user))
        add(f + "user", p + "użytkownik nie może zawierać znaku „:” ani znaków sterujących");
      if (s.password.empty()) add(f + "password", p + "podaj hasło do serwera Icecast");

      // two streams that run at the same time cannot feed one mountpoint
      if (st.enabled && host_ok && port_ok && mount_ok) {
        const std::string key = lower(s.host) + ":" + std::to_string(s.port) + s.mount;
        auto [it, inserted] = targets.emplace(key, std::make_pair(i, k));
        if (!inserted) {
          const auto [other_station, other_stream] = it->second;
          add(f + "mount", p + "ten sam serwer i mountpoint co " + station_mention(c.stations[other_station], other_station) +
                               ", strumień " + std::to_string(other_stream + 1));
        }
      }
    }
  }
  return issues;
}

std::vector<std::string> normalize_config(EncoderConfig& c) {
  std::vector<std::string> notices;
  std::set<std::string> station_ids;
  for (const auto& st : c.stations)
    if (!st.id.empty()) station_ids.insert(st.id);
  int next_station = 1;

  for (size_t i = 0; i < c.stations.size(); ++i) {
    auto& st = c.stations[i];
    if (st.id.empty()) st.id = next_id(kStationIdPrefix, station_ids, next_station);

    for (size_t j = 0; j < st.inputs.size(); ++j) {
      auto& in = st.inputs[j];
      in.gain_db = round_to(in.gain_db, 0.5);
      in.delay_s = std::round(in.delay_s * 10.0) / 10.0;  // 0.1 s steps (dividing keeps e.g. 2.3 exact)
      if (in.sdp.empty() || in.sdp.size() > kMaxSdpBytes) continue;  // validate_config reports an oversized SDP
      const auto r = parse_sdp(in.sdp);
      if (!r.stream) continue;
      if (in.name.empty()) in.name = sanitize_name(r.stream->session_name);
      const uint32_t min = min_rx_latency_us(*r.stream);
      if (in.latency_us < min) {
        in.latency_us = min;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "opóźnienie odbioru podniesione do %.3f ms (ptime źródła %.3f ms + 1 ms)", min / 1000.0,
                      r.stream->ptime_ms);
        notices.push_back(input_prefix(st, i, j) + buf);
      }
    }

    std::set<std::string> stream_ids;
    for (const auto& s : st.streams)
      if (!s.id.empty()) stream_ids.insert(s.id);
    int next_stream = 1;
    for (auto& s : st.streams) {
      if (s.id.empty()) s.id = next_id(kStreamIdPrefix, stream_ids, next_stream);
      s.gain_db = round_to(s.gain_db, 0.5);
    }
  }
  return notices;
}

EncoderConfig mask_passwords(const EncoderConfig& c) {
  EncoderConfig m = c;
  m.password_hash.clear();
  for (auto& st : m.stations)
    for (auto& s : st.streams) s.password.clear();
  return m;
}

void merge_passwords(EncoderConfig& next, const EncoderConfig& previous) {
  next.password_hash = previous.password_hash;
  for (auto& st : next.stations) {
    if (st.id.empty()) continue;
    const StationConfig* old_station = nullptr;
    for (const auto& o : previous.stations)
      if (o.id == st.id) {
        old_station = &o;
        break;
      }
    if (!old_station) continue;
    for (auto& s : st.streams) {
      if (!s.password.empty() || s.id.empty()) continue;
      for (const auto& o : old_station->streams)
        if (o.id == s.id) {
          if (same_server(s, o)) s.password = o.password;
          break;
        }
    }
  }
}

std::optional<EncoderConfig> load_config_file(const std::string& path, std::string* error, bool* missing) {
  if (missing) *missing = false;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (errno == ENOENT) {
      if (missing) *missing = true;
    } else if (error) {
      *error = "nie można odczytać " + path;
    }
    return std::nullopt;
  }
  std::string text;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  const bool read_error = std::ferror(f) != 0;
  std::fclose(f);
  if (read_error) {
    if (error) *error = "błąd odczytu " + path;
    return std::nullopt;
  }
  return config_from_json(text, error);
}

bool save_config_file(const std::string& path, const EncoderConfig& c, std::string* error) {
  // create the directory and any missing parents (results are checked by the write below)
  const auto last = path.find_last_of("/\\");
  if (last != std::string::npos && last > 0) {
    const std::string dir = path.substr(0, last);
    for (size_t pos = dir.find_first_of("/\\", 1); pos != std::string::npos; pos = dir.find_first_of("/\\", pos + 1))
      platform::ensure_directory(dir.substr(0, pos));
    platform::ensure_directory(dir);
  }
  const std::string tmp = path + ".tmp";
  auto fail = [&](std::string message) {
    std::remove(tmp.c_str());  // never leave a partial file behind
    if (error) *error = std::move(message);
    return false;
  };
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {  // nothing was created, so nothing is removed (tmp may name something that is not ours)
    if (error) *error = "nie można zapisać " + tmp;
    return false;
  }
  const std::string text = config_to_json(c);
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
  // on disk before the rename, so a power cut leaves the old or the new file, never an empty one
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  ok = std::fclose(f) == 0 && ok;
  if (!ok) return fail("błąd zapisu " + tmp);
  if (!platform::replace_file(tmp, path)) return fail("nie można zastąpić " + path);
  return true;
}

}  // namespace bam::encoder
