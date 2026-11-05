// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <random>
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
  mixer.process(5000, 1);  // a new bus starts kMixMarginFrames before its limit: its consumers see no gap
  mixer.process(5480, 1);
  int32_t v = 1;
  ol.ring().read(5000 - bam::kMixMarginFrames - 1, &v, 1);
  CHECK(v == 0);  // before the start: never mixed
  ol.ring().read(5000 - bam::kMixMarginFrames + 1, &v, 1);
  CHECK(v != 0);  // mixed (fading in) from the start on
  ol.ring().read(5400, &v, 1);
  CHECK(v == static_cast<int32_t>(a(5400) / 2));
  mixer.process(50000, 2);  // step: continue at "now", no 45 000-frame catch-up
  mixer.process(50480, 2);
  ol.ring().read(20000, &v, 1);
  CHECK(v == 0);  // never mixed
  ol.ring().read(50000 - bam::kMixMarginFrames - 1, &v, 1);
  CHECK(v == 0);
  // after the step the bus restarts kMixMarginFrames back as well, at the level it had (no new fade-in)
  ol.ring().read(50000 - bam::kMixMarginFrames, &v, 1);
  CHECK(v == static_cast<int32_t>(a(50000 - bam::kMixMarginFrames) / 2));
  ol.ring().read(50100, &v, 1);
  CHECK(v == static_cast<int32_t>(a(50100) / 2));
}

TEST_CASE("mixer keeps the bus peak when a process() call mixes no new block") {
  Ring al, ar, ol, orr;
  std::vector<int32_t> half(20000, 0x40000000);
  al.ring().write(0, half.data(), half.size());
  ar.ring().write(0, half.data(), half.size());
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.bus = 0;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  p.inputs = {{al.ring(), ar.ring(), 1.0, 0}};
  mixer.set_plan({p});
  mixer.process(1000, 1);  // first call: the margin before "now" is mixed (fading in)
  mixer.process(1048, 1);  // one more full 48-frame block mixed
  const float peak = mixer.peak_db(0, 0);
  CHECK(peak > -120.0f);  // playing, not silent
  mixer.process(1060, 1);  // only 12 more frames of "now": less than one block, nothing new mixed
  CHECK(mixer.peak_db(0, 0) == peak);  // must keep the last real peak, not reset to -120
}

namespace {
// Output frames [from, to) of a bus ring.
std::vector<int32_t> span(Ring& r, uint64_t from, uint64_t to) {
  std::vector<int32_t> v(to - from);
  r.ring().read(from, v.data(), v.size());
  return v;
}
constexpr int64_t kMaxStep = 2147483648LL / bam::kMixRampFrames + 1;  // full scale over the ramp, plus 1 LSB
}  // namespace

TEST_CASE("mixer ramps gain and mute changes over 5 ms instead of stepping") {
  Ring xl, xr, yl, yr, ol, orr;
  std::vector<int32_t> loud(40000, 0x7FFFFF00);
  xl.ring().write(0, loud.data(), loud.size());
  xr.ring().write(0, loud.data(), loud.size());
  yl.ring().write(0, nullptr, 40000);  // a silent second member
  yr.ring().write(0, nullptr, 40000);
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.bus = 3;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  p.inputs = {{xl.ring(), xr.ring(), 1.0, 0}, {yl.ring(), yr.ring(), 0.5, 0}};
  mixer.set_plan({p});
  uint64_t now = 1000;
  for (; now <= 8000; now += 48) mixer.process(now, 1);
  CHECK(span(ol, 6000, 7900) == std::vector<int32_t>(1900, 0x7FFFFF00));
  const uint64_t muted_at = now;
  // mute X; the plan also lists the members in another order: levels follow the member, not the slot
  p.inputs = {{yl.ring(), yr.ring(), 0.5, 0}, {xl.ring(), xr.ring(), 0.0, 0}};
  mixer.set_plan({p});
  for (; now <= 16000; now += 48) mixer.process(now, 1);
  auto down = span(ol, muted_at - 500, muted_at + 1000);
  int64_t max_step = 0;
  bool monotonic = true;
  for (size_t i = 1; i < down.size(); ++i) {
    max_step = std::max<int64_t>(max_step, static_cast<int64_t>(down[i - 1]) - down[i]);
    if (down[i] > down[i - 1]) monotonic = false;
  }
  CHECK(monotonic);
  CHECK(max_step <= kMaxStep);
  CHECK(down.front() == 0x7FFFFF00);  // still at full level before the change
  CHECK(span(ol, muted_at + 1000, 15000) == std::vector<int32_t>(15000 - muted_at - 1000, 0));  // exactly silent after
  // the ramp takes 240 frames: count the frames strictly between full level and silence
  size_t between = 0;
  for (int32_t v : down) between += v != 0 && v != 0x7FFFFF00;
  CHECK(between == bam::kMixRampFrames - 1);

  // unmute: the same ramp upwards, then exactly the input again
  const uint64_t unmuted_at = now;
  p.inputs[1].gain = 1.0;
  mixer.set_plan({p});
  for (; now <= 24000; now += 48) mixer.process(now, 1);
  auto up = span(orr, unmuted_at - 500, unmuted_at + 1000);
  max_step = 0;
  monotonic = true;
  for (size_t i = 1; i < up.size(); ++i) {
    max_step = std::max<int64_t>(max_step, static_cast<int64_t>(up[i]) - up[i - 1]);
    if (up[i] < up[i - 1]) monotonic = false;
  }
  CHECK(monotonic);
  CHECK(max_step <= kMaxStep);
  CHECK(up.front() == 0);
  CHECK(span(orr, unmuted_at + 1000, 23000) == std::vector<int32_t>(23000 - unmuted_at - 1000, 0x7FFFFF00));
  CHECK(mixer.late() == 0);
}

TEST_CASE("mixer: a member that joins fades in, and a constant 0 dB mix stays bit-exact") {
  Ring al, ar, bl, br, ol, orr;
  fill(al.ring(), 0, 40000, a);
  fill(ar.ring(), 0, 40000, a);
  std::vector<int32_t> half(40000, 0x40000000);  // a + half never clips: the output minus a is B's share
  bl.ring().write(0, half.data(), half.size());
  br.ring().write(0, half.data(), half.size());
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  p.inputs = {{al.ring(), ar.ring(), 1.0, 0}};
  mixer.set_plan({p});
  uint64_t now = 1000;
  for (; now <= 10000; now += 48) mixer.process(now, 1);
  auto got = span(ol, 2000, 9900);
  int bad = 0;
  for (size_t i = 0; i < got.size(); ++i) bad += got[i] != a(2000 + i);
  CHECK(bad == 0);  // one member at 0 dB: bit-exact
  const uint64_t joined_at = now;
  p.inputs.push_back({bl.ring(), br.ring(), 1.0, 0});  // B joins at 0 dB: must not start at full level
  mixer.set_plan({p});
  for (; now <= 20000; now += 48) mixer.process(now, 1);
  got = span(ol, joined_at - 200, joined_at + 600);
  int64_t prev = 0, max_step = 0;
  bool monotonic = true;
  for (size_t i = 0; i < got.size(); ++i) {
    const int64_t share = static_cast<int64_t>(got[i]) - a(joined_at - 200 + i);
    max_step = std::max(max_step, share - prev);
    if (share < prev) monotonic = false;
    prev = share;
  }
  CHECK(monotonic);
  CHECK(max_step <= kMaxStep);
  CHECK(got.front() == a(joined_at - 200));  // B not yet audible
  got = span(ol, joined_at + 600, 19000);
  bad = 0;
  for (size_t i = 0; i < got.size(); ++i) bad += got[i] != a(joined_at + 600 + i) + 0x40000000;
  CHECK(bad == 0);  // both at 0 dB after the fade: bit-exact sum again
}

TEST_CASE("mixer: a bus taken over by another destination starts clean") {
  Ring al, ar, bl, br, ol, orr;
  std::vector<int32_t> neg(40000, -0x10000000), pos(40000, 0x20000000);
  al.ring().write(0, neg.data(), neg.size());
  ar.ring().write(0, neg.data(), neg.size());
  bl.ring().write(0, pos.data(), pos.size());
  br.ring().write(0, pos.data(), pos.size());
  bam::Mixer mixer;
  bam::MixBusPlan pa;
  pa.bus = 0;
  pa.owner = 1;  // destination A
  pa.out_left = ol.ring();
  pa.out_right = orr.ring();
  pa.inputs = {{al.ring(), ar.ring(), 1.0, 0}};
  mixer.set_plan({pa});
  uint64_t now = 1000;
  for (; now <= 8000; now += 48) mixer.process(now, 1);
  // bus 0 now belongs to destination B, whose slowest member is 20 ms late
  bam::MixBusPlan pb = pa;
  pb.owner = 2;
  pb.inputs = {{bl.ring(), br.ring(), 1.0, 960}};
  pb.max_delay = 960;
  mixer.set_plan({pb});
  const uint64_t first_read = now - 960 - bam::kMixMarginFrames;  // what B's consumer reads first (D_mix behind)
  for (; now <= 16000; now += 48) mixer.process(now, 1);
  auto got = span(ol, first_read, 14000);
  int a_audio = 0;
  for (int32_t v : got) a_audio += v < 0;
  CHECK(a_audio == 0);  // nothing of A's mix left in what B plays
  CHECK(got.back() == 0x20000000);
}

// The real mix_loop (engine/src/stream_workers.cpp) wakes process() on a fixed 1 ms schedule, but
// under OS scheduling jitter the wake can land late; process() then drains every outstanding
// block at once, so the mixer itself always catches up. The danger is the consumer: it never
// calls process(), it just reads D_mix = max_delay + kMixMarginFrames behind its own clock,
// whenever it likes. If the margin does not comfortably exceed the mixer thread's wake-up
// jitter, a read landing just before the mixer's next, late wake-up finds frames not yet mixed
// (ruling R16: this is what Windows CI's mixer_late counter caught at kMixMarginFrames == 96).
TEST_CASE("mixer stays ahead of the margin through a jittered mixer-thread wake-up") {
  Ring ml, mr, ol, orr;
  constexpr int32_t kValue = 0x11223300;  // a constant, non-zero, non-clipping member signal
  const std::vector<int32_t> loud(54000, kValue);  // covers the whole run, one ring lap (< 65536)
  ml.ring().write(0, loud.data(), loud.size());
  mr.ring().write(0, loud.data(), loud.size());
  bam::Mixer mixer;
  bam::MixBusPlan p;
  p.out_left = ol.ring();
  p.out_right = orr.ring();
  constexpr uint32_t kMemberDelay = 240;  // 5 ms: a plausible D_src for a mixed member
  p.inputs = {{ml.ring(), mr.ring(), 1.0, kMemberDelay}};
  p.max_delay = kMemberDelay;
  mixer.set_plan({p});

  std::mt19937 rng(7);  // deterministic: same wake-up sequence every run
  // The wake-up lateness itself drifts from one wake to the next (a loaded scheduler's jitter
  // does not jump between "on time" and "2.5 ms late" from one 1 ms tick to the next); model it
  // as a bounded walk within [0, 2.5] ms, each step at most 0.6 ms.
  std::uniform_real_distribution<double> jitter_step_ms(-0.6, 0.6);
  constexpr double kMaxJitterMs = 2.5;
  constexpr double kPeriodMs = 1.0;  // mix_loop's nominal cadence
  constexpr double kFramesPerMs = 48.0;
  constexpr uint64_t kStart = 4000;  // clear of frame 0 so "now - delay - margin" never underflows
  constexpr int kWakes = 1000;       // ~1 s of simulated mixer-thread wakes

  double schedule_ms = 0.0, jitter_ms = 0.0;
  uint64_t checks = 0, bad_reads = 0;
  for (int i = 0; i < kWakes; ++i) {
    schedule_ms += kPeriodMs;  // the nominal schedule never slips, however late a wake lands
    jitter_ms = std::clamp(jitter_ms + jitter_step_ms(rng), 0.0, kMaxJitterMs);
    const uint64_t now = kStart + static_cast<uint64_t>((schedule_ms + jitter_ms) * kFramesPerMs);
    // Skip the member's own join fade-in (kMixRampFrames, unrelated to the margin under test):
    // past that, a 0 dB member plays bit-exact and any mismatch is purely a margin/jitter miss.
    if (i > 20) {
      // The consumer reads at the single most adversarial instant of the gap just ending: the
      // moment right before this (possibly late) wake finally catches the mix up.
      const uint64_t consumer_now = now - 1;
      const uint64_t pos = consumer_now - kMemberDelay - bam::kMixMarginFrames;
      int32_t v = 0;
      ol.ring().read(pos, &v, 1);
      ++checks;
      if (v != kValue) ++bad_reads;
    }
    mixer.process(now, 1);
  }
  MESSAGE(checks << " consumer reads at the margin, " << bad_reads << " not yet mixed; late() = " << mixer.late());
  CHECK(bad_reads == 0);
  CHECK(mixer.late() == 0);
}
