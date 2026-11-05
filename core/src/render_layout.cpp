// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/render_layout.hpp"

#include <algorithm>
#include <map>

#include "bam/routing.hpp"

namespace bam {

std::vector<DeviceLayout> device_layouts(const std::vector<BridgeDestReq>& dests) {
  struct Entry {
    RenderFeed feed;
    const std::string* id;
  };
  std::map<std::string, std::vector<Entry>> devices;
  for (const auto& d : dests) {
    const auto parsed = parse_dest(d.id);
    if (!parsed || parsed->kind != DestKind::WinOut) continue;
    devices[parsed->key].push_back({{parsed->pair, d.kind, d.pair, d.delay_frames}, &d.id});
  }
  std::vector<DeviceLayout> out;
  out.reserve(devices.size());
  for (auto& [endpoint, entries] : devices) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      return a.feed.channel_pair != b.feed.channel_pair ? a.feed.channel_pair < b.feed.channel_pair : *a.id < *b.id;
    });
    DeviceLayout& l = out.emplace_back();
    l.endpoint = endpoint;
    for (const Entry& e : entries) {
      l.feeds.push_back(e.feed);
      l.dest_ids.push_back(*e.id);
      l.max_delay = std::max(l.max_delay, e.feed.delay);
    }
  }
  return out;
}

DeviceAction plan_device(const DeviceLayout* previous, const DeviceLayout* next) {
  if (!previous) return next ? DeviceAction::Start : DeviceAction::Keep;
  if (!next) return DeviceAction::Stop;
  // the pacer reads "now - max_delay": another timeline needs a new stream
  if (previous->max_delay != next->max_delay) return DeviceAction::Restart;
  return previous->feeds == next->feeds ? DeviceAction::Keep : DeviceAction::UpdateFeeds;
}

}  // namespace bam
