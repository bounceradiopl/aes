// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/rtp.hpp"

namespace bam {

std::optional<RtpPacketView> parse_rtp(std::span<const uint8_t> p) {
  if (p.size() < kRtpHeaderSize) return std::nullopt;
  if ((p[0] >> 6) != 2) return std::nullopt;
  const bool padding = (p[0] & 0x20) != 0;
  const bool extension = (p[0] & 0x10) != 0;
  const size_t csrc_count = p[0] & 0x0F;

  RtpPacketView v;
  v.header.marker = (p[1] & 0x80) != 0;
  v.header.payload_type = p[1] & 0x7F;
  v.header.sequence = static_cast<uint16_t>((p[2] << 8) | p[3]);
  v.header.timestamp = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
  v.header.ssrc = (uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) | (uint32_t(p[10]) << 8) | p[11];

  size_t offset = kRtpHeaderSize + csrc_count * 4;
  if (offset > p.size()) return std::nullopt;
  if (extension) {
    if (offset + 4 > p.size()) return std::nullopt;
    size_t words = (size_t(p[offset + 2]) << 8) | p[offset + 3];
    offset += 4 + words * 4;
    if (offset > p.size()) return std::nullopt;
  }
  size_t end = p.size();
  if (padding) {
    size_t pad = p.back();
    if (pad == 0 || offset + pad > end) return std::nullopt;
    end -= pad;
  }
  v.payload = p.subspan(offset, end - offset);
  return v;
}

void write_rtp_header(const RtpHeader& h, uint8_t* out) {
  out[0] = 0x80;
  out[1] = static_cast<uint8_t>((h.marker ? 0x80 : 0) | (h.payload_type & 0x7F));
  out[2] = static_cast<uint8_t>(h.sequence >> 8);
  out[3] = static_cast<uint8_t>(h.sequence);
  out[4] = static_cast<uint8_t>(h.timestamp >> 24);
  out[5] = static_cast<uint8_t>(h.timestamp >> 16);
  out[6] = static_cast<uint8_t>(h.timestamp >> 8);
  out[7] = static_cast<uint8_t>(h.timestamp);
  out[8] = static_cast<uint8_t>(h.ssrc >> 24);
  out[9] = static_cast<uint8_t>(h.ssrc >> 16);
  out[10] = static_cast<uint8_t>(h.ssrc >> 8);
  out[11] = static_cast<uint8_t>(h.ssrc);
}

void decode_pcm(Encoding e, const uint8_t* payload, size_t frames, int channels, int32_t* const* planar_out) {
  const uint8_t* p = payload;
  if (e == Encoding::L24) {
    for (size_t f = 0; f < frames; ++f) {
      for (int c = 0; c < channels; ++c) {
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8);
        planar_out[c][f] = static_cast<int32_t>(v);
        p += 3;
      }
    }
  } else {
    for (size_t f = 0; f < frames; ++f) {
      for (int c = 0; c < channels; ++c) {
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16);
        planar_out[c][f] = static_cast<int32_t>(v);
        p += 2;
      }
    }
  }
}

void encode_l24(const int32_t* const* planar_in, int channels, size_t frames, uint8_t* out) {
  for (size_t f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      uint32_t v = static_cast<uint32_t>(planar_in[c][f]);
      out[0] = static_cast<uint8_t>(v >> 24);
      out[1] = static_cast<uint8_t>(v >> 16);
      out[2] = static_cast<uint8_t>(v >> 8);
      out += 3;
    }
  }
}

uint64_t unwrap_rtp_timestamp(uint32_t ts, uint64_t reference) {
  // The signed 32-bit distance picks the nearest of the three candidate laps.
  const int32_t diff = static_cast<int32_t>(ts - static_cast<uint32_t>(reference));
  if (diff < 0 && static_cast<uint64_t>(-static_cast<int64_t>(diff)) > reference) return ts;  // no lap below 0
  return reference + static_cast<int64_t>(diff);
}

}  // namespace bam
