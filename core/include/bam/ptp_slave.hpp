// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "bam/clock.hpp"
#include "bam/ptp_messages.hpp"

namespace bam {

enum class PtpState { Listening, Locked, Holdover };

struct PtpStatus {
  PtpState state = PtpState::Listening;
  bool has_master = false;
  ClockIdentity grandmaster;
  PortIdentity master;
  int64_t offset_ns = 0;
  int64_t path_delay_ns = 0;
  uint64_t sync_lost = 0;  // "Synchronization Lost" counter from the Axia statistics window
  uint64_t steps = 0;
  uint8_t domain = 0;
  double rate_ppm = 0;
};

// PTPv2 ordinary clock in slave-only mode, end-to-end delay mechanism, software timestamps.
// Not thread-safe: one thread feeds messages and calls tick(); the result is clock().
class PtpSlave {
 public:
  PtpSlave(uint8_t domain, ClockIdentity self, MediaClock initial);

  void on_message(std::span<const uint8_t> bytes, int64_t rx_local_ns);
  std::optional<std::vector<uint8_t>> poll_delay_req(int64_t now_local_ns);
  void on_delay_req_sent(int64_t tx_local_ns);
  void tick(int64_t now_local_ns);

  const MediaClock& clock() const { return clock_; }
  bool consume_step();
  PtpStatus status() const;

 private:
  struct Foreign {
    AnnounceBody announce;
    int64_t last_seen = 0;
    int64_t timeout = 0;
  };

  void on_announce(const PtpMessage& m, int64_t now);
  void select_master(int64_t now);
  void reset_servo_windows();
  void measure(int64_t t1, int64_t t2_local);
  void step_to(int64_t t2_local, int64_t master_time);
  void enter_holdover(int64_t now);

  uint8_t domain_;
  PortIdentity self_;
  MediaClock clock_;
  PtpState state_ = PtpState::Listening;

  std::map<std::pair<uint64_t, uint16_t>, std::pair<PortIdentity, Foreign>> foreign_;  // key: port identity
  std::optional<PortIdentity> master_;
  ClockIdentity grandmaster_;

  // sync pairing
  std::optional<uint16_t> pending_sync_seq_;
  int64_t pending_sync_t2_ = 0;
  int64_t pending_sync_corr_ = 0;
  int64_t last_sync_local_ = 0;

  // delay request/response
  uint16_t delay_seq_ = 0;
  std::optional<uint16_t> pending_delay_seq_;
  int64_t pending_delay_t3_ = 0;
  uint64_t pending_delay_steps_ = 0;
  int64_t last_delay_req_ = 0;
  bool delay_req_due_ = false;
  std::deque<int64_t> delay_window_;
  std::optional<int64_t> mean_path_delay_;
  int64_t last_ms_delay_ = 0;
  bool have_ms_delay_ = false;

  // servo
  bool stepped_once_ = false;
  bool step_pending_ = false;
  double freq_ = 1.0;
  bool coasting_ = false;          // running on freq_ only because syncs stopped arriving
  int64_t sync_interval_ = 125'000'000;
  double holdover_freq_ = 1.0;  // long-term average of freq_, used while no syncs arrive
  int64_t prev_t2_local_ = 0;
  std::deque<int64_t> offset_window_;
  int64_t offset_ = 0;
  int good_count_ = 0;
  int big_count_ = 0;
  uint64_t sync_lost_ = 0;
  uint64_t steps_ = 0;
};

}  // namespace bam
