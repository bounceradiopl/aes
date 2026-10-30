// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/config.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <nlohmann/json.hpp>

#include "bam/gain.hpp"
#include "bam/ipv4.hpp"
#include "bam/routing.hpp"
#include "bam/sdp.hpp"

namespace bam {

namespace {
using json = nlohmann::ordered_json;

constexpr uint32_t kMinLatencyUs = 1000;
constexpr uint32_t kMaxLatencyUs = 1'000'000;
constexpr size_t kMaxNameBytes = 40;

std::string two_digits(int n) {
  char b[8];
  std::snprintf(b, sizeof(b), "%02d", n);
  return b;
}

template <typename T>
void read_field(const json& j, const char* key, T& out) {
  if (j.contains(key)) out = j.at(key).get<T>();
}

uint32_t min_latency_for(const SdpStream& s) {
  return static_cast<uint32_t>(std::ceil(s.ptime_ms * 1000.0 - 1e-6)) + 1000;
}

// 1.0 → 1.1: every "device" becomes an output, a subscription and an ASIO pair name, wired the
// way 1.0 worked (ASIO N plays to output N, subscription N is recorded on ASIO N).
void migrate_v1(const json& j, Config& c) {
  if (!j.contains("devices")) return;
  const auto& arr = j.at("devices");
  if (!arr.is_array()) throw std::runtime_error("\"devices\" musi być tablicą");
  c.routes.clear();
  for (size_t i = 0; i < arr.size() && i < c.outputs.size(); ++i) {
    const auto& dj = arr[i];
    auto& o = c.outputs[i];
    auto& s = c.subscriptions[i];
    read_field(dj, "enabled", o.enabled);
    read_field(dj, "name", o.name);
    read_field(dj, "tx_address", o.address);
    read_field(dj, "tx_ptime_us", o.ptime_us);
    read_field(dj, "keep_active", o.keep_active);
    read_field(dj, "rx_sdp", s.sdp);
    read_field(dj, "rx_latency_us", s.latency_us);
    c.asio_names[i] = o.name;
    s.enabled = o.enabled && !s.sdp.empty();
    if (s.enabled) {
      auto parsed = parse_sdp(s.sdp);
      s.name = parsed.stream ? parsed.stream->session_name : "";
    }
    const std::string n = std::to_string(i + 1);
    if (o.enabled) c.routes["net-out:" + n] = "asio-out:" + n;
    if (s.enabled) c.routes["asio-in:" + n] = "net-in:" + n;
  }
}
}  // namespace

const char* ptp_role_name(PtpRole r) {
  switch (r) {
    case PtpRole::Auto:
      return "auto";
    case PtpRole::Preferred:
      return "preferred";
    case PtpRole::Slave:
      return "slave";
  }
  return "auto";
}

std::optional<PtpRole> parse_ptp_role(std::string_view s) {
  if (s == "auto") return PtpRole::Auto;
  if (s == "preferred") return PtpRole::Preferred;
  if (s == "slave") return PtpRole::Slave;
  return std::nullopt;
}

Config default_config() {
  Config c;
  for (int i = 0; i < kDeviceCount; ++i) {
    auto& o = c.outputs[i];
    o.enabled = i == 0;
    o.name = "BounceAES " + two_digits(i + 1);
    o.address = "239.69.100." + std::to_string(i + 1);
    c.asio_names[i] = o.name;
  }
  c.routes["net-out:1"] = "asio-out:1";
  return c;
}

std::string config_to_json(const Config& c) {
  json j;
  j["version"] = kConfigVersion;
  j["station_name"] = c.station_name;
  j["interface_ip"] = c.interface_ip;
  j["ptp_domain"] = c.ptp_domain;
  j["ptp_role"] = ptp_role_name(c.ptp_role);
  j["ptp_priority1"] = c.ptp_priority1;
  j["ptp_priority2"] = c.ptp_priority2;
  j["pc_nominal_level_dbfs"] = c.pc_nominal_level_dbfs;
  j["http_bind"] = c.http_bind;
  j["http_port"] = c.http_port;
  j["password_hash"] = c.password_hash;
  json outputs = json::array(), subs = json::array(), names = json::array();
  for (const auto& o : c.outputs)
    outputs.push_back({{"enabled", o.enabled}, {"name", o.name}, {"address", o.address}, {"ptime_us", o.ptime_us}, {"keep_active", o.keep_active}});
  for (const auto& s : c.subscriptions)
    subs.push_back({{"enabled", s.enabled}, {"name", s.name}, {"sdp", s.sdp}, {"latency_us", s.latency_us}});
  for (const auto& n : c.asio_names) names.push_back(n);
  j["outputs"] = outputs;
  j["subscriptions"] = subs;
  j["asio_names"] = names;
  json routes = json::object();
  for (const auto& [dst, src] : c.routes) routes[dst] = src;
  j["routes"] = routes;
  return j.dump(2);
}

std::optional<Config> config_from_json(std::string_view text, std::string* error, bool* migrated) {
  if (migrated) *migrated = false;
  try {
    const json j = json::parse(text);
    if (!j.is_object()) throw std::runtime_error("oczekiwano obiektu JSON");
    Config c = default_config();
    read_field(j, "station_name", c.station_name);
    read_field(j, "interface_ip", c.interface_ip);
    read_field(j, "ptp_domain", c.ptp_domain);
    if (j.contains("ptp_role")) {
      auto role = parse_ptp_role(j.at("ptp_role").get<std::string>());
      if (!role) throw std::runtime_error("nieznana rola PTP");
      c.ptp_role = *role;
    }
    read_field(j, "ptp_priority1", c.ptp_priority1);
    read_field(j, "ptp_priority2", c.ptp_priority2);
    read_field(j, "pc_nominal_level_dbfs", c.pc_nominal_level_dbfs);
    read_field(j, "http_bind", c.http_bind);
    read_field(j, "http_port", c.http_port);
    read_field(j, "password_hash", c.password_hash);

    if (!j.contains("version")) {
      migrate_v1(j, c);
      if (migrated) *migrated = j.contains("devices");
      return c;
    }
    if (j.at("version").get<int>() > kConfigVersion) throw std::runtime_error("plik z nowszej wersji programu");

    auto read_array = [&](const char* key, auto&& fn) {
      if (!j.contains(key)) return;
      const auto& arr = j.at(key);
      if (!arr.is_array()) throw std::runtime_error(std::string("\"") + key + "\" musi być tablicą");
      for (size_t i = 0; i < arr.size() && i < static_cast<size_t>(kDeviceCount); ++i) fn(i, arr[i]);
    };
    read_array("outputs", [&](size_t i, const json& oj) {
      auto& o = c.outputs[i];
      read_field(oj, "enabled", o.enabled);
      read_field(oj, "name", o.name);
      read_field(oj, "address", o.address);
      read_field(oj, "ptime_us", o.ptime_us);
      read_field(oj, "keep_active", o.keep_active);
    });
    read_array("subscriptions", [&](size_t i, const json& sj) {
      auto& s = c.subscriptions[i];
      read_field(sj, "enabled", s.enabled);
      read_field(sj, "name", s.name);
      read_field(sj, "sdp", s.sdp);
      read_field(sj, "latency_us", s.latency_us);
    });
    read_array("asio_names", [&](size_t i, const json& nj) { c.asio_names[i] = nj.get<std::string>(); });
    if (j.contains("routes")) {
      const auto& r = j.at("routes");
      if (!r.is_object()) throw std::runtime_error("\"routes\" musi być obiektem");
      c.routes.clear();
      for (auto it = r.begin(); it != r.end(); ++it) {
        if (it.value().is_null()) continue;
        c.routes[it.key()] = it.value().get<std::string>();
      }
    }
    return c;
  } catch (const std::exception& e) {
    if (error) *error = std::string("nieprawidłowa konfiguracja: ") + e.what();
    return std::nullopt;
  }
}

std::vector<ConfigIssue> validate_config(const Config& c) {
  std::vector<ConfigIssue> issues;
  auto add = [&](int n, std::string field, std::string msg) { issues.push_back({n, std::move(field), std::move(msg)}); };

  if (c.station_name.size() > kMaxNameBytes) add(0, "station_name", "nazwa stacji może mieć najwyżej 40 bajtów");
  if (!c.interface_ip.empty() && !parse_ipv4(c.interface_ip)) add(0, "interface_ip", "nieprawidłowy adres karty sieciowej");
  if (c.ptp_domain < 0 || c.ptp_domain > 127) add(0, "ptp_domain", "domena PTP musi być w zakresie 0–127");
  if (c.ptp_priority1 < 0 || c.ptp_priority1 > 255) add(0, "ptp_priority1", "priorytet PTP 1 musi być w zakresie 0–255");
  if (c.ptp_priority2 < 0 || c.ptp_priority2 > 255) add(0, "ptp_priority2", "priorytet PTP 2 musi być w zakresie 0–255");
  if (c.pc_nominal_level_dbfs < -30 || c.pc_nominal_level_dbfs > 0)
    add(0, "pc_nominal_level_dbfs", "poziom nominalny musi być w zakresie −30…0 dBFS");
  if (!parse_ipv4(c.http_bind)) add(0, "http_bind", "nieprawidłowy adres panelu");
  if (c.http_port == 0) add(0, "http_port", "port panelu nie może być 0");

  std::map<std::string, int> used;
  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& o = c.outputs[i];
    const int n = i + 1;
    const std::string f = "outputs[" + std::to_string(n) + "].";
    if (o.name.size() > kMaxNameBytes) add(n, f + "name", "nazwa może mieć najwyżej 40 bajtów");
    auto ip = parse_ipv4(o.address);
    if (!ip || !is_multicast(*ip) || is_link_local_multicast(*ip)) {
      add(n, f + "address", "adres wyjścia musi być adresem multicast 224.0.1.0–239.255.255.255");
    } else if (o.enabled) {
      auto [it, inserted] = used.emplace(o.address, n);
      if (!inserted) add(n, f + "address", "adres " + o.address + " jest już używany przez wyjście " + two_digits(it->second));
    }
    if (o.ptime_us != 1000 && o.ptime_us != 1333 && o.ptime_us != 4000) add(n, f + "ptime_us", "ptime musi wynosić 1 ms, 1,333 ms lub 4 ms");

    const auto& s = c.subscriptions[i];
    const std::string g = "subscriptions[" + std::to_string(n) + "].";
    if (s.name.size() > kMaxNameBytes) add(n, g + "name", "nazwa może mieć najwyżej 40 bajtów");
    if (s.latency_us < kMinLatencyUs || s.latency_us > kMaxLatencyUs)
      add(n, g + "latency_us", "opóźnienie odbioru musi być w zakresie 1–1000 ms");
    if (s.enabled && s.sdp.empty()) add(n, g + "sdp", "subskrypcja wymaga SDP źródła");
    if (!s.sdp.empty()) {
      auto r = parse_sdp(s.sdp);
      if (!r.stream) {
        add(n, g + "sdp", "SDP: " + r.error);
      } else if (s.latency_us >= kMinLatencyUs && s.latency_us < min_latency_for(*r.stream)) {
        add(n, g + "latency_us", "opóźnienie odbioru musi być co najmniej ptime źródła + 1 ms");
      }
    }
    if (c.asio_names[i].size() > kMaxNameBytes) add(n, "asio_names[" + std::to_string(n) + "]", "nazwa może mieć najwyżej 40 bajtów");
  }

  std::set<std::string> win_sources, win_dests;
  for (const auto& [dst, src] : c.routes) {
    auto d = parse_dest(dst);
    auto s = parse_source(src);
    if (!d) {
      add(0, "routes." + dst, "nieprawidłowy cel routingu: " + dst);
      continue;
    }
    if (!s) {
      add(0, "routes." + dst, "nieprawidłowe źródło routingu: " + src);
      continue;
    }
    if (is_windows(*s)) win_sources.insert(to_id(*s));
    if (is_windows(*d)) win_dests.insert(dst);
  }
  if (win_sources.size() > static_cast<size_t>(kMaxWindowsRoutes))
    add(0, "routes", "najwyżej 16 źródeł Windows jednocześnie");
  if (win_dests.size() > static_cast<size_t>(kMaxWindowsRoutes))
    add(0, "routes", "najwyżej 16 urządzeń Windows jako cele jednocześnie");
  return issues;
}

std::vector<std::string> normalize_config(Config& c) {
  std::vector<std::string> notices;
  for (int i = 0; i < kDeviceCount; ++i) {
    auto& s = c.subscriptions[i];
    if (s.sdp.empty()) continue;
    auto r = parse_sdp(s.sdp);
    if (!r.stream) continue;
    if (s.name.empty()) s.name = r.stream->session_name;
    const uint32_t min = min_latency_for(*r.stream);
    if (s.latency_us < min) {
      s.latency_us = min;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "Subskrypcja %s: opóźnienie odbioru podniesione do %.3f ms (ptime źródła %.3f ms + 1 ms)",
                    two_digits(i + 1).c_str(), min / 1000.0, r.stream->ptime_ms);
      notices.emplace_back(buf);
    }
  }
  // Canonicalise route ids (e.g. application names in lower case).
  std::map<std::string, std::string> canonical;
  for (const auto& [dst, src] : c.routes) {
    auto d = parse_dest(dst);
    auto s = parse_source(src);
    canonical[d ? to_id(*d) : dst] = s ? to_id(*s) : src;
  }
  c.routes = std::move(canonical);
  return notices;
}

uint32_t ptime_us_to_frames(uint32_t us) { return static_cast<uint32_t>(std::lround(us * (kSampleRate / 1000.0) / 1000.0)); }

double gain_to_app(const Config& c) {
  const int db = c.pc_nominal_level_dbfs + 20;
  return db == 0 ? 1.0 : db_to_gain(db);
}

double gain_to_network(const Config& c) {
  const int db = c.pc_nominal_level_dbfs + 20;
  return db == 0 ? 1.0 : db_to_gain(-db);
}

}  // namespace bam
