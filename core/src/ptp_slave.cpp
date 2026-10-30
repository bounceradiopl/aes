// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/ptp_slave.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bam {

namespace {
constexpr int64_t kSecond = 1'000'000'000;
constexpr int64_t kSyncTimeout = 3 * kSecond;
constexpr int64_t kMinAnnounceTimeout = 6 * kSecond;
constexpr int64_t kDelayReqInterval = kSecond;
constexpr int64_t kStepThreshold = 1'000'000;   // 1 ms
constexpr int64_t kLockThreshold = 500'000;     // 500 us
constexpr int kStepConfirmations = 3;
constexpr int kLockConfirmations = 4;
constexpr size_t kDelayWindow = 9;
constexpr size_t kOffsetWindow = 8;
constexpr double kKp = 0.2;
constexpr double kKi = 0.02;
constexpr double kMaxFreqError = 500e-6;
constexpr double kHoldoverAlpha = 1.0 / 64;  // ~8 s averaging of the frequency estimate at 8 syncs/s

int64_t median(std::deque<int64_t> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

std::vector<uint8_t> port_key(const PortIdentity& p) {
  std::vector<uint8_t> k(p.clock.bytes.begin(), p.clock.bytes.end());
  k.push_back(static_cast<uint8_t>(p.port >> 8));
  k.push_back(static_cast<uint8_t>(p.port));
  return k;
}
}  // namespace

PtpSlave::PtpSlave(uint8_t domain, ClockIdentity self, MediaClock initial) : domain_(domain), clock_(initial) {
  self_.clock = self;
  self_.port = 1;
}

void PtpSlave::on_message(std::span<const uint8_t> bytes, int64_t rx_local) {
  auto m = parse_ptp(bytes);
  if (!m || m->header.domain != domain_) return;
  if (m->header.source.clock == self_.clock) return;

  switch (m->header.type) {
    case PtpMsgType::Announce:
      on_announce(*m, rx_local);
      return;
    case PtpMsgType::Sync:
      if (!master_ || !(m->header.source == *master_)) return;
      if (m->header.two_step()) {
        pending_sync_seq_ = m->header.sequence;
        pending_sync_t2_ = rx_local;
        pending_sync_corr_ = m->header.correction_ns();
      } else {
        pending_sync_seq_.reset();
        measure(m->timestamp.to_ns() + m->header.correction_ns(), rx_local);
      }
      return;
    case PtpMsgType::FollowUp:
      if (!master_ || !(m->header.source == *master_)) return;
      if (pending_sync_seq_ && *pending_sync_seq_ == m->header.sequence) {
        pending_sync_seq_.reset();
        measure(m->timestamp.to_ns() + m->header.correction_ns() + pending_sync_corr_, pending_sync_t2_);
      }
      return;
    case PtpMsgType::DelayResp: {
      if (!master_ || !(m->header.source == *master_) || !(m->requesting == self_)) return;
      if (!pending_delay_seq_ || *pending_delay_seq_ != m->header.sequence) return;
      pending_delay_seq_.reset();
      // Discard exchanges that straddle a clock step: t3 would be on the old time base.
      if (!stepped_once_ || pending_delay_steps_ != steps_ || !have_ms_delay_) return;
      const int64_t t4 = m->timestamp.to_ns() - m->header.correction_ns();
      const int64_t sm_delay = t4 - clock_.to_ptp(pending_delay_t3_);
      const int64_t mpd = (last_ms_delay_ + sm_delay) / 2;
      if (mpd < 0 || mpd > kSecond) return;
      delay_window_.push_back(mpd);
      if (delay_window_.size() > kDelayWindow) delay_window_.pop_front();
      mean_path_delay_ = median(delay_window_);
      return;
    }
    default:
      return;
  }
}

void PtpSlave::on_announce(const PtpMessage& m, int64_t now) {
  auto& entry = foreign_[port_key(m.header.source)];
  entry.first = m.header.source;
  entry.second.announce = m.announce;
  entry.second.last_seen = now;
  const int64_t interval = static_cast<int64_t>(std::ldexp(1.0, std::clamp<int>(m.header.log_interval, -3, 4)) * kSecond);
  entry.second.timeout = std::max(4 * interval, kMinAnnounceTimeout);
  select_master(now);
}

void PtpSlave::select_master(int64_t now) {
  const std::pair<PortIdentity, Foreign>* best = nullptr;
  for (const auto& [key, value] : foreign_) {
    if (!best || compare_announce(value.second.announce, best->second.announce) < 0) best = &value;
  }
  if (!best) {
    if (master_) {
      master_.reset();
      enter_holdover(now);
    }
    return;
  }
  if (!master_ || !(*master_ == best->first)) {
    master_ = best->first;
    reset_servo_windows();
    last_sync_local_ = now;
  }
  grandmaster_ = best->second.announce.gm_identity;
}

void PtpSlave::reset_servo_windows() {
  pending_sync_seq_.reset();
  pending_delay_seq_.reset();
  delay_window_.clear();
  mean_path_delay_.reset();
  have_ms_delay_ = false;
  offset_window_.clear();
  good_count_ = 0;
  big_count_ = 0;
  prev_t2_local_ = 0;
  delay_req_due_ = true;
}

void PtpSlave::step_to(int64_t t2_local, int64_t master_time) {
  clock_.set_mapping(t2_local, master_time, freq_);
  stepped_once_ = true;
  step_pending_ = true;
  ++steps_;
  offset_window_.clear();
  have_ms_delay_ = false;
  good_count_ = 0;
  big_count_ = 0;
  offset_ = 0;
}

void PtpSlave::measure(int64_t t1, int64_t t2_local) {
  last_sync_local_ = t2_local;
  const int64_t mpd = mean_path_delay_.value_or(0);
  if (!stepped_once_) {
    step_to(t2_local, t1 + mpd);
    prev_t2_local_ = t2_local;
    return;
  }

  const int64_t t2 = clock_.to_ptp(t2_local);
  last_ms_delay_ = t2 - t1;
  have_ms_delay_ = true;
  const int64_t raw = t2 - t1 - mpd;

  // Clip outliers (software timestamps see occasional scheduling spikes) against a short median.
  offset_window_.push_back(raw);
  if (offset_window_.size() > kOffsetWindow) offset_window_.pop_front();
  int64_t filtered = raw;
  if (offset_window_.size() >= 4) {
    const int64_t med = median(offset_window_);
    std::deque<int64_t> dev;
    for (auto v : offset_window_) dev.push_back(std::llabs(v - med));
    const int64_t limit = std::max<int64_t>(3 * median(dev), 20'000);
    if (std::llabs(raw - med) > limit) filtered = med;
  }
  offset_ = filtered;

  if (std::llabs(filtered) > kStepThreshold) {
    good_count_ = 0;
    if (++big_count_ >= kStepConfirmations) step_to(t2_local, t1 + mpd);
    prev_t2_local_ = t2_local;
    return;
  }
  big_count_ = 0;

  const double interval = std::max<double>(static_cast<double>(t2_local - prev_t2_local_), 10e6);
  prev_t2_local_ = t2_local;
  sync_interval_ = static_cast<int64_t>(interval);
  coasting_ = false;
  freq_ -= kKi * static_cast<double>(filtered) / interval;
  freq_ = std::clamp(freq_, 1.0 - kMaxFreqError, 1.0 + kMaxFreqError);
  double rate = freq_ - kKp * static_cast<double>(filtered) / interval;
  rate = std::clamp(rate, 1.0 - 2 * kMaxFreqError, 1.0 + 2 * kMaxFreqError);
  clock_.set_mapping(t2_local, t2, rate);  // continuity: same time at t2, new slope

  if (state_ == PtpState::Locked) {
    holdover_freq_ += kHoldoverAlpha * (freq_ - holdover_freq_);
  } else {
    holdover_freq_ = freq_;
  }

  if (std::llabs(filtered) < kLockThreshold) {
    if (++good_count_ >= kLockConfirmations) state_ = PtpState::Locked;
  } else {
    good_count_ = 0;
  }
}

std::optional<std::vector<uint8_t>> PtpSlave::poll_delay_req(int64_t now) {
  if (!master_ || !stepped_once_) return std::nullopt;
  if (!delay_req_due_ && now - last_delay_req_ < kDelayReqInterval) return std::nullopt;
  delay_req_due_ = false;
  last_delay_req_ = now;
  pending_delay_seq_ = ++delay_seq_;
  pending_delay_t3_ = now;
  pending_delay_steps_ = steps_;
  return build_delay_req(domain_, self_, delay_seq_);
}

void PtpSlave::on_delay_req_sent(int64_t tx_local) { pending_delay_t3_ = tx_local; }

void PtpSlave::enter_holdover(int64_t now) {
  // Free-run on the frequency estimate alone: the proportional correction only made sense
  // while measurements were arriving.
  if (stepped_once_) clock_.set_mapping(now, clock_.to_ptp(now), holdover_freq_);
  if (state_ == PtpState::Locked) {
    ++sync_lost_;
    state_ = PtpState::Holdover;
  }
  good_count_ = 0;
}

void PtpSlave::tick(int64_t now) {
  for (auto it = foreign_.begin(); it != foreign_.end();) {
    if (now - it->second.second.last_seen > it->second.second.timeout) {
      it = foreign_.erase(it);
    } else {
      ++it;
    }
  }
  select_master(now);
  // A missed sync must not leave the proportional correction applied indefinitely.
  if (stepped_once_ && !coasting_ && now - last_sync_local_ > std::max<int64_t>(2 * sync_interval_, 250'000'000)) {
    clock_.set_mapping(now, clock_.to_ptp(now), freq_);
    coasting_ = true;
  }
  if (master_ && state_ != PtpState::Holdover && now - last_sync_local_ > kSyncTimeout) enter_holdover(now);
}

bool PtpSlave::consume_step() {
  const bool s = step_pending_;
  step_pending_ = false;
  return s;
}

PtpStatus PtpSlave::status() const {
  PtpStatus s;
  s.state = state_;
  s.has_master = master_.has_value();
  if (master_) {
    s.master = *master_;
    s.grandmaster = grandmaster_;
  }
  s.offset_ns = offset_;
  s.path_delay_ns = mean_path_delay_.value_or(0);
  s.sync_lost = sync_lost_;
  s.steps = steps_;
  s.domain = domain_;
  s.rate_ppm = (clock_.rate() - 1.0) * 1e6;
  return s;
}

}  // namespace bam
