// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

#include "bam/constants.hpp"
#include "bam/shm_layout.hpp"
#include "bam/timeline.hpp"

namespace {
struct RingStorage {
  std::vector<int32_t> samples = std::vector<int32_t>(bam::kRingFrames, 0);
  std::vector<uint32_t> tags = std::vector<uint32_t>(bam::kBlocksPerRing, 0);
  bam::ChannelRing ring{samples.data(), tags.data()};
};

std::vector<int32_t> ramp(int32_t start, size_t n) {
  std::vector<int32_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = start + static_cast<int32_t>(i);
  return v;
}
}  // namespace

TEST_CASE("lap tag never zero") {
  CHECK(bam::lap_tag(0) == 1);
  CHECK(bam::lap_tag(65535) == 1);
  CHECK(bam::lap_tag(65536) == 2);
}

TEST_CASE("ring write/read with wrap") {
  RingStorage s;
  const uint64_t start = 3 * uint64_t(bam::kRingFrames) + 65530;
  auto data = ramp(100, 100);
  s.ring.write(start, data.data(), data.size());
  std::vector<int32_t> out(100);
  s.ring.read(start, out.data(), out.size());
  CHECK(out == data);
}

TEST_CASE("unwritten region reads zeros") {
  RingStorage s;
  std::vector<int32_t> out(64, 7);
  s.ring.read(1000, out.data(), out.size());
  for (auto v : out) CHECK(v == 0);
}

TEST_CASE("stale lap reads zeros") {
  RingStorage s;
  auto data = ramp(1, 32);
  s.ring.write(160, data.data(), data.size());
  std::vector<int32_t> out(32, 7);
  s.ring.read(160 + bam::kRingFrames, out.data(), out.size());
  for (auto v : out) CHECK(v == 0);
}

TEST_CASE("partial block write in a new lap clears the rest of the block") {
  RingStorage s;
  auto old_data = ramp(1000, 16);
  s.ring.write(32, old_data.data(), 16);
  auto fresh = ramp(5, 4);
  s.ring.write(32 + bam::kRingFrames + 4, fresh.data(), 4);
  std::vector<int32_t> out(16, 7);
  s.ring.read(32 + bam::kRingFrames, out.data(), 16);
  for (int i = 0; i < 16; ++i) {
    if (i >= 4 && i < 8) {
      CHECK(out[i] == 5 + (i - 4));
    } else {
      CHECK(out[i] == 0);
    }
  }
}

TEST_CASE("null source writes valid silence") {
  RingStorage s;
  auto data = ramp(9, 16);
  s.ring.write(48, data.data(), 16);
  s.ring.write(48, nullptr, 16);
  std::vector<int32_t> out(16, 7);
  s.ring.read(48, out.data(), 16);
  for (auto v : out) CHECK(v == 0);
}

TEST_CASE("shared memory view layout") {
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  std::vector<std::byte> mem(bam::ShmLayout::total_bytes());
  bam::ShmView::initialize(mem.data(), 77);
  bam::ShmView view(mem.data());
  CHECK(view.compatible());
  CHECK(view.header().service_generation.load() == 77);
  CHECK(view.header().sample_rate == 48000);

  CHECK(bam::ShmLayout::header_bytes() % 4096 == 0);
  CHECK(bam::ShmLayout::net_in_samples_offset(0) >= bam::ShmLayout::header_bytes());
  CHECK(bam::ShmLayout::asio_out_samples_offset(0) >= bam::ShmLayout::net_in_samples_offset(63) + bam::kRingFrames * 4);
  CHECK(bam::ShmLayout::win_src_samples_offset(0) >= bam::ShmLayout::asio_out_samples_offset(63) + bam::kRingFrames * 4);
  CHECK(bam::ShmLayout::win_src_tags_offset(31) + bam::kBlocksPerRing * 4 <= bam::ShmLayout::total_bytes());

  auto data = ramp(42, 10);
  std::vector<int32_t> out(10);
  view.asio_out(5).write(1000, data.data(), 10);
  view.asio_out(5).read(1000, out.data(), 10);
  CHECK(out == data);
  view.net_in(5).read(1000, out.data(), 10);
  CHECK(out[0] == 0);
  view.win_src(31).write(500, data.data(), 10);
  view.win_src(31).read(500, out.data(), 10);
  CHECK(out == data);

  // source_ring dispatches by kind
  CHECK(view.source_ring(bam::ShmSource::Asio, 5).valid());
  view.source_ring(bam::ShmSource::Asio, 5).read(1000, out.data(), 10);
  CHECK(out == data);
  CHECK_FALSE(view.source_ring(bam::ShmSource::None, 0).valid());

  // ASIO route table seqlock
  view.set_asio_route(7, bam::ShmSource::Net, 3, 240);
  auto route = view.asio_route(7);
  CHECK(route.kind == bam::ShmSource::Net);
  CHECK(route.pair == 3);
  CHECK(route.delay == 240);
  CHECK(view.asio_route(0).kind == bam::ShmSource::None);

  // bridge request/report seqlocks, incl. a large payload and truncation
  view.write_request("{\"a\":1}");
  uint32_t ver = 0;
  CHECK(view.read_request(&ver) == "{\"a\":1}");
  std::string big(60000, 'x');
  view.write_report(big);
  CHECK(view.read_report(nullptr) == big);
  view.write_report(std::string(70000, 'y'));
  CHECK(view.read_report(nullptr).size() == bam::kBridgeBufferBytes - 1);

  view.set_device_name(3, "Kraków");
  CHECK(view.device_name(3) == "Kraków");
  CHECK(view.header().names_version.load() % 2 == 0);
  view.set_device_name(4, std::string(100, 'x'));
  CHECK(view.device_name(4).size() == 47);

  view.header().magic = 0;
  CHECK_FALSE(view.compatible());
}
