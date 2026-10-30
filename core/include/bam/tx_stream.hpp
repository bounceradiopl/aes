// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "bam/timeline.hpp"

namespace bam {

struct TxConfig {
  uint32_t frames_per_packet = 48;
  uint8_t payload_type = 98;
  uint32_t ssrc = 0;
  double gain = 1.0;
  uint32_t source_delay = 0;  // D_src: packet T carries source samples [T - source_delay, ...)
};

struct TxStats {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t errors = 0;  // packets skipped because the sender fell behind by more than 10 ms, or failed sends
};

using PacketSink = std::function<void(std::span<const uint8_t>)>;

// Turns two timeline rings into an L24 stereo AES67 stream. A packet with timestamp T carries
// samples [T, T + P) and is sent once the media clock reaches T + P.
class TxStream {
 public:
  TxStream(TxConfig cfg, ChannelRing left, ChannelRing right);

  size_t pump(uint64_t now_sample, const PacketSink& send);
  void reset();  // after a clock step: the next pump re-aligns without counting errors
  void count_send_error() { errors_.fetch_add(1, std::memory_order_relaxed); }

  TxStats stats() const;
  void reset_stats();
  const TxConfig& config() const { return cfg_; }

 private:
  TxConfig cfg_;
  ChannelRing left_, right_;
  bool aligned_ = false;
  uint64_t next_ = 0;
  uint16_t sequence_;
  std::vector<int32_t> l_, r_;
  std::vector<uint8_t> packet_;
  std::atomic<uint64_t> packets_{0}, bytes_{0}, errors_{0};
};

}  // namespace bam
