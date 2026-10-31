// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/render_pacer.hpp"

#include <algorithm>
#include <cmath>

#include "bam/constants.hpp"

namespace bam {

namespace {
constexpr uint32_t kChunk = 48;               // source frames read at a time (1 ms)
constexpr uint64_t kReanchorFrames = 4800;    // 100 ms off the media clock: start over
}  // namespace

RenderPacer::RenderPacer(uint32_t device_rate, uint32_t device_period_frames)
    : rate_(device_rate),
      target_(std::max(2.0 * device_period_frames, device_rate * 0.020)),
      rs_(static_cast<double>(device_rate) / kSampleRate),
      rc_(target_),
      l_(kChunk),
      r_(kChunk) {}

size_t RenderPacer::step(uint64_t ready, uint64_t generation, uint32_t padding, uint32_t space, double now_s, const Reader& read,
                         std::vector<float>& out) {
  out.clear();
  if (!anchored_ || generation != generation_ || read_pos_ + kReanchorFrames < ready || read_pos_ > ready + kReanchorFrames) {
    // start with the whole target already queued, so the first passes do not run dry
    const auto lead = static_cast<uint64_t>(std::ceil(target_ * kSampleRate / rate_));
    read_pos_ = ready > lead ? ready - lead : 0;
    generation_ = generation;
    anchored_ = true;
    queue_.clear();
    rs_.reset();
    rc_.reset();
    mult_ = 1.0;
    last_s_ = now_s;
    ++anchors_;
  }
  while (read_pos_ + kChunk <= ready) {
    read(read_pos_, l_.data(), r_.data(), kChunk);
    for (uint32_t i = 0; i < kChunk; ++i) {
      staging_.push_back(to_float(l_[i]));
      staging_.push_back(to_float(r_[i]));
    }
    read_pos_ += kChunk;
  }
  if (!staging_.empty()) {
    const size_t frames = staging_.size() / 2;
    std::vector<float> tmp(frames * 2 + 512);
    size_t offset = 0;
    while (offset < frames) {
      size_t used = 0;
      const size_t n = rs_.process(staging_.data() + offset * 2, frames - offset, tmp.data(), tmp.size() / 2, mult_, &used);
      queue_.insert(queue_.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n * 2));
      offset += used;
      if (!used && !n) break;
    }
    staging_.erase(staging_.begin(), staging_.begin() + static_cast<std::ptrdiff_t>(std::min(offset, frames) * 2));
  }

  const size_t n = std::min<size_t>(space, queue_.size() / 2);
  out.assign(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n * 2));
  queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n * 2));

  const double dt = std::max(0.0, now_s - last_s_);
  last_s_ = now_s;
  mult_ = rc_.update(static_cast<double>(padding) + n + queue_.size() / 2.0, dt);
  return n;
}

}  // namespace bam
