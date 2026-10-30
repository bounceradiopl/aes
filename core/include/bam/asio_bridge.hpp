// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "bam/shm_layout.hpp"

namespace bam {

// Buffer exchange between an ASIO host and the shared timeline, for the period starting at
// sample S with buffer size B:
//   inputs  of device d come from [S - B - Lrx_d, S - Lrx_d)
//   outputs go to                 [S + kOutputLeadBuffers*B, S + (kOutputLeadBuffers+1)*B)
// Two buffers of output lead give the host a full extra period of slack: a callback that
// finishes late (or one that was skipped and is caught up) still lands before the packet
// carrying those samples is due, so the stream never carries a hole for a merely late host.
inline constexpr uint32_t kOutputLeadBuffers = 2;

class AsioBridge {
 public:
  explicit AsioBridge(ShmView view) : view_(view) {}

  // First timeline sample the host's output buffer for period S is written to.
  static uint64_t output_start(uint64_t period_start, uint32_t frames) {
    return period_start + uint64_t(kOutputLeadBuffers) * frames;
  }

  void read_inputs(uint64_t period_start, uint32_t frames, int32_t* const* in_bufs) const;  // kChannelCount entries
  void write_outputs(uint64_t period_start, uint32_t frames, const int32_t* const* out_bufs);

 private:
  ShmView view_;
};

// ASIO channel name (max 31 bytes): "BounceAES 02 R" or "BounceAES 02 R (Kraków)".
std::string channel_label(int channel, std::string_view device_name);

}  // namespace bam
