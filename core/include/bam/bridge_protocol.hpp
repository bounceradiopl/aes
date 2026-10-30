// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bam/shm_layout.hpp"

namespace bam {

// Service -> bridge (shared memory "request", JSON): what to capture into win_src pairs and what
// to play on Windows output devices.
struct BridgeSourceReq {
  int slot = 0;    // win_src pair 0..15
  std::string id;  // win-app:<exe> / win-loop:<endpoint> / win-in:<endpoint>
};

struct BridgeDestReq {
  std::string id;  // win-out:<endpoint>
  ShmSource kind = ShmSource::None;
  int pair = 0;
  uint32_t delay_frames = 0;  // D_src of the routed source
};

struct BridgeRequest {
  uint64_t generation = 0;
  std::vector<BridgeSourceReq> sources;
  std::vector<BridgeDestReq> dests;
};

// Bridge -> service (shared memory "report", JSON): what exists in this Windows session and how each slot is doing.
struct BridgeEndpoint {
  std::string id;
  std::string name;
  bool render = true;  // false: capture device
  bool default_device = false;
};

struct BridgeApp {
  std::string exe;  // lower case
  std::string name;
  uint32_t pid = 0;
};

struct BridgeSlotState {
  std::string id;
  bool ok = false;
  std::string reason;  // Polish, why the slot is unavailable
  float peak_db[2] = {-120, -120};
};

struct BridgeReport {
  uint64_t generation = 0;  // request generation the bridge has applied
  std::string os_build;
  std::vector<BridgeEndpoint> endpoints;
  std::vector<BridgeApp> apps;
  std::vector<BridgeSlotState> slots;
};

std::string to_json(const BridgeRequest& r);
std::optional<BridgeRequest> request_from_json(std::string_view text);
std::string to_json(const BridgeReport& r);
std::optional<BridgeReport> report_from_json(std::string_view text);

}  // namespace bam
