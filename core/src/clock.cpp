// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/clock.hpp"

#include <cmath>

#include "bam/constants.hpp"

namespace bam {

namespace {
constexpr int64_t kNsPerSecond = 1'000'000'000;
}

void MediaClock::set_mapping(int64_t local_ns, int64_t ptp_ns, double rate) {
  base_local_ = local_ns;
  base_ptp_ = ptp_ns;
  rate_ = rate;
}

int64_t MediaClock::to_ptp(int64_t local_ns) const {
  const int64_t d = local_ns - base_local_;
  return base_ptp_ + d + std::llround(static_cast<double>(d) * (rate_ - 1.0));
}

int64_t MediaClock::to_local(int64_t ptp_ns) const {
  const int64_t d = ptp_ns - base_ptp_;
  return base_local_ + d + std::llround(static_cast<double>(d) * (1.0 / rate_ - 1.0));
}

uint64_t MediaClock::sample_from_ptp(int64_t ptp_ns) {
  if (ptp_ns < 0) return 0;
  const uint64_t sec = static_cast<uint64_t>(ptp_ns / kNsPerSecond);
  const uint64_t rem = static_cast<uint64_t>(ptp_ns % kNsPerSecond);
  return sec * kSampleRate + rem * kSampleRate / kNsPerSecond;
}

int64_t MediaClock::ptp_from_sample(uint64_t sample) {
  const uint64_t sec = sample / kSampleRate;
  const uint64_t r = sample % kSampleRate;
  const uint64_t ns = (r * kNsPerSecond + kSampleRate - 1) / kSampleRate;  // ceil
  return static_cast<int64_t>(sec) * kNsPerSecond + static_cast<int64_t>(ns);
}

MediaClock ClockSource::get() const {
  std::lock_guard lock(mutex_);
  return clock_;
}

void ClockSource::set(const MediaClock& c, bool stepped) {
  {
    std::lock_guard lock(mutex_);
    clock_ = c;
  }
  if (stepped) generation_.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace bam
