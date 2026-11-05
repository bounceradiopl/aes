// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/rate_control.hpp"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace bam {

namespace {
// Loop tuned for a ~0.02 Hz natural frequency, slightly under-damped: drift of a few hundred ppm
// is absorbed within a minute without audible pitch movement from scheduling jitter.
constexpr double kFillTau = 0.5;   // s, low-pass on the measured fill
constexpr double kP = 4.7e-6;      // per frame of fill error
constexpr double kI = 3.3e-7;      // per frame-second
}  // namespace

RateController::RateController(double target_fill_frames, double max_ppm) : target_(target_fill_frames), max_(max_ppm * 1e-6) {}

void RateController::reset() {
  started_ = false;
  integral_ = 0;
  correction_ = 0;
}

double RateController::update(double fill_frames, double dt_seconds) {
  if (!started_) {
    filtered_ = fill_frames;
    started_ = true;
  } else {
    filtered_ += (fill_frames - filtered_) * std::min(1.0, dt_seconds / kFillTau);
  }
  const double error = filtered_ - target_;
  integral_ += error * dt_seconds;
  integral_ = std::clamp(integral_, -max_ / kI, max_ / kI);  // anti-windup
  correction_ = std::clamp(-(kP * error + kI * integral_), -max_, max_);
  return 1.0 + correction_;
}

DriftResampler::DriftResampler(double base_ratio, int channels) : base_(base_ratio), channels_(channels) {
  int err = 0;
  state_ = src_new(SRC_SINC_MEDIUM_QUALITY, channels, &err);
  if (!state_) throw std::runtime_error(std::string("libsamplerate: ") + src_strerror(err));
}

DriftResampler::~DriftResampler() {
  if (state_) src_delete(static_cast<SRC_STATE*>(state_));
}

void DriftResampler::reset() { src_reset(static_cast<SRC_STATE*>(state_)); }

size_t DriftResampler::process(const float* in, size_t in_frames, float* out, size_t out_capacity, double ratio_multiplier,
                               size_t* consumed) {
  SRC_DATA d{};
  d.data_in = in;
  d.data_out = out;
  d.input_frames = static_cast<long>(in_frames);
  d.output_frames = static_cast<long>(out_capacity);
  d.src_ratio = base_ * ratio_multiplier;
  d.end_of_input = 0;
  if (src_process(static_cast<SRC_STATE*>(state_), &d) != 0) {
    ++errors_;
    if (consumed) *consumed = in_frames;  // drop the block rather than stall
    return 0;
  }
  if (consumed) *consumed = static_cast<size_t>(d.input_frames_used);
  return static_cast<size_t>(d.output_frames_gen);
}

float to_float(int32_t s) { return static_cast<float>(static_cast<double>(s) / 2147483648.0); }

int32_t from_float(float f) {
  if (std::isnan(f)) return 0;
  const double v = std::nearbyint(static_cast<double>(f) * 2147483648.0);
  if (v >= 2147483647.0) return 2147483647;
  if (v <= -2147483648.0) return -2147483647 - 1;
  return static_cast<int32_t>(v);
}

}  // namespace bam
