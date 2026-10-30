// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "bam/format.hpp"

namespace bam {

// One AES67 audio stream as described by an SDP document (first m=audio section).
struct SdpStream {
  std::string session_name;
  std::string origin_address;     // unicast address from o=
  std::string multicast_address;  // from c= (media level wins over session level)
  int ttl = 0;
  uint16_t port = 0;
  int payload_type = 0;
  Encoding encoding = Encoding::L24;
  uint32_t sample_rate = 0;
  int channels = 0;
  double ptime_ms = 1.0;
  uint32_t frames_per_packet = 48;  // round(ptime_ms * sample_rate / 1000)
  std::optional<uint32_t> framecount;
  std::string source_filter;  // source address from a=source-filter: incl, may be empty
  std::string ptp_gmid;       // grandmaster identity from a=ts-refclk
  int ptp_domain = 0;
  uint32_t mediaclk_offset = 0;
};

struct SdpParseResult {
  std::optional<SdpStream> stream;
  std::string error;  // Polish, user facing
};

SdpParseResult parse_sdp(std::string_view text);
std::string generate_sdp(const SdpStream& s, uint64_t session_id, uint64_t version);

}  // namespace bam
