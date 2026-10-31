// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "bam/rate_control.hpp"

namespace bam {

// Feeds a sound card running on its own clock (the bridge's RenderStream) from a source on the
// media clock. Called once per device event: reads the source up to `ready`, resamples it to the
// device rate with a PI-trimmed ratio and returns what fits into the device buffer now.
//
// It keeps the device buffer plus its own queue at two device periods (at least 20 ms): the
// Windows engine takes a whole period per pass, so a single period of headroom runs dry whenever
// the refill is a little late, which is heard as regular clicks.
class RenderPacer {
 public:
  using Reader = std::function<void(uint64_t pos, int32_t* left, int32_t* right, uint32_t frames)>;

  RenderPacer(uint32_t device_rate, uint32_t device_period_frames);

  // ready: media position up to which the source is complete; generation: media clock step
  // generation (a change re-anchors); padding: frames still queued in the device; space: frames
  // that fit now. Writes interleaved stereo float frames to `out` and returns their count.
  size_t step(uint64_t ready, uint64_t generation, uint32_t padding, uint32_t space, double now_s, const Reader& read,
              std::vector<float>& out);

  double target_frames() const { return target_; }
  double correction_ppm() const { return rc_.correction_ppm(); }
  uint64_t anchors() const { return anchors_; }

 private:
  uint32_t rate_;
  double target_;
  DriftResampler rs_;
  RateController rc_;
  std::vector<float> queue_, staging_;
  std::vector<int32_t> l_, r_;
  double mult_ = 1.0;
  double last_s_ = 0;
  bool anchored_ = false;
  uint64_t read_pos_ = 0, generation_ = 0, anchors_ = 0;
};

}  // namespace bam
