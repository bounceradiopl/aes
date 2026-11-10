// SPDX-License-Identifier: GPL-3.0-or-later
// AES67 output of a station (spec §3): the station signal on the mix timeline, mapped by the delay
// D onto the RTP timeline. One writer thread calls write(), the sender thread pump(); any thread
// may set_delay(), delay() and stats().
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "bam/timeline.hpp"
#include "bam/tx_stream.hpp"

namespace bam::encoder::detail {

struct Aes67Stats {
  uint64_t packets = 0, gaps = 0, send_errors = 0;
};

class Aes67Output {
 public:
  Aes67Output(uint32_t ptime_us, uint32_t ssrc);

  // Writer side (the one thread that hands the station signal to its streams): stereo float,
  // interleaved; `pos` is the mix-timeline position of the first frame's content.
  void write(const float* stereo, size_t frames, uint64_t pos);

  // Any thread: the delay D (mix position -> RTP timestamp). A change fades out, waits until the
  // new mapping reaches content not sent yet, fades in.
  void set_delay(uint32_t frames);
  uint32_t delay() const { return delay_.load(std::memory_order_relaxed); }

  // Sender thread only: fill the TX ring up to now + one packet and send the due packets.
  // `generation` is the clock's step generation: a change re-aligns (fade-in, TxStream::reset).
  void pump(uint64_t now, uint64_t generation, const PacketSink& send);
  void count_send_error() { tx_->count_send_error(); }

  Aes67Stats stats() const;
  uint32_t frames_per_packet() const { return fpp_; }

 private:
  enum class Gate { Run, FadeOut, Hold, FadeIn };
  struct Rings {
    std::array<std::vector<int32_t>, 2> samples;
    std::array<std::vector<uint32_t>, 2> tags;
    std::array<ChannelRing, 2> ring;
  };
  static void alloc(Rings& r);
  void align(uint64_t now, bool fresh);
  bool content_missing(uint64_t p, size_t n);
  void copy(uint64_t t, size_t n);

  const uint32_t fpp_;
  Rings content_, txr_;
  std::unique_ptr<TxStream> tx_;
  std::array<std::vector<int32_t>, 2> wbuf_, rbuf_;  // write() and pump() scratch

  std::atomic<uint32_t> delay_{0};
  std::atomic<uint64_t> written_end_{0};
  std::atomic<uint64_t> gaps_{0};

  // gate, sender thread only
  bool aligned_ = false;
  bool started_ = false;    // content was copied: delay changes fade from now on
  uint64_t generation_ = 0;
  uint64_t out_ = 0;        // next TX ring position to fill
  uint32_t cur_delay_ = 0;  // D the gate maps with
  Gate gate_ = Gate::FadeIn;
  uint32_t level_ = 0;      // gain = level_ / kRampFrames
  uint64_t sent_end_ = 0;   // highest source position copied + 1
  uint64_t gap_chunk_ = UINT64_MAX;  // TX chunk (t / fpp_) last counted as a gap
  // a delay change in progress: a hole in the content (the old path ends, the new one starts later)
  // is not a gap, until content is there again (or settle_end_)
  bool settling_ = false;
  bool settle_missing_ = false;
  uint64_t settle_end_ = 0;
  bool content_seen_ = false;  // content was there since the last fresh alignment: holes count as gaps
};

}  // namespace bam::encoder::detail
