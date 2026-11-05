// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "bam/bridge_protocol.hpp"
#include "bam/shm_layout.hpp"

namespace bam {

// One channel pair of a Windows playback device and the timeline pair it plays.
struct RenderFeed {
  int channel_pair = 1;  // device channels 2p-1, 2p (1-based)
  ShmSource kind = ShmSource::None;
  int pair = 0;          // ring pair of `kind`
  uint32_t delay = 0;    // D_src: the rings are complete up to "now - delay"
  bool operator==(const RenderFeed&) const = default;
};

// Everything the bridge plays on one playback device (one WASAPI stream).
struct DeviceLayout {
  std::string endpoint;
  uint32_t max_delay = 0;                // the stream's timeline: the slowest feed's delay
  std::vector<RenderFeed> feeds;         // sorted by channel_pair
  std::vector<std::string> dest_ids;     // request ids, parallel to `feeds` (slot state reporting)
};

enum class DeviceAction { Keep, UpdateFeeds, Restart, Start, Stop };

// Groups the request's win-out destinations by endpoint (the "#N" of an id is its channel pair).
std::vector<DeviceLayout> device_layouts(const std::vector<BridgeDestReq>& dests);

// What to do with a device given its previous and its new layout (nullptr: none). A running
// stream takes new feeds in place as long as its timeline (max_delay) stays the same.
DeviceAction plan_device(const DeviceLayout* previous, const DeviceLayout* next);

}  // namespace bam
