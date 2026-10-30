// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/sap.hpp"

#include <algorithm>

#include "bam/ipv4.hpp"

namespace bam {

namespace {
constexpr size_t kSapHeader = 8;
constexpr const char* kSdpType = "application/sdp";
constexpr int64_t kExpireNs = 300LL * 1'000'000'000;  // 5 minutes without an announcement
}  // namespace

std::optional<SapPacket> parse_sap(std::span<const uint8_t> d) {
  if (d.size() < kSapHeader) return std::nullopt;
  const uint8_t flags = d[0];
  if ((flags >> 5) != 1) return std::nullopt;  // version 1
  if (flags & 0x10) return std::nullopt;       // IPv6 origin not supported
  if (flags & 0x03) return std::nullopt;       // encrypted or compressed
  const size_t auth_len = size_t(d[1]) * 4;
  SapPacket p;
  p.deletion = (flags & 0x04) != 0;
  p.hash = static_cast<uint16_t>((d[2] << 8) | d[3]);
  std::copy(d.begin() + 4, d.begin() + 8, p.origin.begin());
  size_t offset = kSapHeader + auth_len;
  if (offset > d.size()) return std::nullopt;
  std::string_view rest(reinterpret_cast<const char*>(d.data() + offset), d.size() - offset);
  if (rest.substr(0, 2) != "v=") {
    const size_t nul = rest.find('\0');
    if (nul == std::string_view::npos) return std::nullopt;
    p.payload_type = std::string(rest.substr(0, nul));
    rest.remove_prefix(nul + 1);
  }
  p.sdp = std::string(rest);
  return p;
}

std::vector<uint8_t> build_sap(bool deletion, uint16_t hash, std::array<uint8_t, 4> origin, std::string_view sdp) {
  std::vector<uint8_t> out;
  out.reserve(kSapHeader + 16 + sdp.size());
  out.push_back(static_cast<uint8_t>(0x20 | (deletion ? 0x04 : 0)));
  out.push_back(0);
  out.push_back(static_cast<uint8_t>(hash >> 8));
  out.push_back(static_cast<uint8_t>(hash));
  out.insert(out.end(), origin.begin(), origin.end());
  const std::string_view type(kSdpType);
  out.insert(out.end(), type.begin(), type.end());
  out.push_back(0);
  out.insert(out.end(), sdp.begin(), sdp.end());
  return out;
}

uint16_t sap_hash(std::string_view text) {
  uint32_t h = 2166136261u;
  for (unsigned char c : text) {
    h ^= c;
    h *= 16777619u;
  }
  const uint16_t folded = static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFF));
  return folded == 0 ? 1 : folded;
}

void SapDirectory::on_packet(std::span<const uint8_t> data, int64_t now_ns) {
  auto p = parse_sap(data);
  if (!p) return;
  if (!p->payload_type.empty() && p->payload_type != kSdpType) return;
  const std::string origin = ipv4_to_string(p->origin);
  const std::string id = origin + "/" + std::to_string(p->hash);
  if (p->deletion) {
    streams_.erase(id);
    return;
  }
  auto parsed = parse_sdp(p->sdp);
  if (!parsed.stream) return;
  DiscoveredStream& s = streams_[id];
  s.id = id;
  s.name = parsed.stream->session_name.empty() ? id : parsed.stream->session_name;
  s.origin = origin;
  s.sdp = p->sdp;
  s.parsed = *parsed.stream;
  s.last_seen_ns = now_ns;
}

void SapDirectory::expire(int64_t now_ns) {
  for (auto it = streams_.begin(); it != streams_.end();) {
    if (now_ns - it->second.last_seen_ns > kExpireNs) {
      it = streams_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<DiscoveredStream> SapDirectory::list() const {
  std::vector<DiscoveredStream> out;
  for (const auto& [id, s] : streams_) out.push_back(s);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

std::optional<DiscoveredStream> SapDirectory::find(std::string_view id) const {
  auto it = streams_.find(id);
  if (it == streams_.end()) return std::nullopt;
  return it->second;
}

}  // namespace bam
