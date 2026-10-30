// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include <span>
#include <vector>

#include "bam/clock.hpp"
#include "bam/config.hpp"
#include "bam/ptp_messages.hpp"

namespace bam {

enum class PtpState { Listening, Locked, Holdover, Master };

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

// Own clock dataset and role. Default role Slave: the port never transmits as a master.
struct PtpLocal {
  PtpRole role = PtpRole::Slave;
  AnnounceBody announce;  // gm_identity is overwritten with the port's own identity
};

// A message the port wants transmitted while it is the master.
struct PtpOut {
  std::vector<uint8_t> bytes;
  bool event_port = false;     // Sync goes to 319, everything else to 320
  bool needs_tx_time = false;  // caller reports the send time via on_sync_sent()
  uint16_t sync_seq = 0;
};

// PTPv2 ordinary clock, end-to-end delay mechanism, software timestamps. Runs the best master
// clock algorithm over its own dataset and every foreign master: follows the best foreign master
// as a slave, or becomes the master itself when its own dataset has been the best for 3 s.
// Not thread-safe: one thread feeds messages, polls and calls tick(); the result is clock().
class PtpPort {
 public:
  PtpPort(uint8_t domain, ClockIdentity self, MediaClock initial, PtpLocal local = {});

  void set_local(const PtpLocal& local);
  void on_message(std::span<const uint8_t> bytes, int64_t rx_local_ns);
  std::optional<std::vector<uint8_t>> poll_delay_req(int64_t now_local_ns);  // slave only
  void on_delay_req_sent(int64_t tx_local_ns);
  std::vector<PtpOut> poll_master(int64_t now_local_ns);  // Announce / Sync / Follow_Up / Delay_Resp due now
  void on_sync_sent(uint16_t seq, int64_t tx_local_ns);   // queues the Follow_Up with the precise origin
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
  void become_master(int64_t now);
  void leave_master();
  AnnounceBody own_announce() const;

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

  // master
  PtpLocal local_;
  int64_t self_best_since_ = 0;  // 0 = own dataset is not the best right now
  int64_t next_announce_ = 0, next_sync_ = 0;
  uint16_t announce_seq_ = 0, sync_seq_ = 0;
  std::deque<std::pair<uint16_t, int64_t>> followups_;           // sync seq, tx local time
  std::deque<std::tuple<uint16_t, int64_t, PortIdentity>> delay_resps_;  // seq, rx local time, requester
};

using PtpSlave = PtpPort;  // 1.0 name

}  // namespace bam
