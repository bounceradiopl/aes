// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>

#include "bam/encoder/dsp.hpp"

namespace bam::encoder {

DelayLine::DelayLine(double max_seconds)
    : capacity_(static_cast<uint32_t>(std::llround(std::max(0.0, max_seconds) * kStationRate))),
      cursor_(0),
      current_(0),
      target_(0) {
  ring_.assign(static_cast<size_t>(capacity_) * 2, 0.0f);
}

bool DelayLine::set_delay_seconds(double seconds) {
  if (!(seconds >= 0)) return false;
  const double frames = std::round(seconds * kStationRate);
  if (frames > max_delay_frames()) return false;
  target_ = static_cast<uint32_t>(frames);
  if (phase_ == Phase::Steady && target_ == current_) return true;
  if (!started_) {  // nothing heard yet: no fade needed
    current_ = target_;
    cursor_ = written_ - current_;
    return true;
  }
  if (phase_ == Phase::Steady || phase_ == Phase::FadeIn) phase_ = Phase::FadeOut;  // fade from the level reached
  return true;
}

void DelayLine::write(const float* stereo, size_t frames) {
  if (capacity_ == 0) {
    written_ += static_cast<int64_t>(frames);
    return;
  }
  size_t pos = static_cast<size_t>(written_ % capacity_);
  for (size_t i = 0; i < frames; ++i) {
    ring_[pos * 2] = stereo[i * 2];
    ring_[pos * 2 + 1] = stereo[i * 2 + 1];
    if (++pos == capacity_) pos = 0;
  }
  written_ += static_cast<int64_t>(frames);
}

void DelayLine::read(float* out, size_t frames) {
  started_ = true;
  for (size_t i = 0; i < frames; ++i) {
    if (phase_ == Phase::Silence) {
      // A longer delay: hold the cursor while the input runs on, until it is target_ behind.
      if (current_ < target_) {
        ++current_;
        out[i * 2] = out[i * 2 + 1] = 0.0f;
        continue;
      }
      // reached, or the target moved below the delay reached so far: skip ahead to it
      cursor_ += current_ - target_;
      current_ = target_;
      phase_ = Phase::FadeIn;
    }

    // The frame at the cursor; outside the ring (not written yet, or overwritten) it is silence.
    float l = 0.0f, r = 0.0f;
    if (cursor_ >= 0 && cursor_ < written_ && written_ - cursor_ <= capacity_) {
      const size_t at = static_cast<size_t>(cursor_ % capacity_) * 2;
      l = ring_[at];
      r = ring_[at + 1];
    }
    ++cursor_;

    if (phase_ == Phase::FadeOut) {
      if (level_ > 0) --level_;
    } else if (phase_ == Phase::FadeIn) {
      if (++level_ == kRampFrames) phase_ = Phase::Steady;
    }
    const float gain = static_cast<float>(level_) / static_cast<float>(kRampFrames);
    out[i * 2] = l * gain;
    out[i * 2 + 1] = r * gain;

    if (phase_ == Phase::FadeOut && level_ == 0) {
      // silent now: move the read position to the new delay
      if (target_ > current_) {
        phase_ = Phase::Silence;
      } else {
        cursor_ += current_ - target_;
        current_ = target_;
        phase_ = Phase::FadeIn;
      }
    }
  }
}

}  // namespace bam::encoder
