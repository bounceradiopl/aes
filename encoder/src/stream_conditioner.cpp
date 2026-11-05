// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>

#include "bam/encoder/dsp.hpp"
#include "bam/rate_control.hpp"

namespace bam::encoder {

StreamConditioner::StreamConditioner(uint32_t out_rate, double gain)
    : out_rate_(out_rate), ratio_(static_cast<double>(out_rate) / kStationRate), scratch_(kChunkFrames * 2), gain_(gain) {
  if (out_rate_ == kStationRate) return;
  resampler_ = std::make_unique<DriftResampler>(ratio_, 2);
  // Prime the look-ahead with silence until the first output frame appears (and drop it): from
  // then on every input frame yields `ratio_` output frames, so the output keeps pace with the
  // input from the first real block instead of lagging by the filter length.
  const float zero[2] = {0.0f, 0.0f};
  float sink[2 * 4];
  for (int i = 0; i < 1 << 14; ++i) {
    size_t used = 0;
    if (resampler_->process(zero, 1, sink, 4, 1.0, &used) > 0) break;
  }
}

StreamConditioner::~StreamConditioner() = default;

void StreamConditioner::set_gain(double linear) { gain_.set_target(linear); }

size_t StreamConditioner::max_out_frames(size_t in_frames) const {
  if (!resampler_) return in_frames;
  return static_cast<size_t>(std::ceil(static_cast<double>(in_frames) * ratio_)) + 4;
}

size_t StreamConditioner::process(const float* in, size_t in_frames, float* out, size_t out_capacity) {
  size_t written = 0;
  for (size_t done = 0; done < in_frames;) {
    const size_t n = std::min(kChunkFrames, in_frames - done);
    const float* src = in + done * 2;
    float* gained = scratch_.data();
    for (size_t f = 0; f < n; ++f) {
      const float g = static_cast<float>(gain_.next());
      gained[f * 2] = src[f * 2] * g;
      gained[f * 2 + 1] = src[f * 2 + 1] * g;
    }
    done += n;

    if (!resampler_) {
      const size_t copy = std::min(n, out_capacity - written);
      std::memcpy(out + written * 2, gained, copy * 2 * sizeof(float));
      written += copy;
      continue;
    }
    for (size_t consumed = 0; consumed < n;) {
      size_t used = 0;
      const size_t got = resampler_->process(gained + consumed * 2, n - consumed, out + written * 2, out_capacity - written, 1.0, &used);
      written += got;
      consumed += used;
      if (got == 0 && used == 0) break;  // `out` is full
    }
  }
  return written;
}

}  // namespace bam::encoder
