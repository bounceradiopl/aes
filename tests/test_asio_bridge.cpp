// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

#include "bam/asio_bridge.hpp"
#include "bam/log.hpp"
#include "bam/shm_layout.hpp"

namespace {
struct Shm {
  std::vector<std::byte> mem = std::vector<std::byte>(bam::ShmLayout::total_bytes());
  Shm() { bam::ShmView::initialize(mem.data(), 1); }
  bam::ShmView view() { return bam::ShmView(mem.data()); }
};
}  // namespace

TEST_CASE("inputs are read latency + one buffer behind the period start") {
  Shm shm;
  auto view = shm.view();
  view.header().rx_latency_frames[0] = 240;
  std::vector<int32_t> ramp(8000);
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<int32_t>(i);
  view.input(0).write(0, ramp.data(), ramp.size());

  bam::AsioBridge bridge(view);
  std::vector<int32_t> buf(128, -1);
  std::vector<int32_t*> ins(bam::kChannelCount, nullptr);
  ins[0] = buf.data();
  bridge.read_inputs(4800, 128, ins.data());
  CHECK(buf[0] == 4800 - 128 - 240);
  CHECK(buf[127] == 4800 - 240 - 1);

  // Too close to the timeline origin: silence instead of underflow.
  bridge.read_inputs(100, 128, ins.data());
  CHECK(buf[0] == 0);
}

TEST_CASE("outputs are written kOutputLeadBuffers ahead") {
  Shm shm;
  auto view = shm.view();
  bam::AsioBridge bridge(view);
  std::vector<int32_t> data(128);
  for (size_t i = 0; i < data.size(); ++i) data[i] = 1000 + static_cast<int32_t>(i);
  std::vector<const int32_t*> outs(bam::kChannelCount, nullptr);
  outs[1] = data.data();
  bridge.write_outputs(4800, 128, outs.data());
  std::vector<int32_t> back(128);
  CHECK(bam::AsioBridge::output_start(4800, 128) == 4800 + 256);
  view.output(1).read(4800 + 256, back.data(), 128);
  CHECK(back == data);
  // channel without a buffer gets valid silence
  std::vector<int32_t> silent(128, 7);
  view.output(0).read(4800 + 256, silent.data(), 128);
  for (auto v : silent) CHECK(v == 0);
}

TEST_CASE("channel labels") {
  CHECK(bam::channel_label(0, "BounceAES 01") == "BounceAES 01 L");
  CHECK(bam::channel_label(1, "") == "BounceAES 01 R");
  CHECK(bam::channel_label(3, "Kraków") == "BounceAES 02 R (Kraków)");
  CHECK(bam::channel_label(63, "x") == "BounceAES 32 R (x)");
  auto longer = bam::channel_label(0, "Zażółć gęślą jaźń bardzo długa nazwa");
  CHECK(longer.size() <= 31);
  // must not end in the middle of a UTF-8 sequence
  CHECK((static_cast<unsigned char>(longer.back()) & 0xC0) != 0xC0);
}

TEST_CASE("logger keeps recent entries") {
  auto& log = bam::Logger::instance();
  int seen = 0;
  log.add_sink([&](const bam::LogEntry&) { ++seen; });
  for (int i = 0; i < 600; ++i) bam::log_info("wpis " + std::to_string(i));
  auto recent = log.recent();
  CHECK(recent.size() == 500);
  CHECK(recent.back().message == "wpis 599");
  CHECK(seen == 600);
  log.clear_sinks();
}
