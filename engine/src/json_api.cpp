// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_api.hpp"

#include <cmath>

#include "bam/platform/udp.hpp"

namespace bam::api {

namespace {
double round1(double v) { return std::round(v * 10.0) / 10.0; }
}  // namespace

json device_json(const DeviceConfig& d, int index) {
  return {{"index", index},
          {"enabled", d.enabled},
          {"name", d.name},
          {"tx_address", d.tx_address},
          {"tx_ptime_us", d.tx_ptime_us},
          {"keep_active", d.keep_active},
          {"rx_sdp", d.rx_sdp},
          {"rx_latency_us", d.rx_latency_us}};
}

json config_json(const Config& c) {
  json devices = json::array();
  for (int i = 0; i < kDeviceCount; ++i) devices.push_back(device_json(c.devices[i], i + 1));
  return {{"interface_ip", c.interface_ip},
          {"ptp_domain", c.ptp_domain},
          {"pc_nominal_level_dbfs", c.pc_nominal_level_dbfs},
          {"http_bind", c.http_bind},
          {"http_port", c.http_port},
          {"devices", devices}};
}

const char* ptp_state_code(const EngineStatus& s) {
  if (!s.ptp_enabled) return "disabled";
  switch (s.ptp.state) {
    case PtpState::Locked:
      return "locked";
    case PtpState::Holdover:
      return "holdover";
    case PtpState::Listening:
      return s.ptp.steps > 0 ? "listening" : "freerun";
  }
  return "freerun";
}

static json ptp_json(const EngineStatus& s) {
  return {{"state", ptp_state_code(s)},
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

static json counters_json(const DeviceStatus& d) {
  return {{"tx_packets", d.tx.packets},
          {"tx_bytes", d.tx.bytes},
          {"tx_errors", d.tx.errors},
          {"rx_packets", d.rx.packets},
          {"rx_bytes", d.rx.bytes},
          {"rx_underruns", d.rx.underruns},
          {"rx_overruns", d.rx.overruns},
          {"rx_seq_errors", d.rx.seq_errors},
          {"rx_jitter_ms", std::round(d.rx.jitter_ms * 1000.0) / 1000.0}};
}

json status_json(const EngineStatus& s) {
  json devices = json::array();
  for (const auto& d : s.devices) {
    devices.push_back({{"index", d.index},
                       {"enabled", d.enabled},
                       {"name", d.name},
                       {"tx_address", d.tx_address},
                       {"tx_active", d.tx_active},
                       {"tx_conflict", d.tx_conflict},
                       {"rx_configured", d.rx_configured},
                       {"rx_signal", d.rx_signal},
                       {"rx_source_name", d.rx_source_name},
                       {"rx_error", d.rx_error},
                       {"counters", counters_json(d)},
                       {"in_db", {round1(d.in_peak_db[0]), round1(d.in_peak_db[1])}},
                       {"out_db", {round1(d.out_peak_db[0]), round1(d.out_peak_db[1])}}});
  }
  return {{"product", kProductName},
          {"version", s.version},
          {"uptime_s", s.uptime_s},
          {"interface_ip", s.interface_ip},
          {"ptp", ptp_json(s)},
          {"asio", {{"connected", s.asio_connected}, {"buffer_frames", s.asio_buffer_frames}}},
          {"devices", devices}};
}

json live_json(const EngineStatus& s) {
  json devices = json::array();
  for (const auto& d : s.devices) {
    if (!d.enabled) continue;
    devices.push_back({{"i", d.index},
                       {"in", {round1(d.in_peak_db[0]), round1(d.in_peak_db[1])}},
                       {"out", {round1(d.out_peak_db[0]), round1(d.out_peak_db[1])}},
                       {"sig", d.rx_signal},
                       {"tx", d.tx_active},
                       {"c", counters_json(d)}});
  }
  return {{"ptp", ptp_json(s)},
          {"asio", {{"connected", s.asio_connected}, {"buffer_frames", s.asio_buffer_frames}}},
          {"uptime_s", s.uptime_s},
          {"devices", devices}};
}

json streams_json(const std::vector<DiscoveredStream>& list) {
  json out = json::array();
  for (const auto& s : list) {
    out.push_back({{"id", s.id},
                   {"name", s.name},
                   {"origin", s.origin},
                   {"address", s.parsed.multicast_address},
                   {"port", s.parsed.port},
                   {"encoding", encoding_name(s.parsed.encoding)},
                   {"channels", s.parsed.channels},
                   {"ptime_ms", std::round(s.parsed.ptime_ms * 1000.0) / 1000.0},
                   {"sdp", s.sdp}});
  }
  return out;
}

json logs_json(const std::vector<LogEntry>& entries) {
  json out = json::array();
  for (const auto& e : entries) {
    const char* level = e.level == LogLevel::Error ? "error" : e.level == LogLevel::Warning ? "warning" : "info";
    out.push_back({{"time_ms", e.unix_ms}, {"level", level}, {"message", e.message}});
  }
  return out;
}

json issues_json(const std::vector<ConfigIssue>& issues) {
  json out = json::array();
  for (const auto& i : issues) out.push_back({{"device", i.device}, {"field", i.field}, {"message", i.message}});
  return out;
}

}  // namespace bam::api
