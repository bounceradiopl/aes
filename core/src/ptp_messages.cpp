// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/ptp_messages.hpp"

#include <cstdio>
#include <cstring>

namespace bam {

namespace {
constexpr size_t kHeaderSize = 34;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void wr16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void wr32(uint8_t* p, uint32_t v) {
  wr16(p, static_cast<uint16_t>(v >> 16));
  wr16(p + 2, static_cast<uint16_t>(v));
}

PtpTimestamp rd_ts(const uint8_t* p) {
  PtpTimestamp t;
  t.seconds = (uint64_t(rd16(p)) << 32) | rd32(p + 2);
  t.nanoseconds = rd32(p + 6);
  return t;
}
void wr_ts(uint8_t* p, const PtpTimestamp& t) {
  wr16(p, static_cast<uint16_t>(t.seconds >> 32));
  wr32(p + 2, static_cast<uint32_t>(t.seconds));
  wr32(p + 6, t.nanoseconds);
}

PortIdentity rd_port(const uint8_t* p) {
  PortIdentity id;
  std::memcpy(id.clock.bytes.data(), p, 8);
  id.port = rd16(p + 8);
  return id;
}
void wr_port(uint8_t* p, const PortIdentity& id) {
  std::memcpy(p, id.clock.bytes.data(), 8);
  wr16(p + 8, id.port);
}

std::vector<uint8_t> make_header(PtpMsgType type, size_t length, uint8_t domain, uint16_t flags,
                                 const PortIdentity& src, uint16_t seq, uint8_t control, int8_t log_interval) {
  std::vector<uint8_t> m(length, 0);
  m[0] = static_cast<uint8_t>(type) & 0x0F;
  m[1] = 2;
  wr16(&m[2], static_cast<uint16_t>(length));
  m[4] = domain;
  wr16(&m[6], flags);
  wr_port(&m[20], src);
  wr16(&m[30], seq);
  m[32] = control;
  m[33] = static_cast<uint8_t>(log_interval);
  return m;
}
}  // namespace

std::string ClockIdentity::to_string() const {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7]);
  return buf;
}

bool ClockIdentity::is_zero() const {
  for (auto b : bytes)
    if (b) return false;
  return true;
}

PtpTimestamp PtpTimestamp::from_ns(int64_t ns) {
  PtpTimestamp t;
  t.seconds = static_cast<uint64_t>(ns / 1'000'000'000);
  t.nanoseconds = static_cast<uint32_t>(ns % 1'000'000'000);
  return t;
}

std::optional<PtpMessage> parse_ptp(std::span<const uint8_t> d) {
  if (d.size() < kHeaderSize) return std::nullopt;
  if ((d[1] & 0x0F) != 2) return std::nullopt;
  PtpMessage m;
  const uint8_t type = d[0] & 0x0F;
  m.header.version = d[1] & 0x0F;
  m.header.length = rd16(&d[2]);
  m.header.domain = d[4];
  m.header.flags = rd16(&d[6]);
  m.header.correction_scaled = static_cast<int64_t>((uint64_t(rd32(&d[8])) << 32) | rd32(&d[12]));
  m.header.source = rd_port(&d[20]);
  m.header.sequence = rd16(&d[30]);
  m.header.log_interval = static_cast<int8_t>(d[33]);
  if (m.header.length > d.size()) return std::nullopt;

  switch (type) {
    case 0x0:
    case 0x1:
    case 0x8:
      if (d.size() < 44) return std::nullopt;
      m.header.type = static_cast<PtpMsgType>(type);
      m.timestamp = rd_ts(&d[34]);
      break;
    case 0x9:
      if (d.size() < 54) return std::nullopt;
      m.header.type = PtpMsgType::DelayResp;
      m.timestamp = rd_ts(&d[34]);
      m.requesting = rd_port(&d[44]);
      break;
    case 0xB: {
      if (d.size() < 64) return std::nullopt;
      m.header.type = PtpMsgType::Announce;
      m.timestamp = rd_ts(&d[34]);
      auto& a = m.announce;
      a.utc_offset = static_cast<int16_t>(rd16(&d[44]));
      a.priority1 = d[47];
      a.clock_class = d[48];
      a.clock_accuracy = d[49];
      a.variance = rd16(&d[50]);
      a.priority2 = d[52];
      std::memcpy(a.gm_identity.bytes.data(), &d[53], 8);
      a.steps_removed = rd16(&d[61]);
      a.time_source = d[63];
      break;
    }
    default:
      m.header.type = PtpMsgType::Other;
      break;
  }
  return m;
}

std::vector<uint8_t> build_delay_req(uint8_t domain, const PortIdentity& self, uint16_t seq) {
  return make_header(PtpMsgType::DelayReq, 44, domain, 0, self, seq, 1, 0x7F);
}

std::vector<uint8_t> build_sync(uint8_t domain, const PortIdentity& src, uint16_t seq, bool two_step, PtpTimestamp origin) {
  auto m = make_header(PtpMsgType::Sync, 44, domain, two_step ? 0x0200 : 0, src, seq, 0, -3);
  wr_ts(&m[34], origin);
  return m;
}

std::vector<uint8_t> build_follow_up(uint8_t domain, const PortIdentity& src, uint16_t seq, PtpTimestamp precise) {
  auto m = make_header(PtpMsgType::FollowUp, 44, domain, 0, src, seq, 2, -3);
  wr_ts(&m[34], precise);
  return m;
}

std::vector<uint8_t> build_delay_resp(uint8_t domain, const PortIdentity& src, uint16_t seq, PtpTimestamp rx,
                                      const PortIdentity& requesting) {
  auto m = make_header(PtpMsgType::DelayResp, 54, domain, 0, src, seq, 3, 0);
  wr_ts(&m[34], rx);
  wr_port(&m[44], requesting);
  return m;
}

std::vector<uint8_t> build_announce(uint8_t domain, const PortIdentity& src, uint16_t seq, const AnnounceBody& a) {
  auto m = make_header(PtpMsgType::Announce, 64, domain, 0, src, seq, 5, 1);
  wr16(&m[44], static_cast<uint16_t>(a.utc_offset));
  m[47] = a.priority1;
  m[48] = a.clock_class;
  m[49] = a.clock_accuracy;
  wr16(&m[50], a.variance);
  m[52] = a.priority2;
  std::memcpy(&m[53], a.gm_identity.bytes.data(), 8);
  wr16(&m[61], a.steps_removed);
  m[63] = a.time_source;
  return m;
}

int compare_announce(const AnnounceBody& a, const AnnounceBody& b) {
  if (a.priority1 != b.priority1) return a.priority1 < b.priority1 ? -1 : 1;
  if (a.clock_class != b.clock_class) return a.clock_class < b.clock_class ? -1 : 1;
  if (a.clock_accuracy != b.clock_accuracy) return a.clock_accuracy < b.clock_accuracy ? -1 : 1;
  if (a.variance != b.variance) return a.variance < b.variance ? -1 : 1;
  if (a.priority2 != b.priority2) return a.priority2 < b.priority2 ? -1 : 1;
  int c = std::memcmp(a.gm_identity.bytes.data(), b.gm_identity.bytes.data(), 8);
  if (c != 0) return c < 0 ? -1 : 1;
  if (a.steps_removed != b.steps_removed) return a.steps_removed < b.steps_removed ? -1 : 1;
  return 0;
}

}  // namespace bam
