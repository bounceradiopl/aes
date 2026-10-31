// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "bam/render_pacer.hpp"

namespace {
// The Windows shared-mode engine takes one device period from the buffer per pass and signals the
// event; the bridge thread wakes 0.2-3 ms later and refills. Fewer frames than a period queued at
// a pass is an audible gap. The source (a subscription) is complete up to "now - 5 ms".
struct Result {
  long passes = 0, gaps = 0;
  double correction_ppm = 0;
};

Result run_device(uint32_t device_rate, double drift_ppm, double seconds) {
  const uint32_t period = device_rate / 100;  // 10 ms
  const uint32_t buffer = device_rate / 25;   // 40 ms
  bam::RenderPacer pacer(device_rate, period);
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> wake(0.0002, 0.003);
  const double pass_s = period / (device_rate * (1.0 + drift_ppm * 1e-6));
  std::vector<float> out;
  uint32_t padding = 0;
  Result res;
  auto read = [](uint64_t pos, int32_t* l, int32_t* r, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) l[i] = r[i] = static_cast<int32_t>(((pos + i) & 0xFFFF) << 12);
  };
  for (double t = pass_s; t < seconds; t += pass_s) {
    ++res.passes;
    if (padding < period) {
      if (t > 0.1) ++res.gaps;  // the very first pass may find the buffer still empty
      padding = 0;
    } else {
      padding -= period;
    }
    const double tw = t + wake(rng);
    const uint64_t ready = static_cast<uint64_t>(tw * 48000.0) - 240;
    const size_t n = pacer.step(ready, 1, padding, buffer - padding, tw, read, out);
    CHECK(out.size() == n * 2);
    padding += static_cast<uint32_t>(n);
  }
  res.correction_ppm = pacer.correction_ppm();
  return res;
}

void check_device(uint32_t rate, double ppm) {
  const Result r = run_device(rate, ppm, 120.0);
  // a faster card takes more frames per media second: the pacer produces more, same sign as the drift
  const double expected = ppm;
  MESSAGE(rate << " Hz, " << ppm << " ppm: " << r.passes << " passes, " << r.gaps << " gaps, correction " << r.correction_ppm
                << " ppm (expected about " << expected << ")");
  CHECK(r.gaps == 0);
  CHECK(std::abs(r.correction_ppm - expected) < 25.0);
}
}  // namespace

TEST_CASE("render pacer keeps a 48 kHz card fed without gaps (drift -100 / 0 / +100 ppm)") {
  check_device(48000, -100);
  check_device(48000, 0);
  check_device(48000, 100);
}

TEST_CASE("render pacer keeps a 44.1 kHz card fed without gaps") { check_device(44100, 80); }

TEST_CASE("render pacer target is at least two device periods") {
  bam::RenderPacer p48(48000, 480);
  CHECK(p48.target_frames() >= 960);
  bam::RenderPacer small(48000, 144);  // 3 ms period: 20 ms minimum
  CHECK(small.target_frames() >= 960);
}
