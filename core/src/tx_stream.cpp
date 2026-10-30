// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/tx_stream.hpp"

#include "bam/gain.hpp"
#include "bam/rtp.hpp"

namespace bam {

TxStream::TxStream(TxConfig cfg, ChannelRing left, ChannelRing right)
    : cfg_(cfg),
      left_(left),
      right_(right),
      sequence_(static_cast<uint16_t>(cfg.ssrc * 2654435761u >> 16)),
      l_(cfg.frames_per_packet),
      r_(cfg.frames_per_packet),
      packet_(kRtpHeaderSize + size_t(cfg.frames_per_packet) * 6) {}

size_t TxStream::pump(uint64_t now, const PacketSink& send) {
  const uint64_t p = cfg_.frames_per_packet;
  const uint64_t latest = now >= p ? (now / p) * p - p : 0;  // newest packet that is complete
  if (!aligned_) {
    next_ = latest;
    aligned_ = true;
  }
  if (now >= next_ + p && now - (next_ + p) > 2 * p) {
    errors_.fetch_add((latest - next_) / p, std::memory_order_relaxed);
    next_ = latest;
  }
  size_t sent = 0;
  while (next_ + p <= now) {
    left_.read(next_, l_.data(), p);
    right_.read(next_, r_.data(), p);
    apply_gain(l_.data(), p, cfg_.gain);
    apply_gain(r_.data(), p, cfg_.gain);
    RtpHeader h;
    h.payload_type = cfg_.payload_type;
    h.sequence = sequence_++;
    h.timestamp = static_cast<uint32_t>(next_);
    h.ssrc = cfg_.ssrc;
    write_rtp_header(h, packet_.data());
    const int32_t* planes[] = {l_.data(), r_.data()};
    encode_l24(planes, 2, p, packet_.data() + kRtpHeaderSize);
    send(packet_);
    packets_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(packet_.size(), std::memory_order_relaxed);
    next_ += p;
    ++sent;
  }
  return sent;
}

void TxStream::reset() { aligned_ = false; }

TxStats TxStream::stats() const {
  return TxStats{packets_.load(std::memory_order_relaxed), bytes_.load(std::memory_order_relaxed),
                 errors_.load(std::memory_order_relaxed)};
}

void TxStream::reset_stats() {
  packets_ = 0;
  bytes_ = 0;
  errors_ = 0;
}

}  // namespace bam
