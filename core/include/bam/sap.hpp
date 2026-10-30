// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bam/sdp.hpp"

namespace bam {

// Session Announcement Protocol (RFC 2974), IPv4, no authentication/encryption/compression.
struct SapPacket {
  bool deletion = false;
  uint16_t hash = 0;
  std::array<uint8_t, 4> origin{};
  std::string payload_type;  // empty when the packet omits it
  std::string sdp;
};

std::optional<SapPacket> parse_sap(std::span<const uint8_t> data);
std::vector<uint8_t> build_sap(bool deletion, uint16_t hash, std::array<uint8_t, 4> origin, std::string_view sdp);
uint16_t sap_hash(std::string_view text);

struct DiscoveredStream {
  std::string id;      // "<origin>/<hash>"
  std::string name;    // SDP s=
  std::string origin;  // announcing host
  std::string sdp;
  SdpStream parsed;
  int64_t last_seen_ns = 0;
};

// Catalogue of AES67 streams announced on the network ("Przeglądaj…" in the panel).
class SapDirectory {
 public:
  void on_packet(std::span<const uint8_t> data, int64_t now_ns);
  void expire(int64_t now_ns);
  std::vector<DiscoveredStream> list() const;
  std::optional<DiscoveredStream> find(std::string_view id) const;

 private:
  std::map<std::string, DiscoveredStream, std::less<>> streams_;
};

}  // namespace bam
