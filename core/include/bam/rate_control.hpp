// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace bam {

// Keeps a FIFO between a device clock and the PTP media clock at a target fill by trimming the
// resampling ratio: a PI controller on the (low-pass filtered) fill error. The returned
// multiplier scales the output rate: more data than wanted -> slightly fewer output samples.
class RateController {
 public:
  explicit RateController(double target_fill_frames, double max_ppm = 1000.0);
  double update(double fill_frames, double dt_seconds);  // returns the ratio multiplier, 1 ± max_ppm
  void reset();
  double target() const { return target_; }
  double correction_ppm() const { return correction_ * 1e6; }

 private:
  double target_;
  double max_;
  double filtered_ = 0;
  double integral_ = 0;
  double correction_ = 0;
  bool started_ = false;
};

// Variable-ratio resampler (libsamplerate, SRC_SINC_MEDIUM_QUALITY), interleaved float frames of
// `channels` samples.
class DriftResampler {
 public:
  explicit DriftResampler(double base_ratio, int channels = 2);  // base_ratio: output rate / input rate
  ~DriftResampler();
  DriftResampler(const DriftResampler&) = delete;
  DriftResampler& operator=(const DriftResampler&) = delete;

  // Converts up to in_frames, writes at most out_capacity frames; returns frames written and
  // reports how many input frames were consumed.
  size_t process(const float* in, size_t in_frames, float* out, size_t out_capacity, double ratio_multiplier, size_t* consumed);
  void reset();
  double base_ratio() const { return base_; }
  int channels() const { return channels_; }

 private:
  void* state_ = nullptr;
  double base_;
  int channels_;
};

// Left-aligned 24-in-32-bit samples <-> float in [-1, 1).
float to_float(int32_t s);
int32_t from_float(float f);  // saturating

}  // namespace bam
