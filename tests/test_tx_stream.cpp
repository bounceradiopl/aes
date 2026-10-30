// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <vector>

#include "bam/constants.hpp"
#include "bam/rtp.hpp"
#include "bam/tx_stream.hpp"

namespace {
struct RingStorage {
  std::vector<int32_t> samples = std::vector<int32_t>(bam::kRingFrames, 0);
  std::vector<uint32_t> tags = std::vector<uint32_t>(bam::kBlocksPerRing, 0);
  bam::ChannelRing ring() { return bam::ChannelRing(samples.data(), tags.data()); }
};

struct Captured {
  std::vector<std::vector<uint8_t>> packets;
  bam::PacketSink sink() {
    return [this](std::span<const uint8_t> p) { packets.emplace_back(p.begin(), p.end()); };
  }
};

void fill_ramp(bam::ChannelRing ring, uint64_t start, size_t n, int32_t offset) {
  std::vector<int32_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<int32_t>((start + i) << 8) + offset;
  ring.write(start, v.data(), n);
}
}  // namespace

TEST_CASE("first pump sends the packet that just completed") {
  RingStorage l, r;
  fill_ramp(l.ring(), 0, 2000, 0);
  fill_ramp(r.ring(), 0, 2000, 0x1000);
  bam::TxConfig cfg;
  cfg.frames_per_packet = 48;
  cfg.ssrc = 0x55;
  bam::TxStream tx(cfg, l.ring(), r.ring());
  Captured cap;
  CHECK(tx.pump(480, cap.sink()) == 1);
  REQUIRE(cap.packets.size() == 1);
  CHECK(cap.packets[0].size() == bam::kRtpHeaderSize + 48 * 6);
  auto v = bam::parse_rtp(cap.packets[0]);
  REQUIRE(v);
  CHECK(v->header.timestamp == 432);
  CHECK(v->header.payload_type == 98);
  CHECK(v->header.ssrc == 0x55);
  int32_t dl[48], dr[48];
  int32_t* out[] = {dl, dr};
  bam::decode_pcm(bam::Encoding::L24, v->payload.data(), 48, 2, out);
  CHECK(dl[0] == (432 << 8));
  CHECK(dl[47] == (479 << 8));
  CHECK(dr[0] == (432 << 8) + 0x1000);

  const uint16_t seq0 = v->header.sequence;
  CHECK(tx.pump(500, cap.sink()) == 0);
  CHECK(tx.pump(576, cap.sink()) == 2);
  REQUIRE(cap.packets.size() == 3);
  auto v1 = bam::parse_rtp(cap.packets[1]);
  auto v2 = bam::parse_rtp(cap.packets[2]);
  CHECK(v1->header.timestamp == 480);
  CHECK(v2->header.timestamp == 528);
  CHECK(v1->header.sequence == static_cast<uint16_t>(seq0 + 1));
  CHECK(v2->header.sequence == static_cast<uint16_t>(seq0 + 2));
  CHECK(tx.stats().packets == 3);
  CHECK(tx.stats().bytes == 3 * (12 + 288));
  CHECK(tx.stats().errors == 0);
}

TEST_CASE("long stall skips packets and counts errors") {
  RingStorage l, r;
  bam::TxConfig cfg;
  bam::TxStream tx(cfg, l.ring(), r.ring());
  Captured cap;
  tx.pump(576, cap.sink());  // next = 576
  cap.packets.clear();
  CHECK(tx.pump(576 + 1440, cap.sink()) == 1);  // 30 ms late → skip 29 packets, send the latest
  CHECK(tx.stats().errors == 29);
  auto v = bam::parse_rtp(cap.packets[0]);
  CHECK(v->header.timestamp == 576 + 1440 - 48);
}

TEST_CASE("a hiccup of up to 10 ms is caught up with a burst, without errors") {
  RingStorage l, r;
  bam::TxConfig cfg;
  bam::TxStream tx(cfg, l.ring(), r.ring());
  Captured cap;
  tx.pump(480, cap.sink());
  cap.packets.clear();
  CHECK(tx.pump(480 + 96, cap.sink()) == 2);
  CHECK(tx.pump(576 + 480, cap.sink()) == 10);  // 9 ms late: all ten packets, in order
  CHECK(tx.stats().errors == 0);
  CHECK(bam::parse_rtp(cap.packets[2])->header.timestamp == 576);
  CHECK(bam::parse_rtp(cap.packets.back())->header.timestamp == 576 + 9 * 48);
}

TEST_CASE("reset re-aligns without errors") {
  RingStorage l, r;
  bam::TxConfig cfg;
  bam::TxStream tx(cfg, l.ring(), r.ring());
  Captured cap;
  tx.pump(480, cap.sink());
  tx.reset();
  cap.packets.clear();
  CHECK(tx.pump(48000 * 10, cap.sink()) == 1);
  CHECK(tx.stats().errors == 0);
}

TEST_CASE("64-frame packets are aligned and gain applies to a copy") {
  RingStorage l, r;
  fill_ramp(l.ring(), 0, 1000, 0);
  bam::TxConfig cfg;
  cfg.frames_per_packet = 64;
  cfg.gain = 0.5;
  bam::TxStream tx(cfg, l.ring(), r.ring());
  Captured cap;
  tx.pump(700, cap.sink());
  auto v = bam::parse_rtp(cap.packets.at(0));
  CHECK(v->header.timestamp % 64 == 0);
  CHECK(v->header.timestamp == 576);
  int32_t dl[64], dr[64];
  int32_t* out[] = {dl, dr};
  bam::decode_pcm(bam::Encoding::L24, v->payload.data(), 64, 2, out);
  CHECK(dl[0] == (576 << 7));
  std::vector<int32_t> original(1);
  l.ring().read(576, original.data(), 1);
  CHECK(original[0] == (576 << 8));
}
