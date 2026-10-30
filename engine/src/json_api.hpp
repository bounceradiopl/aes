// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <nlohmann/json.hpp>

#include "bam/engine.hpp"
#include "bam/log.hpp"

namespace bam::api {

using json = nlohmann::ordered_json;

json config_json(const Config& c);        // version 2 shape, without password_hash
json status_json(const EngineStatus& s);  // full status incl. counters
json routing_json(const RoutingView& v);
json live_json(const EngineStatus& s);    // compact feed for meters (10x/s)
json streams_json(const std::vector<DiscoveredStream>& list);
json logs_json(const std::vector<LogEntry>& entries);
json issues_json(const std::vector<ConfigIssue>& issues);
json result_json(const ApplyResult& r);
const char* ptp_state_code(const EngineStatus& s);  // "master" | "locked" | "holdover" | "listening" | "freerun" | "disabled"

}  // namespace bam::api
