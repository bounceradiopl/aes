// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/rx_stream.hpp"

#include <algorithm>
#include <cmath>

#include "bam/constants.hpp"
#include "bam/gain.hpp"
#include "bam/rtp.hpp"

namespace bam {

namespace {
constexpr int64_t kMaxFutureFrames = kSampleRate / 2;  // 0.5 s
constexpr uint64_t kSourceGoneFrames = kSampleRate;    // 1 s without data: stop counting underruns
constexpr int64_t kSignalTimeoutNs = 1'000'000'000;
}  // namespace

RxStream::RxStream(const SdpStream& sdp, ChannelRing left, ChannelRing right, uint32_t latency_frames, double gain)
    : sdp_(sdp),
      left_(left),
      right_(right),
      latency_(latency_frames),
      gain_(gain),
      bytes_per_frame_(bytes_per_sample(sdp.encoding) * static_cast<size_t>(sdp.channels)) {}

void RxStream::on_packet(std::span<const uint8_t> bytes, uint64_t now_sample, int64_t now_local_ns) {
  auto pkt = parse_rtp(bytes);
  if (!pkt) {
    overruns_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (pkt->header.payload_type != sdp_.payload_type) return;  // another stream on the same group
  if (pkt->payload.empty() || pkt->payload.size() % bytes_per_frame_ != 0) {
    overruns_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const size_t frames = pkt->payload.size() / bytes_per_frame_;

  // A sequence number not newer than the last one is a duplicate or a reordered packet: its samples
  // are still written (idempotent, may fill a hole) but it is not counted again.
  const int16_t seq_delta = seq_started_ ? static_cast<int16_t>(pkt->header.sequence - last_seq_) : 1;
  const bool fresh = seq_delta > 0;

  const uint64_t ts = unwrap_rtp_timestamp(pkt->header.timestamp, now_sample);
  const int64_t deadline = static_cast<int64_t>(now_sample) - latency_;
  if (static_cast<int64_t>(ts + frames) <= deadline ||
      static_cast<int64_t>(ts) > static_cast<int64_t>(now_sample) + kMaxFutureFrames) {
    if (fresh) {
      overruns_.fetch_add(1, std::memory_order_relaxed);
      last_seq_ = pkt->header.sequence;
      seq_started_ = true;
    }
    return;
  }
  if (fresh) {
    if (seq_delta > 1) seq_errors_.fetch_add(1, std::memory_order_relaxed);
    last_seq_ = pkt->header.sequence;
    seq_started_ = true;
  }

  if (scratch_l_.size() < frames) {
    scratch_l_.resize(frames);
    scratch_r_.resize(frames);
  }
  int32_t* planes[] = {scratch_l_.data(), scratch_r_.data()};
  decode_pcm(sdp_.encoding, pkt->payload.data(), frames, sdp_.channels, planes);
  apply_gain(scratch_l_.data(), frames, gain_);
  if (sdp_.channels == 2) apply_gain(scratch_r_.data(), frames, gain_);
  left_.write(ts, scratch_l_.data(), frames);
  right_.write(ts, sdp_.channels == 2 ? scratch_r_.data() : scratch_l_.data(), frames);

  if (!fresh) return;

  // RFC 3550 interarrival jitter, in samples.
  const double transit = static_cast<double>(static_cast<int64_t>(now_sample) - static_cast<int64_t>(ts));
  if (started_) {
    const double d = std::fabs(transit - last_transit_);
    const double j = jitter_samples_.load(std::memory_order_relaxed);
    jitter_samples_.store(j + (d - j) / 16.0, std::memory_order_relaxed);
  }
  last_transit_ = transit;

  if (!started_ || ts + frames > highest_end_) highest_end_ = ts + frames;
  if (!started_) checked_until_ = ts;
  started_ = true;
  packets_.fetch_add(1, std::memory_order_relaxed);
  bytes_.fetch_add(bytes.size(), std::memory_order_relaxed);
  last_packet_ns_.store(now_local_ns, std::memory_order_relaxed);
}

void RxStream::on_tick(uint64_t now_sample) {
  if (!started_ || now_sample < latency_) return;
  const uint64_t deadline = now_sample - latency_;
  const uint64_t from = std::max(highest_end_, checked_until_);
  if (deadline > from && deadline - highest_end_ <= kSourceGoneFrames) {
    const uint64_t missing = deadline - from;
    const uint64_t fpp = std::max<uint32_t>(sdp_.frames_per_packet, 1);
    underruns_.fetch_add((missing + fpp - 1) / fpp, std::memory_order_relaxed);
  }
  checked_until_ = std::max(checked_until_, deadline);
}

RxStats RxStream::stats() const {
  RxStats s;
  s.packets = packets_.load(std::memory_order_relaxed);
  s.bytes = bytes_.load(std::memory_order_relaxed);
  s.underruns = underruns_.load(std::memory_order_relaxed);
  s.overruns = overruns_.load(std::memory_order_relaxed);
  s.seq_errors = seq_errors_.load(std::memory_order_relaxed);
  s.jitter_ms = jitter_samples_.load(std::memory_order_relaxed) * 1000.0 / kSampleRate;
  return s;
}

void RxStream::reset_stats() {
  packets_ = 0;
  bytes_ = 0;
  underruns_ = 0;
  overruns_ = 0;
  seq_errors_ = 0;
  jitter_samples_ = 0;
}

bool RxStream::has_signal(int64_t now_local_ns) const {
  const int64_t last = last_packet_ns_.load(std::memory_order_relaxed);
  return last != 0 && now_local_ns - last <= kSignalTimeoutNs;
}

}  // namespace bam
