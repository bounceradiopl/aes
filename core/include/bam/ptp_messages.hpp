// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bam {

// IEEE 1588-2008 (PTPv2) wire format: the subset needed by an E2E slave and a test master.

struct ClockIdentity {
  std::array<uint8_t, 8> bytes{};
  std::string to_string() const;  // "30-D6-59-FF-FE-01-C1-19"
  bool is_zero() const;
  bool operator==(const ClockIdentity&) const = default;
};

struct PortIdentity {
  ClockIdentity clock;
  uint16_t port = 0;
  bool operator==(const PortIdentity&) const = default;
};

enum class PtpMsgType : uint8_t { Sync = 0x0, DelayReq = 0x1, FollowUp = 0x8, DelayResp = 0x9, Announce = 0xB, Other = 0xFF };

struct PtpTimestamp {
  uint64_t seconds = 0;  // 48 bits on the wire
  uint32_t nanoseconds = 0;
  int64_t to_ns() const { return static_cast<int64_t>(seconds) * 1'000'000'000 + nanoseconds; }
  static PtpTimestamp from_ns(int64_t ns);
};

struct PtpHeader {
  PtpMsgType type = PtpMsgType::Other;
  uint8_t version = 2;
  uint16_t length = 0;
  uint8_t domain = 0;
  uint16_t flags = 0;
  int64_t correction_scaled = 0;  // ns * 2^16
  PortIdentity source;
  uint16_t sequence = 0;
  int8_t log_interval = 0;
  bool two_step() const { return (flags & 0x0200) != 0; }
  int64_t correction_ns() const { return correction_scaled >> 16; }
};

struct AnnounceBody {
  int16_t utc_offset = 37;
  uint8_t priority1 = 128;
  uint8_t clock_class = 248;
  uint8_t clock_accuracy = 0xFE;
  uint16_t variance = 0xFFFF;
  uint8_t priority2 = 128;
  ClockIdentity gm_identity;
  uint16_t steps_removed = 0;
  uint8_t time_source = 0xA0;
};

struct PtpMessage {
  PtpHeader header;
  PtpTimestamp timestamp;   // origin / precise origin / receive timestamp
  PortIdentity requesting;  // Delay_Resp only
  AnnounceBody announce;    // Announce only
};

std::optional<PtpMessage> parse_ptp(std::span<const uint8_t> data);

std::vector<uint8_t> build_delay_req(uint8_t domain, const PortIdentity& self, uint16_t seq);
std::vector<uint8_t> build_sync(uint8_t domain, const PortIdentity& src, uint16_t seq, bool two_step, PtpTimestamp origin);
std::vector<uint8_t> build_follow_up(uint8_t domain, const PortIdentity& src, uint16_t seq, PtpTimestamp precise);
std::vector<uint8_t> build_delay_resp(uint8_t domain, const PortIdentity& src, uint16_t seq, PtpTimestamp rx,
                                      const PortIdentity& requesting);
std::vector<uint8_t> build_announce(uint8_t domain, const PortIdentity& src, uint16_t seq, const AnnounceBody& body);

// Dataset comparison (IEEE 1588 9.3.4, without topology): < 0 when a is the better master.
int compare_announce(const AnnounceBody& a, const AnnounceBody& b);

}  // namespace bam
