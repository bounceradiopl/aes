// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace bam {

// Linear mapping between the local monotonic clock and PTP time (both in ns):
//   ptp = base_ptp + (local - base_local) * rate
// The media clock is the 48 kHz sample count since the PTP epoch (AES67 mediaclk:direct=0).
class MediaClock {
 public:
  void set_mapping(int64_t local_ns, int64_t ptp_ns, double rate);
  int64_t to_ptp(int64_t local_ns) const;
  int64_t to_local(int64_t ptp_ns) const;
  double rate() const { return rate_; }
  int64_t base_local() const { return base_local_; }
  int64_t base_ptp() const { return base_ptp_; }

  static uint64_t sample_from_ptp(int64_t ptp_ns);  // floor(ptp_ns * 48000 / 1e9)
  static int64_t ptp_from_sample(uint64_t sample);  // smallest ns that maps to sample

  uint64_t sample_at(int64_t local_ns) const { return sample_from_ptp(to_ptp(local_ns)); }
  int64_t local_for_sample(uint64_t sample) const { return to_local(ptp_from_sample(sample)); }

 private:
  int64_t base_local_ = 0;
  int64_t base_ptp_ = 0;
  double rate_ = 1.0;
};

// Thread-safe holder of the current mapping. step_generation() changes whenever the clock jumps,
// so consumers (TX streams, period scheduler) can re-align.
class ClockSource {
 public:
  MediaClock get() const;
  // The mapping together with the step generation it belongs to (one consistent pair).
  MediaClock get(uint64_t* step_generation) const;
  void set(const MediaClock& c, bool stepped);
  uint64_t step_generation() const { return generation_.load(std::memory_order_acquire); }

 private:
  mutable std::mutex mutex_;
  MediaClock clock_;
  std::atomic<uint64_t> generation_{0};
};

}  // namespace bam
