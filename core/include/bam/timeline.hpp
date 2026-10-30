// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace bam {

// Lap number of a sample in a ring of kRingFrames, offset by one so 0 means "never written".
uint32_t lap_tag(uint64_t sample);

// One audio channel on the shared timeline. Sample n lives at index n % kRingFrames.
// Every 16-frame block carries the lap tag it was last written in, so data from a previous lap
// is never played back: a read of a block with a different tag returns silence.
// Single writer, any number of readers; the reader must stay clear of the region being written.
class ChannelRing {
 public:
  ChannelRing() = default;
  ChannelRing(int32_t* samples, uint32_t* tags) : samples_(samples), tags_(tags) {}

  bool valid() const { return samples_ != nullptr; }
  void write(uint64_t start, const int32_t* src, size_t count);  // src == nullptr writes silence
  void read(uint64_t start, int32_t* dst, size_t count) const;

 private:
  int32_t* samples_ = nullptr;
  uint32_t* tags_ = nullptr;
};

}  // namespace bam
