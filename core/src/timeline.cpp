// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/timeline.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "bam/constants.hpp"

namespace bam {

uint32_t lap_tag(uint64_t sample) {
  const uint32_t lap = static_cast<uint32_t>(sample >> 16);
  return lap == 0xFFFFFFFFu ? 1u : lap + 1u;
}

void ChannelRing::write(uint64_t start, const int32_t* src, size_t count) {
  if (!samples_) return;
  size_t done = 0;
  while (done < count) {
    const uint64_t n = start + done;
    const uint32_t idx = static_cast<uint32_t>(n & kRingMask);
    const uint32_t block = idx / kBlockFrames;
    const uint32_t in_block = idx % kBlockFrames;
    const size_t chunk = std::min<size_t>(count - done, kBlockFrames - in_block);
    const uint32_t tag = lap_tag(n);
    std::atomic_ref<uint32_t> block_tag(tags_[block]);
    if (block_tag.load(std::memory_order_acquire) != tag) {
      std::memset(samples_ + block * kBlockFrames, 0, kBlockFrames * sizeof(int32_t));
      block_tag.store(tag, std::memory_order_release);
    }
    if (src) {
      std::memcpy(samples_ + idx, src + done, chunk * sizeof(int32_t));
    } else {
      std::memset(samples_ + idx, 0, chunk * sizeof(int32_t));
    }
    done += chunk;
  }
}

void ChannelRing::read(uint64_t start, int32_t* dst, size_t count) const {
  if (!samples_) {
    std::memset(dst, 0, count * sizeof(int32_t));
    return;
  }
  size_t done = 0;
  while (done < count) {
    const uint64_t n = start + done;
    const uint32_t idx = static_cast<uint32_t>(n & kRingMask);
    const uint32_t block = idx / kBlockFrames;
    const uint32_t in_block = idx % kBlockFrames;
    const size_t chunk = std::min<size_t>(count - done, kBlockFrames - in_block);
    std::atomic_ref<uint32_t> block_tag(tags_[block]);
    if (block_tag.load(std::memory_order_acquire) == lap_tag(n)) {
      std::memcpy(dst + done, samples_ + idx, chunk * sizeof(int32_t));
    } else {
      std::memset(dst + done, 0, chunk * sizeof(int32_t));
    }
    done += chunk;
  }
}

}  // namespace bam
