// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <vector>

#include "bam/mixer.hpp"
#include "bam/timeline.hpp"

namespace {
struct Ring {
  std::vector<int32_t> samples = std::vector<int32_t>(bam::kRingFrames);
  std::vector<uint32_t> tags = std::vector<uint32_t>(bam::kBlocksPerRing);
  bam::ChannelRing ring() { return bam::ChannelRing(samples.data(), tags.data()); }
};
int32_t a(uint64_t n) { return static_cast<int32_t>(((n * 7) & 0xFFFF) << 8); }
int32_t b(uint64_t n) { return static_cast<int32_t>(((n * 13 + 5) & 0xFFFF) << 8); }
void fill(bam::ChannelRing r, uint64_t from, uint64_t to, int32_t (*f)(uint64_t)) {
  std::vector<int32_t> v(to - from);
  for (uint64_t i = from; i < to; ++i) v[i - from] = f(i);
  r.write(from, v.data(), v.size());
}
}  // namespace

TEST_CASE("mixer sums two sources at 0 dB exactly and aligns their delays") {
  Ring al, ar, bl, br, ol, orr;
  fill(al.ring(), 0, 20000, a);
  fill(ar.ring(), 0, 20000, a);
  fill(bl.ring(), 0, 20000, b);
  fill(br.ring(), 0, 20000, b);
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.bus = 0;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  p.inputs = {{al.ring(), ar.ring(), 1.0, 0}, {bl.ring(), br.ring(), 1.0, 240}};
  p.max_delay = 240;
  mixer.set_plan({p});
  for (uint64_t now = 10000; now <= 20000; now += 48) mixer.process(now, 1);
  std::vector<int32_t> got(4000);
  ol.ring().read(12000, got.data(), got.size());
  int bad = 0;
  for (uint64_t i = 0; i < got.size(); ++i) bad += got[i] != a(12000 + i) + b(12000 + i);
  CHECK(bad == 0);
  CHECK(mixer.late() == 0);
}

TEST_CASE("mixer applies gain, mute and saturates") {
  Ring al, ar, bl, br, ol, orr;
  std::vector<int32_t> loud(20000, 0x7FFFFF00), half(20000, 0x40000000);
  al.ring().write(0, loud.data(), loud.size());
  ar.ring().write(0, half.data(), half.size());
  bl.ring().write(0, loud.data(), loud.size());
  br.ring().write(0, half.data(), half.size());
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  p.inputs = {{al.ring(), ar.ring(), 1.0, 0}, {bl.ring(), br.ring(), 1.0, 0}};
  mixer.set_plan({p});
  for (uint64_t now = 1000; now <= 8000; now += 48) mixer.process(now, 1);
  int32_t l = 0, r = 0;
  ol.ring().read(4000, &l, 1);
  orr.ring().read(4000, &r, 1);
  CHECK(l == bam::from_float(1.0f));      // two full-scale sources: clipped, not wrapped
  CHECK(r == bam::from_float(1.0f));      // 0.5 + 0.5
  p.inputs[1].gain = 0.0;                 // muted member
  p.inputs[0].gain = 0.5;
  mixer.set_plan({p});
  for (uint64_t now = 8000; now <= 16000; now += 48) mixer.process(now, 1);
  orr.ring().read(12000, &r, 1);
  CHECK(r == 0x20000000);
}

TEST_CASE("mixer restarts on a clock step without mixing the past") {
  Ring al, ar, ol, orr;
  fill(al.ring(), 0, 60000, a);
  fill(ar.ring(), 0, 60000, a);
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  p.inputs = {{al.ring(), ar.ring(), 0.5, 0}};
  mixer.set_plan({p});
  mixer.process(5000, 1);
  mixer.process(50000, 2);  // step: continue at "now", no 45 000-frame catch-up
  mixer.process(50480, 2);
  int32_t v = 1;
  ol.ring().read(20000, &v, 1);
  CHECK(v == 0);  // never mixed
  ol.ring().read(50100, &v, 1);
  CHECK(v == static_cast<int32_t>(a(50100) / 2));
}
