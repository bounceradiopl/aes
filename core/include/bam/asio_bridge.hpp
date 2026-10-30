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
//   outputs go to                 [S + B, S + 2B)
class AsioBridge {
 public:
  explicit AsioBridge(ShmView view) : view_(view) {}

  void read_inputs(uint64_t period_start, uint32_t frames, int32_t* const* in_bufs) const;  // kChannelCount entries
  void write_outputs(uint64_t period_start, uint32_t frames, const int32_t* const* out_bufs);

 private:
  ShmView view_;
};

// ASIO channel name (max 31 bytes): "BounceAES 02 R" or "BounceAES 02 R (Kraków)".
std::string channel_label(int channel, std::string_view device_name);

}  // namespace bam
