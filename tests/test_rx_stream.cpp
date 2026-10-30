// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <climits>
#include <vector>

#include "bam/constants.hpp"
#include "bam/gain.hpp"
#include "bam/meters.hpp"
#include "bam/rtp.hpp"
#include "bam/rx_stream.hpp"
#include "bam/sdp.hpp"

namespace {
struct RingStorage {
  std::vector<int32_t> samples = std::vector<int32_t>(bam::kRingFrames, 0);
  std::vector<uint32_t> tags = std::vector<uint32_t>(bam::kBlocksPerRing, 0);
  bam::ChannelRing ring() { return bam::ChannelRing(samples.data(), tags.data()); }
};

bam::SdpStream make_sdp(int channels, uint32_t fpp = 48) {
  bam::SdpStream s;
  s.multicast_address = "239.69.100.1";
  s.port = 5004;
  s.payload_type = 98;
  s.encoding = bam::Encoding::L24;
  s.sample_rate = 48000;
  s.channels = channels;
  s.frames_per_packet = fpp;
  s.ptime_ms = fpp / 48.0;
  return s;
}

// Packet whose samples are value(ts + i) = (ts + i) << 8 (+ 0x100000 on the right channel).
std::vector<uint8_t> make_packet(uint32_t ts, uint16_t seq, int channels, uint32_t frames, uint8_t pt = 98) {
  std::vector<int32_t> l(frames), r(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    l[i] = static_cast<int32_t>((ts + i) << 8);
    r[i] = static_cast<int32_t>((ts + i + 0x100000) << 8);
  }
  const int32_t* planes[] = {l.data(), r.data()};
  std::vector<uint8_t> pkt(bam::kRtpHeaderSize + frames * channels * 3);
  bam::RtpHeader h;
  h.payload_type = pt;
  h.sequence = seq;
  h.timestamp = ts;
  h.ssrc = 1;
  bam::write_rtp_header(h, pkt.data());
  bam::encode_l24(planes, channels, frames, pkt.data() + bam::kRtpHeaderSize);
  return pkt;
}
}  // namespace

TEST_CASE("gain and meters") {
  CHECK(bam::db_to_gain(0) == doctest::Approx(1.0));
  CHECK(bam::db_to_gain(6.0206) == doctest::Approx(2.0).epsilon(1e-4));
  int32_t s[3] = {1000, INT32_MAX / 2 + 10, INT32_MIN / 2 - 10};
  bam::apply_gain(s, 3, 2.0);
  CHECK(s[0] == 2000);
  CHECK(s[1] == INT32_MAX);
  CHECK(s[2] == INT32_MIN);
  int32_t untouched[1] = {12345};
  bam::apply_gain(untouched, 1, 1.0);
  CHECK(untouched[0] == 12345);

  int32_t full[2] = {INT32_MAX, 0};
  CHECK(bam::peak_dbfs(full, 2) == doctest::Approx(0.0).epsilon(1e-6));
  int32_t half[1] = {1 << 30};
  CHECK(bam::peak_dbfs(half, 1) == doctest::Approx(-6.0206).epsilon(1e-3));
  int32_t zero[4] = {};
  CHECK(bam::peak_dbfs(zero, 4) == doctest::Approx(-120.0));
}

TEST_CASE("in-time packet lands at its timestamp") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  auto pkt = make_packet(1000, 1, 2, 48);
  rx.on_packet(pkt, 1050, 1);
  std::vector<int32_t> out(48);
  l.ring().read(1000, out.data(), 48);
  CHECK(out[0] == (1000 << 8));
  CHECK(out[47] == (1047 << 8));
  r.ring().read(1000, out.data(), 48);
  CHECK(out[0] == static_cast<int32_t>((1000 + 0x100000) << 8));
  auto st = rx.stats();
  CHECK(st.packets == 1);
  CHECK(st.bytes == pkt.size());
  CHECK(st.overruns == 0);
}

TEST_CASE("mono is duplicated, gain applied") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(1), l.ring(), r.ring(), 240, 2.0);
  rx.on_packet(make_packet(480, 1, 1, 48), 500, 1);
  std::vector<int32_t> a(48), b(48);
  l.ring().read(480, a.data(), 48);
  r.ring().read(480, b.data(), 48);
  CHECK(a == b);
  CHECK(a[0] == (480 << 9));
}

TEST_CASE("late and future packets are rejected") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  rx.on_packet(make_packet(1000, 1, 2, 48), 1000 + 48 + 240, 1);  // end == deadline → late
  rx.on_packet(make_packet(100000, 2, 2, 48), 1000, 2);            // > 0.5 s ahead
  std::vector<int32_t> out(48, 7);
  l.ring().read(1000, out.data(), 48);
  CHECK(out[0] == 0);
  CHECK(rx.stats().overruns == 2);
  CHECK(rx.stats().packets == 0);
}

TEST_CASE("wrong payload type is ignored") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  rx.on_packet(make_packet(1000, 1, 2, 48, 97), 1000, 1);
  CHECK(rx.stats().packets == 0);
  CHECK(rx.stats().overruns == 0);
}

TEST_CASE("sequence gaps") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  rx.on_packet(make_packet(0, 10, 2, 48), 48, 1);
  rx.on_packet(make_packet(48, 11, 2, 48), 96, 2);
  rx.on_packet(make_packet(144, 13, 2, 48), 192, 3);
  CHECK(rx.stats().seq_errors == 1);
}

TEST_CASE("duplicate packets are ignored") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  auto p0 = make_packet(0, 10, 2, 48);
  rx.on_packet(p0, 48, 1);
  rx.on_packet(p0, 49, 2);
  rx.on_packet(make_packet(48, 11, 2, 48), 96, 3);
  CHECK(rx.stats().seq_errors == 0);
  CHECK(rx.stats().packets == 2);
}

TEST_CASE("underruns counted at deadline") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  rx.on_tick(10000);  // nothing before first packet
  CHECK(rx.stats().underruns == 0);
  for (uint32_t i = 0; i < 10; ++i) rx.on_packet(make_packet(i * 48, static_cast<uint16_t>(i), 2, 48), i * 48 + 48, i);
  rx.on_tick(480 + 240 + 96);  // deadline 576, data up to 480 → 96 frames = 2 packets missing
  CHECK(rx.stats().underruns == 2);
  rx.on_tick(480 + 240 + 96);  // same deadline again: nothing new
  CHECK(rx.stats().underruns == 2);
  rx.on_tick(480 + 240 + 96 + 100000);  // >1 s gap: source gone, not counted
  CHECK(rx.stats().underruns == 2);
  rx.reset_stats();
  CHECK(rx.stats().underruns == 0);
}

TEST_CASE("signal presence") {
  RingStorage l, r;
  bam::RxStream rx(make_sdp(2), l.ring(), r.ring(), 240, 1.0);
  CHECK_FALSE(rx.has_signal(5));
  rx.on_packet(make_packet(0, 1, 2, 48), 48, 1'000'000'000);
  CHECK(rx.has_signal(1'500'000'000));
  CHECK_FALSE(rx.has_signal(2'100'000'000));
}
