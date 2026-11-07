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
// Station numbers ("s<n>") stay below this: validate_config refuses an id past it (given by hand,
// or the one numbering would give after the largest), so the counter, kept at most here, never
// has to come back below a number it gave (ruling S4) and cannot overflow.
constexpr int64_t kMaxStationNumber = 100'000'000'000'000'000LL;  // 10^17
constexpr const char* kStreamIdPrefix = "str";

// n of a station id "s<n>" (any case, leading zeros allowed); kMaxStationNumber when n is at least
// that; 0 when the id is not of that form.
int64_t station_number(std::string_view id) {
  if (id.size() < 2 || (id[0] != 's' && id[0] != 'S')) return 0;
  int64_t n = 0;
  for (char ch : id.substr(1)) {
    if (ch < '0' || ch > '9') return 0;
    if (n < kMaxStationNumber) n = n * 10 + (ch - '0');  // past it the digits only need checking
  }
  return std::min(n, kMaxStationNumber);
}

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

std::string lower(std::string s) { return ascii_lower(s); }

bool same_server(const StreamConfig& a, const StreamConfig& b) { return a.port == b.port && lower(a.host) == lower(b.host); }

bool is_blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

std::string trim_blanks(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && is_blank(s[b])) ++b;
  while (e > b && is_blank(s[e - 1])) --e;
  return std::string(s.substr(b, e - b));
}

// Why a non-empty Stereo Tool key has the wrong shape, or "" when it is fine. Never mentions
// the key itself.
std::string key_shape_error(std::string_view key) {
  if (key.empty()) return {};
  if (key.size() > kMaxStereoToolKeyChars) return "może mieć najwyżej " + std::to_string(kMaxStereoToolKeyChars) + " znaków";
  for (unsigned char ch : key)
    if (ch < 0x20 || ch > 0x7e) return "może zawierać tylko drukowalne znaki ASCII (bez polskich liter i znaków sterujących)";
  if (key.size() < 2 || key.front() != '<' || key.back() != '>')
    return "musi zaczynać się znakiem „<” i kończyć znakiem „>” (wklej cały klucz z nawiasami)";
  return {};
}

bool is_alnum(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); }

bool in_gain_range(double db) { return db >= kGainMinDb && db <= kGainMaxDb; }

double round_to(double v, double step) { return std::round(v / step) * step; }

// The smallest "<prefix><n>" with n >= from not in `used`; it is added to `used` and `from` moves
// past it, so assigning k ids costs O((k + |used|) log |used|) in total, not a rescan from 1 each.
std::string next_id(const char* prefix, std::set<std::string>& used, int64_t& from) {
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

// Calls fn(object) for j[key] when present (must be an object).
template <typename Fn>
void read_object(const json& j, const char* key, const std::string& path, Fn&& fn) {
  if (!j.contains(key)) return;
  const json& obj = j.at(key);
  if (!obj.is_object()) bad_field(path + key, "oczekiwano obiektu");
  fn(obj);
}

json to_json(const EncoderConfig& c, bool api) {
  json j;
  j["version"] = kEncoderConfigVersion;
  j["interface_ip"] = c.interface_ip;
  j["ptp_domain"] = c.ptp_domain;
  j["http_bind"] = c.http_bind;
  j["http_port"] = c.http_port;
  if (!api) j["password_hash"] = c.password_hash;
  j["stereotool_default_key"] = api ? std::string() : c.stereotool_default_key;
  if (api) j["stereotool_default_key_set"] = !c.stereotool_default_key.empty();
  j["stereotool_web_port_base"] = c.stereotool_web_port_base;
  if (!api) j["next_station_number"] = c.next_station_number;
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
    json processing = {{"enabled", st.processing.enabled},
                       {"license_key", api ? std::string() : st.processing.license_key},
                       {"preset", st.processing.preset},
                       {"web_port_slot", st.processing.web_port_slot}};
    if (api) processing["license_key_set"] = !st.processing.license_key.empty();
    stations.push_back({{"id", st.id},
                        {"name", st.name},
                        {"enabled", st.enabled},
                        {"inputs", std::move(inputs)},
                        {"streams", std::move(streams)},
                        {"processing", std::move(processing)}});
  }
  j["stations"] = std::move(stations);
  return j;
}

}  // namespace

std::string ascii_lower(std::string s) {
  for (char& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
  return s;
}

EncoderConfig default_encoder_config() { return EncoderConfig{}; }

bool valid_preset_name(std::string_view name) {
  constexpr std::string_view ext = ".sts";
  if (name.size() <= ext.size() || name.size() > kMaxPresetNameChars || name.front() == ' ') return false;
  for (char ch : name) {
    const bool ok = is_alnum(ch) || ch == ' ' || ch == '_' || ch == '.' || ch == '-';
    if (!ok) return false;
  }
  if (lower(std::string(name.substr(name.size() - ext.size()))) != ext) return false;
  const std::string_view before = name.substr(0, name.size() - ext.size());
  if (std::none_of(before.begin(), before.end(), is_alnum)) return false;  // " .sts", "..sts", "-.sts"
  // Windows device names stay devices whatever the extension ("CON.sts" opens the console)
  const std::string stem = lower(trim_blanks(name.substr(0, name.find('.'))));
  for (const char* device : {"con", "prn", "aux", "nul"})
    if (stem == device) return false;
  if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '0' && stem[3] <= '9') return false;
  return true;
}

const std::string& effective_stereotool_key(const EncoderConfig& c, const StationConfig& st) {
  return st.processing.license_key.empty() ? c.stereotool_default_key : st.processing.license_key;
}

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
    read_field(j, "stereotool_default_key", c.stereotool_default_key, "");
    read_field(j, "stereotool_default_key_clear", c.stereotool_default_key_clear, "");
    read_field(j, "stereotool_web_port_base", c.stereotool_web_port_base, "");
    read_field(j, "next_station_number", c.next_station_number, "");
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
      read_object(sj, "processing", sp, [&](const json& pj) {
        const std::string pp = sp + "processing.";
        read_field(pj, "enabled", st.processing.enabled, pp);
        read_field(pj, "license_key", st.processing.license_key, pp);
        read_field(pj, "preset", st.processing.preset, pp);
        read_field(pj, "license_key_clear", st.processing.license_key_clear, pp);
        read_field(pj, "web_port_slot", st.processing.web_port_slot, pp);
      });
      c.stations.push_back(std::move(st));
    });
    return c;
  } catch (const json::parse_error& e) {
    // nlohmann ends the message with "; last read: '<text>'", which may be part of a password or
    // a Stereo Tool key: only the position and the reason are passed on
    std::string what = e.what();
    if (const auto cut = what.find("; last read"); cut != std::string::npos) what.resize(cut);
    if (error) *error = "nieprawidłowa konfiguracja: " + what;
    return std::nullopt;
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
  const bool http_port_ok = c.http_port >= 1 && c.http_port <= 65535;
  if (!http_port_ok) add("http_port", "Port panelu musi być w zakresie 1–65535");
  if (c.stereotool_web_port_base < kMinStereoToolWebPortBase || c.stereotool_web_port_base > kMaxStereoToolWebPortBase) {
    add("stereotool_web_port_base", "Port bazowy interfejsu WWW Stereo Tool musi być w zakresie " +
                                        std::to_string(kMinStereoToolWebPortBase) + "–" + std::to_string(kMaxStereoToolWebPortBase));
  } else if (const int last = c.stereotool_web_port_base + kMaxStations - 1;
             http_port_ok && c.http_port >= c.stereotool_web_port_base && c.http_port <= last) {
    add("stereotool_web_port_base", "Porty interfejsu WWW Stereo Tool (" + std::to_string(c.stereotool_web_port_base) + "–" +
                                        std::to_string(last) + ") obejmują port panelu " + std::to_string(c.http_port) +
                                        ": zmień port bazowy albo port panelu");
  }
  if (const std::string why = key_shape_error(c.stereotool_default_key); !why.empty())
    add("stereotool_default_key", "Domyślny klucz Stereo Tool " + why);
  if (c.stations.size() > static_cast<size_t>(kMaxStations))
    add("stations", "Najwyżej " + std::to_string(kMaxStations) + " stacji (jest " + std::to_string(c.stations.size()) + ")");

  std::map<std::string, size_t> station_ids;                       // id in lower case -> station index
  std::map<int, size_t> web_slots;                                 // web port slot -> station index
  std::map<std::string, std::pair<size_t, size_t>> targets;        // server + mount -> (station, stream)
  for (size_t i = 0; i < c.stations.size(); ++i) {
    const auto& st = c.stations[i];
    const std::string sp = station_path(i);
    const std::string label = station_label(st, i) + ": ";

    if (st.id.empty()) {
      add(sp + ".id", label + "brak identyfikatora");
    } else if (!valid_id(st.id)) {
      add(sp + ".id", label + "nieprawidłowy identyfikator „" + st.id + "” (dozwolone litery, cyfry, „-” i „_”, najwyżej 32 znaki)");
    } else if (station_number(st.id) >= kMaxStationNumber) {
      // also what a new station gets once a hand-made id took the largest number: name the station
      // with the largest valid number (the cause) and say how out
      std::string way_out;
      size_t largest = c.stations.size();
      for (size_t j = 0; j < c.stations.size(); ++j) {
        const int64_t n = station_number(c.stations[j].id);
        if (j != i && n > 0 && n < kMaxStationNumber &&
            (largest == c.stations.size() || n > station_number(c.stations[largest].id)))
          largest = j;
      }
      if (largest < c.stations.size()) {
        const StationConfig& big = c.stations[largest];
        way_out = "numery stacji skończyły się przez " + station_ref(big, largest) + " z ręcznie nadanym identyfikatorem „" +
                  big.id + "”: zatrzymaj usługę, w config.json zmień jej identyfikator na mniejszy (np. „s100”; jej folder "
                  "stereotool\\station-" + big.id + " zostaje wtedy niepotrzebny i można go usunąć), ustaw "
                  "„next_station_number” na największy numer stacji + 1 i uruchom usługę";
      } else {
        way_out = "numery stacji skończyły się przez ręcznie nadany bardzo duży numer (tej stacji już nie ma): zatrzymaj "
                  "usługę, w config.json ustaw „next_station_number” na największy numer stacji + 1 i uruchom usługę";
      }
      add(sp + ".id", label + "identyfikator „" + st.id + "” ma za duży numer (najwyżej " +
                          std::to_string(kMaxStationNumber - 1) +
                          "): nadaj stacji inny identyfikator. Jeśli to nowa stacja dodana w panelu, " + way_out);
    } else if (auto [it, inserted] = station_ids.emplace(ascii_lower(st.id), i); !inserted) {
      const bool other_case = c.stations[it->second].id != st.id;
      add(sp + ".id", label + "identyfikator „" + st.id + "” jest już używany przez " + station_ref(c.stations[it->second], it->second) +
                          (other_case ? " (wielkość liter nie ma znaczenia)" : ""));
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

    // Stereo Tool processing: the shapes always, the requirements when it is switched on
    const ProcessingConfig& pr = st.processing;
    const std::string pf = sp + ".processing.";
    if (!pr.preset.empty() && !valid_preset_name(pr.preset)) {
      // never quoted when it could be a key pasted into the wrong field
      const bool quotable = pr.preset.size() <= kMaxPresetNameChars && !has_control(pr.preset) &&
                            pr.preset.find_first_of("<>") == std::string::npos;
      add(pf + "preset", label + "nieprawidłowa nazwa presetu Stereo Tool" + (quotable ? " „" + pr.preset + "”" : std::string()) +
                             " (litery, cyfry, spacja, „-”, „_” i „.”, bez spacji na początku, z rozszerzeniem .sts, najwyżej " +
                             std::to_string(kMaxPresetNameChars) + " znaki)");
    }
    if (const std::string why = key_shape_error(pr.license_key); !why.empty())
      add(pf + "license_key", label + "klucz licencji Stereo Tool " + why);
    if (pr.web_port_slot < -1 || pr.web_port_slot >= kMaxStations) {
      add(pf + "web_port_slot", label + "numer portu interfejsu WWW Stereo Tool musi być w zakresie 0–" + std::to_string(kMaxStations - 1));
    } else if (pr.web_port_slot >= 0) {
      if (auto [it, inserted] = web_slots.emplace(pr.web_port_slot, i); !inserted)
        add(pf + "web_port_slot", label + "port interfejsu WWW Stereo Tool (numer " + std::to_string(pr.web_port_slot) +
                                      ") jest już używany przez " + station_ref(c.stations[it->second], it->second));
    }
    if (pr.enabled) {
      if (pr.preset.empty()) add(pf + "preset", label + "processing wymaga wybrania presetu Stereo Tool");
      if (effective_stereotool_key(c, st).empty())
        add(pf + "license_key", label + "processing wymaga klucza licencji Stereo Tool (klucza stacji albo domyślnego klucza w Ustawieniach)");
    }
  }
  return issues;
}

std::vector<EncoderConfigIssue> config_warnings(const EncoderConfig& c) {
  std::vector<EncoderConfigIssue> warnings;
  std::map<std::string, size_t> first_on_key;  // effective key -> the first station using it
  for (size_t i = 0; i < c.stations.size(); ++i) {
    const StationConfig& st = c.stations[i];
    const std::string& key = effective_stereotool_key(c, st);
    if (!st.enabled || !st.processing.enabled || key.empty()) continue;
    auto [it, inserted] = first_on_key.emplace(key, i);
    if (inserted) continue;
    const StationConfig& first = c.stations[it->second];
    const bool both_default = st.processing.license_key.empty() && first.processing.license_key.empty();
    warnings.push_back({station_path(i) + ".processing.license_key",
                        station_label(st, i) + ": ten sam klucz licencji Stereo Tool" + (both_default ? " (domyślny)" : "") + " co " +
                            station_mention(first, it->second) + ": sprawdź, czy licencja obejmuje obie stacje"});
  }
  return warnings;
}


std::vector<std::string> normalize_config(EncoderConfig& c) {
  std::vector<std::string> notices;
  c.stereotool_default_key = trim_blanks(c.stereotool_default_key);
  c.stereotool_default_key_clear = false;
  std::set<std::string> station_ids;  // in lower case: a new id never differs from one only in case
  for (const auto& st : c.stations)
    if (!st.id.empty()) station_ids.insert(ascii_lower(st.id));
  // ruling S4: new ids continue past every number given so far (the counter) and every "s<n>" in
  // the configuration (a 1.0 file has no counter); a gap may be a deleted station's id
  int64_t next_station = std::clamp<int64_t>(c.next_station_number, 1, kMaxStationNumber);
  for (const auto& st : c.stations)
    if (const int64_t n = station_number(st.id); n < kMaxStationNumber) next_station = std::max(next_station, n + 1);
  // web port slots: a station without one gets the lowest free slot; given ones are never moved
  std::set<int> slots;
  for (const auto& st : c.stations)
    if (st.processing.web_port_slot >= 0) slots.insert(st.processing.web_port_slot);
  for (auto& st : c.stations) {
    if (st.processing.web_port_slot != -1) continue;
    int slot = 0;
    while (slots.count(slot)) ++slot;
    if (slot >= kMaxStations) break;  // more stations than slots: validate_config reports the count
    st.processing.web_port_slot = slot;
    slots.insert(slot);
  }

  c.next_station_number = next_station;
  for (size_t i = 0; i < c.stations.size(); ++i) {
    auto& st = c.stations[i];
    if (st.id.empty()) st.id = next_id(kStationIdPrefix, station_ids, next_station);
    c.next_station_number = next_station;
    st.processing.license_key = trim_blanks(st.processing.license_key);
    st.processing.license_key_clear = false;

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
    int64_t next_stream = 1;
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
  m.stereotool_default_key.clear();
  for (auto& st : m.stations) {
    st.processing.license_key.clear();
    for (auto& s : st.streams) s.password.clear();
  }
  return m;
}

void retire_reused_ids(EncoderConfig& next, const EncoderConfig& previous) {
  for (auto& st : next.stations) {
    if (st.id.empty()) continue;
    bool same = false, other_case = false;
    for (const auto& o : previous.stations) {
      same |= o.id == st.id;
      other_case |= ascii_lower(o.id) == ascii_lower(st.id);
    }
    if (same) continue;
    const int64_t n = station_number(st.id);
    if (other_case || (n > 0 && n < previous.next_station_number)) st.id.clear();  // normalize_config gives a new one
  }
}

void merge_passwords(EncoderConfig& next, const EncoderConfig& previous) {
  next.password_hash = previous.password_hash;
  next.next_station_number = std::max(next.next_station_number, previous.next_station_number);  // never back (S4)
  // Stereo Tool keys: no server rule (a key is not tied to a server); a blank key is no key
  if (trim_blanks(next.stereotool_default_key).empty())
    next.stereotool_default_key = next.stereotool_default_key_clear ? std::string() : previous.stereotool_default_key;
  next.stereotool_default_key_clear = false;
  for (auto& st : next.stations) {
    const bool clear_key = st.processing.license_key_clear;
    st.processing.license_key_clear = false;
    const bool keep_key = trim_blanks(st.processing.license_key).empty();
    if (keep_key) st.processing.license_key.clear();
    if (st.id.empty()) continue;
    const StationConfig* old_station = nullptr;
    for (const auto& o : previous.stations)
      if (o.id == st.id) {
        old_station = &o;
        break;
      }
    if (!old_station) continue;
    if (keep_key && !clear_key) st.processing.license_key = old_station->processing.license_key;
    if (st.processing.web_port_slot == -1) st.processing.web_port_slot = old_station->processing.web_port_slot;
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
