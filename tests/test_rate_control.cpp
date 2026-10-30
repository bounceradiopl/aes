// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include "bam/rate_control.hpp"

namespace {
// A device clock off by `ppm` delivers 10 ms blocks with ±2 ms scheduling jitter into a FIFO that
// a writer drains on the media clock in 1 ms steps (48 frames), so every fill reading carries up
// to 48 frames of quantisation noise; the controller trims the resampling ratio.
void simulate_drift(double ppm) {
  constexpr double kTarget = 480;
  bam::RateController rc(kTarget);
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> jitter(-0.002, 0.002);
  const double device_rate = 48000.0 * (1.0 + ppm * 1e-6);
  double produced = 0, mult = 1.0, last = 0, worst_after_60s = 0;
  for (int k = 1; k <= 360000; ++k) {  // 1 hour
    const double t = k * 0.01 + jitter(rng);
    const double dt = t - last;
    last = t;
    produced += device_rate * dt * mult;
    const double drained = std::floor(t * 1000.0) * 48.0;
    const double fifo = kTarget + produced - drained;
    mult = rc.update(fifo, dt);
    if (t > 60) worst_after_60s = std::max(worst_after_60s, std::abs(fifo - kTarget));
  }
  const double expected_ppm = (1.0 / (1.0 + ppm * 1e-6) - 1.0) * 1e6;
  MESSAGE("drift " << ppm << " ppm: worst fill error after 60 s " << worst_after_60s << " frames, correction " << rc.correction_ppm()
                   << " ppm (expected " << expected_ppm << ")");
  CHECK(worst_after_60s <= 96);
  CHECK(std::abs(rc.correction_ppm() - expected_ppm) < 5.0);
}
}  // namespace

TEST_CASE("rate controller locks a +100 ppm device to the media clock") { simulate_drift(100); }
TEST_CASE("rate controller locks a -100 ppm device to the media clock") { simulate_drift(-100); }

TEST_CASE("rate controller correction is limited") {
  bam::RateController rc(480, 1000);
  double m = 1;
  for (int i = 0; i < 1000; ++i) m = rc.update(48000, 0.01);
  CHECK(m == doctest::Approx(1.0 - 1000e-6));
}

TEST_CASE("drift resampler keeps a sine's level at ratio 1.0001") {
  bam::DriftResampler rs(1.0);
  constexpr int kBlock = 480;
  std::vector<float> in(kBlock * 2), out(kBlock * 4);
  double phase = 0, sum = 0;
  size_t counted = 0;
  bool finite = true;
  for (int b = 0; b < 500; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      const float v = static_cast<float>(0.5 * std::sin(phase));
      phase += 2 * 3.14159265358979323846 * 1000.0 / 48000.0;
      in[i * 2] = in[i * 2 + 1] = v;
    }
    size_t offset = 0;
    while (offset < kBlock) {
      size_t used = 0;
      const size_t n = rs.process(in.data() + offset * 2, kBlock - offset, out.data(), kBlock * 2, 1.0001, &used);
      offset += used;
      for (size_t i = 0; i < n * 2; ++i) {
        finite = finite && std::isfinite(out[i]);
        if (b > 20) {
          sum += double(out[i]) * out[i];
          ++counted;
        }
      }
      if (!used && !n) break;
    }
  }
  CHECK(finite);
  REQUIRE(counted > 0);
  const double rms_db = 10 * std::log10(sum / counted);
  const double expected_db = 20 * std::log10(0.5 / std::sqrt(2.0));
  MESSAGE("RMS " << rms_db << " dB, expected " << expected_db << " dB");
  CHECK(std::abs(rms_db - expected_db) < 0.1);
}

TEST_CASE("float conversion of left-aligned samples") {
  for (int32_t v : {0, 1 << 8, -(1 << 8), 0x7FFFFF00, static_cast<int32_t>(0x80000000), 0x12345600, -0x12345600})
    CHECK(bam::from_float(bam::to_float(v)) == v);
  CHECK(bam::to_float(0x40000000) == doctest::Approx(0.5));
  CHECK(bam::from_float(1.5f) == 0x7FFFFFFF);
  CHECK(bam::from_float(-2.0f) == static_cast<int32_t>(0x80000000));
  CHECK(bam::from_float(std::nanf("")) == 0);
}
