// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/render_layout.hpp"

namespace {
const std::string kUsb = "{0.0.0.00000000}.{usb}";
const std::string kSpk = "{0.0.0.00000000}.{spk}";

bam::BridgeDestReq dest(const std::string& id, bam::ShmSource kind, int pair, uint32_t delay, int channel_pair = 1) {
  bam::BridgeDestReq d;
  d.id = id;
  d.kind = kind;
  d.pair = pair;
  d.delay_frames = delay;
  d.channel_pair = channel_pair;
  return d;
}

bam::DeviceLayout layout(uint32_t max_delay, std::vector<bam::RenderFeed> feeds) { return {kUsb, max_delay, std::move(feeds), {}}; }
}  // namespace

TEST_CASE("render layout: dests are grouped per playback device, sorted by channel pair") {
  const std::vector<bam::BridgeDestReq> dests = {
      dest("win-out:" + kUsb + "#3", bam::ShmSource::Asio, 4, 240, 3),
      dest("win-out:" + kSpk, bam::ShmSource::Net, 1, 480),
      dest("net-out:2", bam::ShmSource::Asio, 0, 0),  // not a Windows device
      dest("win-out:" + kUsb, bam::ShmSource::Mix, 2, 960),
      dest("win-out:", bam::ShmSource::Asio, 0, 0),  // invalid id
  };
  const auto layouts = bam::device_layouts(dests);
  REQUIRE(layouts.size() == 2);
  const bam::DeviceLayout* usb = layouts[0].endpoint == kUsb ? &layouts[0] : &layouts[1];
  const bam::DeviceLayout* spk = layouts[0].endpoint == kSpk ? &layouts[0] : &layouts[1];
  REQUIRE(usb->endpoint == kUsb);
  REQUIRE(spk->endpoint == kSpk);

  REQUIRE(usb->feeds.size() == 2);
  CHECK(usb->feeds[0] == bam::RenderFeed{1, bam::ShmSource::Mix, 2, 960});
  CHECK(usb->feeds[1] == bam::RenderFeed{3, bam::ShmSource::Asio, 4, 240});
  CHECK(usb->max_delay == 960);
  REQUIRE(usb->dest_ids.size() == 2);
  CHECK(usb->dest_ids[0] == "win-out:" + kUsb);
  CHECK(usb->dest_ids[1] == "win-out:" + kUsb + "#3");

  REQUIRE(spk->feeds.size() == 1);
  CHECK(spk->feeds[0] == bam::RenderFeed{1, bam::ShmSource::Net, 1, 480});
  CHECK(spk->max_delay == 480);
}

TEST_CASE("render layout: the channel pair comes from the destination id") {
  // "#2" in the id is the device pair, whatever the request's channel_pair says
  const auto layouts = bam::device_layouts({dest("win-out:" + kUsb + "#2", bam::ShmSource::Asio, 0, 0, 1)});
  REQUIRE(layouts.size() == 1);
  REQUIRE(layouts[0].feeds.size() == 1);
  CHECK(layouts[0].feeds[0].channel_pair == 2);
}

TEST_CASE("render layout: no destinations, no devices") { CHECK(bam::device_layouts({}).empty()); }

TEST_CASE("render layout: what happens to a running device") {
  const auto one = layout(960, {{1, bam::ShmSource::Mix, 0, 960}});
  const auto two = layout(960, {{1, bam::ShmSource::Mix, 0, 960}, {2, bam::ShmSource::Asio, 1, 240}});
  const auto rerouted = layout(960, {{1, bam::ShmSource::Mix, 0, 960}, {2, bam::ShmSource::Asio, 3, 240}});
  const auto other_delay = layout(960, {{1, bam::ShmSource::Mix, 0, 960}, {2, bam::ShmSource::Asio, 1, 480}});
  const auto slower = layout(1200, {{1, bam::ShmSource::Mix, 0, 960}, {2, bam::ShmSource::Net, 1, 1200}});

  CHECK(bam::plan_device(nullptr, &one) == bam::DeviceAction::Start);
  CHECK(bam::plan_device(&one, nullptr) == bam::DeviceAction::Stop);
  CHECK(bam::plan_device(nullptr, nullptr) == bam::DeviceAction::Keep);
  CHECK(bam::plan_device(&two, &two) == bam::DeviceAction::Keep);
  CHECK(bam::plan_device(&one, &two) == bam::DeviceAction::UpdateFeeds);       // a pair joins
  CHECK(bam::plan_device(&two, &one) == bam::DeviceAction::UpdateFeeds);       // a pair leaves
  CHECK(bam::plan_device(&two, &rerouted) == bam::DeviceAction::UpdateFeeds);  // a pair gets another source
  CHECK(bam::plan_device(&two, &other_delay) == bam::DeviceAction::UpdateFeeds);
  CHECK(bam::plan_device(&two, &slower) == bam::DeviceAction::Restart);  // the pacer's timeline moves
  CHECK(bam::plan_device(&slower, &two) == bam::DeviceAction::Restart);
}

TEST_CASE("render layout: the request order does not restart a device") {
  const std::vector<bam::BridgeDestReq> dests = {dest("win-out:" + kUsb + "#2", bam::ShmSource::Asio, 1, 240, 2),
                                                 dest("win-out:" + kUsb, bam::ShmSource::Mix, 0, 960)};
  const auto a = bam::device_layouts(dests);
  const auto b = bam::device_layouts({dests[1], dests[0]});  // another request order
  REQUIRE(a.size() == 1);
  REQUIRE(b.size() == 1);
  CHECK(bam::plan_device(&a[0], &b[0]) == bam::DeviceAction::Keep);
}
