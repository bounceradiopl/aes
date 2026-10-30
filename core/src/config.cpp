// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/config.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>

#include "bam/gain.hpp"
#include "bam/ipv4.hpp"
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
}  // namespace

Config default_config() {
  Config c;
  for (int i = 0; i < kDeviceCount; ++i) {
    auto& d = c.devices[i];
    d.enabled = i == 0;
    d.name = "BounceAES " + two_digits(i + 1);
    d.tx_address = "239.69.100." + std::to_string(i + 1);
  }
  return c;
}

std::string config_to_json(const Config& c) {
  json j;
  j["interface_ip"] = c.interface_ip;
  j["ptp_domain"] = c.ptp_domain;
  j["pc_nominal_level_dbfs"] = c.pc_nominal_level_dbfs;
  j["http_bind"] = c.http_bind;
  j["http_port"] = c.http_port;
  j["password_hash"] = c.password_hash;
  json devices = json::array();
  for (const auto& d : c.devices) {
    devices.push_back({{"enabled", d.enabled},
                       {"name", d.name},
                       {"tx_address", d.tx_address},
                       {"tx_ptime_us", d.tx_ptime_us},
                       {"keep_active", d.keep_active},
                       {"rx_sdp", d.rx_sdp},
                       {"rx_latency_us", d.rx_latency_us}});
  }
  j["devices"] = devices;
  return j.dump(2);
}

std::optional<Config> config_from_json(std::string_view text, std::string* error) {
  try {
    const json j = json::parse(text);
    if (!j.is_object()) throw std::runtime_error("oczekiwano obiektu JSON");
    Config c = default_config();
    read_field(j, "interface_ip", c.interface_ip);
    read_field(j, "ptp_domain", c.ptp_domain);
    read_field(j, "pc_nominal_level_dbfs", c.pc_nominal_level_dbfs);
    read_field(j, "http_bind", c.http_bind);
    read_field(j, "http_port", c.http_port);
    read_field(j, "password_hash", c.password_hash);
    if (j.contains("devices")) {
      const auto& arr = j.at("devices");
      if (!arr.is_array()) throw std::runtime_error("\"devices\" musi być tablicą");
      for (size_t i = 0; i < arr.size() && i < c.devices.size(); ++i) {
        auto& d = c.devices[i];
        const auto& dj = arr[i];
        read_field(dj, "enabled", d.enabled);
        read_field(dj, "name", d.name);
        read_field(dj, "tx_address", d.tx_address);
        read_field(dj, "tx_ptime_us", d.tx_ptime_us);
        read_field(dj, "keep_active", d.keep_active);
        read_field(dj, "rx_sdp", d.rx_sdp);
        read_field(dj, "rx_latency_us", d.rx_latency_us);
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
  auto add = [&](int dev, const char* field, std::string msg) { issues.push_back({dev, field, std::move(msg)}); };

  if (!c.interface_ip.empty() && !parse_ipv4(c.interface_ip)) add(0, "interface_ip", "nieprawidłowy adres karty sieciowej");
  if (c.ptp_domain < 0 || c.ptp_domain > 127) add(0, "ptp_domain", "domena PTP musi być w zakresie 0–127");
  if (c.pc_nominal_level_dbfs < -30 || c.pc_nominal_level_dbfs > 0)
    add(0, "pc_nominal_level_dbfs", "poziom nominalny musi być w zakresie −30…0 dBFS");
  if (!parse_ipv4(c.http_bind)) add(0, "http_bind", "nieprawidłowy adres panelu");
  if (c.http_port == 0) add(0, "http_port", "port panelu nie może być 0");

  std::map<std::string, int> used;
  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& d = c.devices[i];
    const int dev = i + 1;
    if (d.name.size() > kMaxNameBytes) add(dev, "name", "nazwa może mieć najwyżej 40 bajtów");
    auto ip = parse_ipv4(d.tx_address);
    if (!ip || !is_multicast(*ip) || is_link_local_multicast(*ip)) {
      add(dev, "tx_address", "adres wyjścia musi być adresem multicast 224.0.1.0–239.255.255.255");
    } else if (d.enabled) {
      auto [it, inserted] = used.emplace(d.tx_address, dev);
      if (!inserted) add(dev, "tx_address", "adres " + d.tx_address + " jest już używany przez urządzenie " + two_digits(it->second));
    }
    if (d.tx_ptime_us != 1000 && d.tx_ptime_us != 1333 && d.tx_ptime_us != 4000)
      add(dev, "tx_ptime_us", "ptime musi wynosić 1 ms, 1,333 ms lub 4 ms");
    if (d.rx_latency_us < kMinLatencyUs || d.rx_latency_us > kMaxLatencyUs)
      add(dev, "rx_latency_us", "opóźnienie odbioru musi być w zakresie 1–1000 ms");
    if (!d.rx_sdp.empty()) {
      auto r = parse_sdp(d.rx_sdp);
      if (!r.stream) {
        add(dev, "rx_sdp", "SDP wejścia: " + r.error);
      } else if (d.rx_latency_us >= kMinLatencyUs && d.rx_latency_us < min_latency_for(*r.stream)) {
        add(dev, "rx_latency_us", "opóźnienie odbioru musi być co najmniej ptime źródła + 1 ms");
      }
    }
  }
  return issues;
}

std::vector<std::string> normalize_config(Config& c) {
  std::vector<std::string> notices;
  for (int i = 0; i < kDeviceCount; ++i) {
    auto& d = c.devices[i];
    if (d.rx_sdp.empty()) continue;
    auto r = parse_sdp(d.rx_sdp);
    if (!r.stream) continue;
    const uint32_t min = min_latency_for(*r.stream);
    if (d.rx_latency_us < min) {
      d.rx_latency_us = min;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "Urządzenie %s: opóźnienie odbioru podniesione do %.3f ms (ptime źródła %.3f ms + 1 ms)",
                    two_digits(i + 1).c_str(), min / 1000.0, r.stream->ptime_ms);
      notices.emplace_back(buf);
    }
  }
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
