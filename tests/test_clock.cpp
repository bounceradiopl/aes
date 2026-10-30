// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <random>

#include "bam/clock.hpp"

TEST_CASE("identity mapping") {
  bam::MediaClock c;
  CHECK(c.to_ptp(123456789) == 123456789);
  CHECK(c.to_local(987654321) == 987654321);
  CHECK(c.rate() == 1.0);
}

TEST_CASE("sample conversions") {
  CHECK(bam::MediaClock::sample_from_ptp(1'000'000'000) == 48000);
  CHECK(bam::MediaClock::sample_from_ptp(20'833) == 0);
  CHECK(bam::MediaClock::sample_from_ptp(20'834) == 1);
  CHECK(bam::MediaClock::ptp_from_sample(1) == 20'834);
  CHECK(bam::MediaClock::ptp_from_sample(48000) == 1'000'000'000);

  std::mt19937_64 rng(42);
  for (int i = 0; i < 10000; ++i) {
    uint64_t s = (rng() % (uint64_t(1) << 47)) + 1;
    int64_t ns = bam::MediaClock::ptp_from_sample(s);
    REQUIRE(bam::MediaClock::sample_from_ptp(ns) == s);
    REQUIRE(bam::MediaClock::sample_from_ptp(ns - 1) == s - 1);
  }
}

TEST_CASE("rate mapping is invertible") {
  bam::MediaClock c;
  c.set_mapping(1'000'000, 1'700'000'000'000'000'000, 1.0001);
  CHECK(c.to_ptp(1'000'000) == 1'700'000'000'000'000'000);
  CHECK(c.to_ptp(1'000'000 + 1'000'000'000) == 1'700'000'000'000'000'000 + 1'000'100'000);
  for (int64_t x : {int64_t(0), int64_t(5'000'000'000), int64_t(3'600'000'000'000)}) {
    CHECK(std::llabs(c.to_local(c.to_ptp(x)) - x) <= 1);
  }
  uint64_t s = c.sample_at(2'000'000);
  CHECK(c.local_for_sample(s) <= 2'000'000);
  CHECK(c.sample_at(c.local_for_sample(s + 10)) == s + 10);
}

TEST_CASE("clock source generations") {
  bam::ClockSource src;
  CHECK(src.step_generation() == 0);
  bam::MediaClock c;
  c.set_mapping(0, 100, 1.0);
  src.set(c, false);
  CHECK(src.step_generation() == 0);
  CHECK(src.get().to_ptp(0) == 100);
  src.set(c, true);
  CHECK(src.step_generation() == 1);
}
