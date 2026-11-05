// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "bam/encoder/dsp.hpp"
#include "bam/rate_control.hpp"

namespace bam::encoder {

namespace {
uint32_t checked_rate(uint32_t rate) {
  if (std::find(kStreamRates.begin(), kStreamRates.end(), rate) == kStreamRates.end())
    throw std::invalid_argument("stream sample rate not supported: " + std::to_string(rate));
  return rate;
}
}  // namespace

StreamConditioner::StreamConditioner(uint32_t out_rate, double gain)
    : out_rate_(checked_rate(out_rate)), ratio_(static_cast<double>(out_rate) / kStationRate), scratch_(kChunkFrames * 2), gain_(gain) {
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
  return static_cast<size_t>(std::ceil(static_cast<double>(in_frames) * ratio_)) + kOutSlackFrames;
}

size_t StreamConditioner::process(const float* in, size_t in_frames, float* out, size_t out_capacity) {
  // An `out` smaller than max_out_frames() is a caller bug: take only the input that fits and
  // count the rest, rather than let the resampler bank it and run ahead of the input.
  size_t take = in_frames;
  if (max_out_frames(take) > out_capacity) {
    take = !resampler_ ? out_capacity
                       : out_capacity <= kOutSlackFrames ? 0
                                                         : static_cast<size_t>(static_cast<double>(out_capacity - kOutSlackFrames) / ratio_);
    while (take > 0 && max_out_frames(take) > out_capacity) --take;
    dropped_ += in_frames - take;
  }

  size_t written = 0;
  for (size_t done = 0; done < take;) {
    const size_t n = std::min(kChunkFrames, take - done);
    const float* src = in + done * 2;
    float* gained = scratch_.data();
    for (size_t f = 0; f < n; ++f) {
      const float g = static_cast<float>(gain_.next());
      gained[f * 2] = src[f * 2] * g;
      gained[f * 2 + 1] = src[f * 2 + 1] * g;
    }
    done += n;

    if (!resampler_) {
      std::memcpy(out + written * 2, gained, n * 2 * sizeof(float));
      written += n;
      continue;
    }
    for (size_t consumed = 0; consumed < n;) {
      size_t used = 0;
      const size_t got = resampler_->process(gained + consumed * 2, n - consumed, out + written * 2, out_capacity - written, 1.0, &used);
      written += got;
      consumed += used;
      if (got == 0 && used == 0) {  // `out` full after all (not expected within max_out_frames)
        dropped_ += n - consumed;
        break;
      }
    }
  }
  return written;
}

}  // namespace bam::encoder
