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

Result run_device(uint32_t device_rate, double drift_ppm, double seconds, int channels = 2) {
  const uint32_t period = device_rate / 100;  // 10 ms
  const uint32_t buffer = device_rate / 25;   // 40 ms
  bam::RenderPacer pacer(device_rate, period, channels);
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> wake(0.0002, 0.003);
  const double pass_s = period / (device_rate * (1.0 + drift_ppm * 1e-6));
  std::vector<float> out;
  uint32_t padding = 0;
  Result res;
  // a ramp per channel, each channel offset so a channel mix-up would show
  auto read = [channels](uint64_t pos, uint32_t frames, float* interleaved) {
    for (uint32_t i = 0; i < frames; ++i)
      for (int c = 0; c < channels; ++c)
        interleaved[i * channels + c] = static_cast<float>(((pos + i + 4096u * c) & 0xFFFF) / 65536.0 - 0.5);
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
    CHECK(out.size() == n * static_cast<size_t>(channels));
    padding += static_cast<uint32_t>(n);
  }
  res.correction_ppm = pacer.correction_ppm();
  return res;
}

void check_device(uint32_t rate, double ppm, int channels = 2, double seconds = 120.0) {
  const Result r = run_device(rate, ppm, seconds, channels);
  // a faster card takes more frames per media second: the pacer produces more, same sign as the drift
  const double expected = ppm;
  MESSAGE(rate << " Hz, " << channels << " ch, " << ppm << " ppm: " << r.passes << " passes, " << r.gaps << " gaps, correction " << r.correction_ppm
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

TEST_CASE("render pacer feeds an 8-channel card without gaps") {
  const Result eight = run_device(48000, 60, 60.0, 8);
  MESSAGE("48000 Hz, 8 ch, 60 ppm: " << eight.passes << " passes, " << eight.gaps << " gaps, correction " << eight.correction_ppm << " ppm");
  CHECK(eight.gaps == 0);
  // at 60 s the loop (about 0.02 Hz) is still settling, so the 25 ppm check of check_device does not
  // apply; the channel count must not change the control at all
  const Result two = run_device(48000, 60, 60.0, 2);
  CHECK(eight.passes == two.passes);
  CHECK(eight.correction_ppm == doctest::Approx(two.correction_ppm).epsilon(1e-9));
}

TEST_CASE("render pacer keeps each channel in its place") {
  constexpr int kChannels = 8;
  bam::RenderPacer pacer(48000, 480, kChannels);
  auto read = [](uint64_t, uint32_t frames, float* interleaved) {
    for (uint32_t i = 0; i < frames; ++i)
      for (int c = 0; c < kChannels; ++c) interleaved[i * kChannels + c] = 0.05f * (c + 1) * (c % 2 ? -1.0f : 1.0f);
  };
  std::vector<float> out, all;
  for (int pass = 1; pass <= 200; ++pass) {  // 2 s, a device that takes everything
    const uint64_t ready = 480ull * pass + 4800;
    pacer.step(ready, 1, 0, 480, pass * 0.01, read, out);
    all.insert(all.end(), out.begin(), out.end());
  }
  const size_t frames = all.size() / kChannels;
  REQUIRE(frames > 48000);
  double worst = 0;
  for (size_t i = frames / 2; i < frames; ++i)  // well past the resampler's start
    for (int c = 0; c < kChannels; ++c)
      worst = std::max(worst, std::abs(all[i * kChannels + c] - 0.05 * (c + 1) * (c % 2 ? -1.0 : 1.0)));
  MESSAGE("worst channel error " << worst);
  CHECK(worst < 1e-4);
}
