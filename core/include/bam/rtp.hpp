// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "bam/format.hpp"

namespace bam {

inline constexpr size_t kRtpHeaderSize = 12;

struct RtpHeader {
  uint8_t payload_type = 0;
  bool marker = false;
  uint16_t sequence = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
};

struct RtpPacketView {
  RtpHeader header;
  std::span<const uint8_t> payload;
};

// RTP version 2 only. CSRC list and header extension are skipped, padding is stripped.
std::optional<RtpPacketView> parse_rtp(std::span<const uint8_t> packet);

// Writes the fixed 12-byte header (no CSRC, no extension).
void write_rtp_header(const RtpHeader& h, uint8_t* out);

// Interleaved big-endian PCM -> planar int32, left aligned (L24: s << 8, L16: s << 16).
void decode_pcm(Encoding e, const uint8_t* payload, size_t frames, int channels, int32_t* const* planar_out);

// Planar int32 -> interleaved big-endian L24 (top 24 bits of each sample).
void encode_l24(const int32_t* const* planar_in, int channels, size_t frames, uint8_t* out);

// Returns the 64-bit sample index whose low 32 bits equal ts and which is nearest to reference.
uint64_t unwrap_rtp_timestamp(uint32_t ts, uint64_t reference_sample);

}  // namespace bam
