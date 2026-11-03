// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_api.hpp"

#include <cmath>

#include "bam/platform/udp.hpp"

namespace bam::api {

namespace {
double round1(double v) { return std::round(v * 10.0) / 10.0; }

json db_pair(const float (&db)[2]) { return {round1(db[0]), round1(db[1])}; }

json ptp_json(const EngineStatus& s) {
  return {{"state", ptp_state_code(s)},
          {"role", ptp_role_name(s.ptp_role)},
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

json tx_counters(const TxStats& t) { return {{"tx_packets", t.packets}, {"tx_bytes", t.bytes}, {"tx_errors", t.errors}}; }

json rx_counters(const RxStats& r) {
  return {{"rx_packets", r.packets},
          {"rx_bytes", r.bytes},
          {"rx_underruns", r.underruns},
          {"rx_overruns", r.overruns},
          {"rx_seq_errors", r.seq_errors},
          {"rx_jitter_ms", std::round(r.jitter_ms * 1000.0) / 1000.0}};
}

json asio_json(const EngineStatus& s) { return {{"connected", s.asio_connected}, {"buffer_frames", s.asio_buffer_frames}}; }
}  // namespace

json config_json(const Config& c) {
  json j = json::parse(config_to_json(c));
  j.erase("password_hash");
  return j;
}

const char* ptp_state_code(const EngineStatus& s) {
  if (!s.ptp_enabled) return "disabled";
  switch (s.ptp.state) {
    case PtpState::Master:
      return "master";
    case PtpState::Locked:
      return "locked";
    case PtpState::Holdover:
      return "holdover";
    case PtpState::Listening:
      return s.ptp.steps > 0 ? "listening" : "freerun";
  }
  return "freerun";
}

json status_json(const EngineStatus& s) {
  json outputs = json::array();
  for (const auto& o : s.outputs) {
    outputs.push_back({{"index", o.index},
                       {"enabled", o.enabled},
                       {"name", o.name},
                       {"address", o.address},
                       {"ptime_us", o.ptime_us},
                       {"source", o.source},
                       {"active", o.tx_active},
                       {"conflict", o.conflict},
                       {"counters", tx_counters(o.tx)},
                       {"db", db_pair(o.peak_db)}});
  }
  json subs = json::array();
  for (const auto& sub : s.subscriptions) {
    subs.push_back({{"index", sub.index},
                    {"enabled", sub.enabled},
                    {"name", sub.name},
                    {"address", sub.address},
                    {"signal", sub.signal},
                    {"error", sub.error},
                    {"latency_us", sub.latency_us},
                    {"counters", rx_counters(sub.rx)},
                    {"db", db_pair(sub.peak_db)}});
  }
  return {{"product", kProductName},
          {"version", s.version},
          {"station_name", s.station_name},
          {"uptime_s", s.uptime_s},
          {"interface_ip", s.interface_ip},
          {"ptp", ptp_json(s)},
          {"asio", asio_json(s)},
          {"bridge", {{"connected", s.bridge_connected}}},
          {"mixer_late", s.mixer_late},
          {"outputs", outputs},
          {"subscriptions", subs}};
}

json live_json(const EngineStatus& s) {
  json outputs = json::array();
  for (const auto& o : s.outputs) {
    if (!o.enabled) continue;
    outputs.push_back({{"i", o.index}, {"db", db_pair(o.peak_db)}, {"tx", o.tx_active}, {"c", tx_counters(o.tx)}});
  }
  json subs = json::array();
  for (const auto& sub : s.subscriptions) {
    if (!sub.enabled) continue;
    subs.push_back({{"i", sub.index}, {"db", db_pair(sub.peak_db)}, {"sig", sub.signal}, {"c", rx_counters(sub.rx)}});
  }
  return {{"ptp", ptp_json(s)},
          {"asio", asio_json(s)},
          {"bridge", {{"connected", s.bridge_connected}}},
          {"uptime_s", s.uptime_s},
          {"outputs", outputs},
          {"subscriptions", subs}};
}

json routing_json(const RoutingView& v) {
  auto nodes = [](const std::vector<RouteNode>& list) {
    json out = json::array();
    for (const auto& n : list) {
      out.push_back({{"id", n.id},
                     {"name", n.name},
                     {"group", n.group},
                     {"available", n.available},
                     {"note", n.note},
                     {"virtual", n.virtual_device},
                     {"mix", n.mix},
                     {"db", db_pair(n.peak_db)}});
    }
    return out;
  };
  json routes = json::object();
  for (const auto& [dst, src] : v.routes) routes[dst] = src;
  json mixes = json::object(), delays = json::object();
  for (const auto& [dst, m] : v.mixes) {
    json list = json::array();
    for (const auto& s : m.sources) list.push_back({{"source", s.source}, {"gain_db", s.gain_db}, {"mute", s.mute}});
    mixes[dst] = list;
  }
  for (const auto& [dst, ms] : v.mix_delay_ms) delays[dst] = ms;
  return {{"sources", nodes(v.sources)}, {"dests", nodes(v.dests)}, {"routes", routes}, {"mixes", mixes}, {"mix_delay_ms", delays}};
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

json result_json(const ApplyResult& r) {
  json j = {{"ok", r.ok}, {"issues", issues_json(r.issues)}, {"notices", r.notices}};
  if (r.created) j["created"] = r.created;
  return j;
}

}  // namespace bam::api
