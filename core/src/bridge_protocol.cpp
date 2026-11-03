// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/bridge_protocol.hpp"

#include <nlohmann/json.hpp>

namespace bam {

namespace {
using json = nlohmann::json;

const char* kind_name(ShmSource k) {
  switch (k) {
    case ShmSource::Net:
      return "net";
    case ShmSource::Asio:
      return "asio";
    case ShmSource::Win:
      return "win";
    case ShmSource::Mix:
      return "mix";
    case ShmSource::None:
      break;
  }
  return "none";
}

ShmSource kind_from(const std::string& s) {
  if (s == "net") return ShmSource::Net;
  if (s == "asio") return ShmSource::Asio;
  if (s == "win") return ShmSource::Win;
  if (s == "mix") return ShmSource::Mix;
  return ShmSource::None;
}
}  // namespace

std::string to_json(const BridgeRequest& r) {
  json sources = json::array();
  for (const auto& s : r.sources) sources.push_back({{"slot", s.slot}, {"id", s.id}});
  json dests = json::array();
  for (const auto& d : r.dests)
    dests.push_back({{"id", d.id},
                      {"kind", kind_name(d.kind)},
                      {"pair", d.pair},
                      {"delay_frames", d.delay_frames},
                      {"channel_pair", d.channel_pair}});
  return json{{"generation", r.generation}, {"sources", sources}, {"dests", dests}}.dump();
}

std::optional<BridgeRequest> request_from_json(std::string_view text) {
  try {
    const json j = json::parse(text);
    BridgeRequest r;
    r.generation = j.at("generation").get<uint64_t>();
    for (const auto& s : j.at("sources")) {
      BridgeSourceReq q;
      q.slot = s.at("slot").get<int>();
      q.id = s.at("id").get<std::string>();
      if (q.slot < 0 || q.slot >= kWinPairs) return std::nullopt;
      r.sources.push_back(std::move(q));
    }
    for (const auto& d : j.at("dests")) {
      BridgeDestReq q;
      q.id = d.at("id").get<std::string>();
      q.kind = kind_from(d.at("kind").get<std::string>());
      q.pair = d.at("pair").get<int>();
      q.delay_frames = d.at("delay_frames").get<uint32_t>();
      q.channel_pair = d.value("channel_pair", 1);
      r.dests.push_back(std::move(q));
    }
    return r;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string to_json(const BridgeReport& r) {
  json endpoints = json::array();
  for (const auto& e : r.endpoints)
    endpoints.push_back(
        {{"id", e.id}, {"name", e.name}, {"render", e.render}, {"default", e.default_device}, {"channels", e.channels}});
  json apps = json::array();
  for (const auto& a : r.apps) apps.push_back({{"exe", a.exe}, {"name", a.name}, {"pid", a.pid}});
  json slots = json::array();
  for (const auto& s : r.slots)
    slots.push_back({{"id", s.id}, {"ok", s.ok}, {"reason", s.reason}, {"db", {s.peak_db[0], s.peak_db[1]}}});
  return json{{"generation", r.generation}, {"os_build", r.os_build}, {"endpoints", endpoints}, {"apps", apps}, {"slots", slots}}.dump();
}

std::optional<BridgeReport> report_from_json(std::string_view text) {
  try {
    const json j = json::parse(text);
    BridgeReport r;
    r.generation = j.at("generation").get<uint64_t>();
    r.os_build = j.value("os_build", "");
    for (const auto& e : j.at("endpoints"))
      r.endpoints.push_back({e.at("id").get<std::string>(), e.value("name", ""), e.value("render", true), e.value("default", false),
                              e.value("channels", 2)});
    for (const auto& a : j.at("apps")) r.apps.push_back({a.at("exe").get<std::string>(), a.value("name", ""), a.value("pid", 0u)});
    for (const auto& s : j.at("slots")) {
      BridgeSlotState st;
      st.id = s.at("id").get<std::string>();
      st.ok = s.value("ok", false);
      st.reason = s.value("reason", "");
      if (s.contains("db") && s["db"].is_array() && s["db"].size() == 2) {
        st.peak_db[0] = s["db"][0].get<float>();
        st.peak_db[1] = s["db"][1].get<float>();
      }
      r.slots.push_back(std::move(st));
    }
    return r;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace bam
