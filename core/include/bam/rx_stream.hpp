// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "bam/sdp.hpp"
#include "bam/timeline.hpp"

namespace bam {

// Counters named after the Axia "Livewire Statistics" window.
struct RxStats {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t underruns = 0;   // packets missing when their play-out deadline passed
  uint64_t overruns = 0;    // packets rejected: late, too far in the future or malformed
  uint64_t seq_errors = 0;  // RTP sequence discontinuities
  double jitter_ms = 0;     // RFC 3550 inter-arrival jitter
};

// Receives one AES67 stream and places its samples on the shared timeline at their RTP timestamp.
// on_packet()/on_tick() are called from one network thread; stats()/has_signal() from any thread.
class RxStream {
 public:
  RxStream(const SdpStream& sdp, ChannelRing left, ChannelRing right, uint32_t latency_frames, double gain);

  void on_packet(std::span<const uint8_t> bytes, uint64_t now_sample, int64_t now_local_ns);
  void on_tick(uint64_t now_sample);

  RxStats stats() const;
  void reset_stats();
  bool has_signal(int64_t now_local_ns) const;
  uint32_t latency_frames() const { return latency_; }
  const SdpStream& sdp() const { return sdp_; }

 private:
  SdpStream sdp_;
  ChannelRing left_, right_;
  uint32_t latency_;
  double gain_;
  size_t bytes_per_frame_;

  std::atomic<uint64_t> packets_{0}, bytes_{0}, underruns_{0}, overruns_{0}, seq_errors_{0};
  std::atomic<double> jitter_samples_{0};
  std::atomic<int64_t> last_packet_ns_{0};

  bool started_ = false;
  bool seq_started_ = false;
  uint16_t last_seq_ = 0;
  uint64_t highest_end_ = 0;
  uint64_t checked_until_ = 0;
  double last_transit_ = 0;
  std::vector<int32_t> scratch_l_, scratch_r_;
};

}  // namespace bam
