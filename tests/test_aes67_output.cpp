// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "../encoder/src/aes67_output.hpp"

namespace {
namespace bd = bam::encoder::detail;

constexpr float kScale = 1.0f / 2147483648.0f;
float sample(uint64_t pos, int ch) { return float(int32_t((pos * 2654435761u + uint64_t(ch) * 977) & 0x7fffff00u) - 0x40000000) * kScale; }

struct Captured { uint32_t timestamp; std::vector<int32_t> left, right; };

// Decodes the L24 payload of every packet (RTP header 12 bytes, then L R L R, 3 bytes each).
std::vector<Captured> run(bd::Aes67Output& out, uint64_t from, uint64_t to, uint64_t gen = 1) {
  std::vector<Captured> got;
  for (uint64_t now = from; now <= to; now += 12) {
    out.pump(now, gen, [&](std::span<const uint8_t> p) {
      Captured c;
      c.timestamp = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
      for (size_t i = 12; i + 6 <= p.size(); i += 6) {
        auto s24 = [&](size_t o) { return int32_t(uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8); };
        c.left.push_back(s24(i));
        c.right.push_back(s24(i + 3));
      }
      got.push_back(std::move(c));
    });
  }
  return got;
}

void feed(bd::Aes67Output& out, uint64_t from, uint64_t to) {
  std::vector<float> buf;
  for (uint64_t p = from; p < to; p += 480) {
    const size_t n = size_t(std::min<uint64_t>(480, to - p));
    buf.resize(n * 2);
    for (size_t f = 0; f < n; ++f) { buf[f * 2] = sample(p + f, 0); buf[f * 2 + 1] = sample(p + f, 1); }
    out.write(buf.data(), n, p);
  }
}
}  // namespace

TEST_CASE("aes67 output: packet T carries the content of mix position T - D, bit for bit") {
  bd::Aes67Output out(1000, 0x1234);
  out.set_delay(2000);
  feed(out, 100000, 120000);
  const auto pkts = run(out, 100000 + 2000 + 600, 100000 + 2000 + 9000);  // after the first fade-in
  REQUIRE(pkts.size() > 100);
  for (const auto& c : pkts) {
    if (c.timestamp < 100000 + 2000 + 600 + 240 + 48) continue;  // the start fades in over 240 frames
    for (size_t f = 0; f < c.left.size(); ++f) {
      const uint64_t src = uint64_t(c.timestamp) + f - 2000;
      CHECK(c.left[f] == (int32_t(double(sample(src, 0)) * 2147483648.0) & int32_t(0xffffff00)));
      CHECK(c.right[f] == (int32_t(double(sample(src, 1)) * 2147483648.0) & int32_t(0xffffff00)));
    }
  }
  CHECK(out.stats().gaps == 0);
}

TEST_CASE("aes67 output: a longer delay never repeats content; it fades out, holds silence, fades in") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 48000);
  run(out, 2000, 20000);
  out.set_delay(1000 + 4320);  // processing switched on: about 90 ms more
  const auto pkts = run(out, 20012, 40000);
  // no output position after the change carries a source position that was already sent
  bool silent_seen = false;
  for (const auto& c : pkts)
    for (size_t f = 0; f < c.left.size(); ++f) silent_seen |= c.left[f] == 0 && c.right[f] == 0;
  CHECK(silent_seen);
  // and from 240 frames after the hold ends the content is T - 5320 again
  const auto& last = pkts.back();
  const uint64_t src = uint64_t(last.timestamp) - 5320;
  CHECK(last.left[0] == (int32_t(double(sample(src, 0)) * 2147483648.0) & int32_t(0xffffff00)));
}

TEST_CASE("aes67 output: content that never arrived is silence and counts as a gap") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 10000);  // nothing after 10000
  const auto pkts = run(out, 1000, 16000);
  CHECK(out.stats().gaps > 0);
  CHECK(pkts.back().left[0] == 0);
  CHECK(out.stats().packets == pkts.size());
}

TEST_CASE("aes67 output: a clock step re-aligns without a burst of packets") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 20000);
  run(out, 1000, 10000, 1);
  const size_t before = out.stats().packets;
  run(out, 500000, 500048, 2);  // the clock jumped far ahead
  CHECK(out.stats().packets - before <= 2);
}

namespace {
bool carries(const Captured& c, size_t f, uint64_t src) {
  return c.left[f] == (int32_t(double(sample(src, 0)) * 2147483648.0) & int32_t(0xffffff00)) &&
         c.right[f] == (int32_t(double(sample(src, 1)) * 2147483648.0) & int32_t(0xffffff00));
}
}  // namespace

TEST_CASE("aes67 output: a delay set again while fading or holding: the newest wins, silence is no gap") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 60000);
  run(out, 2000, 20000);
  out.set_delay(5320);
  run(out, 20012, 20100);  // fading out
  out.set_delay(3000);
  run(out, 20112, 21000);  // holding
  out.set_delay(4000);
  const auto pkts = run(out, 21012, 40000);
  CHECK(out.delay() == 4000);
  // the hold ends once T - 4000 reaches the content not sent yet (~19290): resumes near 23290
  size_t first_full = 0;
  for (const auto& c : pkts) {
    if (!carries(c, 0, c.timestamp - 4000)) continue;
    first_full = c.timestamp;
    break;
  }
  CHECK(first_full > 23290);
  CHECK(first_full < 23290 + 240 + 2 * 48);
  for (const auto& c : pkts)
    if (c.timestamp >= first_full)
      for (size_t f = 0; f < c.left.size(); ++f) CHECK(carries(c, f, c.timestamp + f - 4000));
  CHECK(out.stats().gaps == 0);
}

TEST_CASE("aes67 output: a clock step while holding re-aligns with the newest delay") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 60000);
  run(out, 2000, 20000, 1);
  out.set_delay(5320);
  run(out, 20012, 20600, 1);  // faded out, holding until ~24600
  const auto pkts = run(out, 21000, 30000, 2);
  for (const auto& c : pkts)
    if (c.timestamp >= 21000 + 240 + 48)
      for (size_t f = 0; f < c.left.size(); ++f) CHECK(carries(c, f, c.timestamp + f - 5320));
  CHECK(out.stats().gaps == 0);
}

TEST_CASE("aes67 output: missing content counts at most one gap per packet") {
  bd::Aes67Output out(4000, 1);  // 192 frames per packet
  out.set_delay(1000);
  const auto pkts = run(out, 1000, 20000);  // nothing written at all
  CHECK(out.frames_per_packet() == 192);
  CHECK(out.stats().gaps > 0);
  CHECK(out.stats().gaps <= pkts.size() + 2);  // the fill runs one packet ahead of the sending
}

TEST_CASE("aes67 output: after a clock step back a delay change holds only the difference") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 98000, 112000);
  run(out, 100000, 110000, 1);
  feed(out, 18000, 40000);
  run(out, 20000, 25000, 2);  // the clock stepped back 90000 frames
  out.set_delay(1500);
  const auto pkts = run(out, 25012, 35000, 2);
  size_t silent = 0;
  for (const auto& c : pkts)
    for (size_t f = 0; f < c.left.size(); ++f) silent += c.left[f] == 0 && c.right[f] == 0;
  CHECK(silent >= 500);
  CHECK(silent < 1000);
  const auto& last = pkts.back();
  CHECK(carries(last, 0, last.timestamp - 1500));
}

TEST_CASE("aes67 output: a stalled sender with a longer delay pending never repeats content") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 60000);
  run(out, 2000, 20000);  // sent up to source ~19048
  out.set_delay(11000);
  const auto pkts = run(out, 26000, 40000);  // the sender stalled 6000 frames
  for (const auto& c : pkts)
    if (c.timestamp < 30000)
      for (size_t f = 0; f < c.left.size(); ++f) CHECK((c.left[f] == 0 && c.right[f] == 0));
  const auto& last = pkts.back();
  CHECK(carries(last, 0, last.timestamp - 11000));
  CHECK(out.stats().gaps == 0);
}
