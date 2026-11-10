// SPDX-License-Identifier: GPL-3.0-or-later
#include "aes67_output.hpp"

#include <algorithm>
#include <climits>

#include "bam/config.hpp"  // ptime_us_to_frames
#include "bam/constants.hpp"
#include "bam/encoder/dsp.hpp"  // kRampFrames, kStationRate

namespace bam::encoder::detail {

namespace {
constexpr size_t kWriteChunk = 1024;
// A sender this far behind (or the clock this far back) starts over instead of filling the backlog.
constexpr uint64_t kMaxFillFrames = kStationRate / 10;

// The inverse of the station's int32 -> float scale, so gain 1 is bit exact.
int32_t to_int32(float f) {
  if (f >= 1.0f) return INT32_MAX;
  if (f <= -1.0f) return INT32_MIN;
  return f == f ? static_cast<int32_t>(static_cast<double>(f) * 2147483648.0) : 0;
}
}  // namespace

void Aes67Output::alloc(Rings& r) {
  for (int ch = 0; ch < 2; ++ch) {
    r.samples[ch].assign(kRingFrames, 0);
    r.tags[ch].assign(kBlocksPerRing, 0);
    r.ring[ch] = ChannelRing(r.samples[ch].data(), r.tags[ch].data());
  }
}

Aes67Output::Aes67Output(uint32_t ptime_us, uint32_t ssrc) : fpp_(ptime_us_to_frames(ptime_us)) {
  alloc(content_);
  alloc(txr_);
  TxConfig cfg;
  cfg.frames_per_packet = fpp_;
  cfg.payload_type = 98;
  cfg.ssrc = ssrc;
  cfg.gain = 1.0;
  cfg.source_delay = 0;
  tx_ = std::make_unique<TxStream>(cfg, txr_.ring[0], txr_.ring[1]);
  for (int ch = 0; ch < 2; ++ch) {
    wbuf_[ch].assign(kWriteChunk, 0);
    rbuf_[ch].assign(fpp_, 0);
  }
}

void Aes67Output::write(const float* stereo, size_t frames, uint64_t pos) {
  for (size_t done = 0; done < frames;) {
    const size_t n = std::min(frames - done, kWriteChunk);
    for (size_t f = 0; f < n; ++f) {
      wbuf_[0][f] = to_int32(stereo[(done + f) * 2]);
      wbuf_[1][f] = to_int32(stereo[(done + f) * 2 + 1]);
    }
    content_.ring[0].write(pos + done, wbuf_[0].data(), n);
    content_.ring[1].write(pos + done, wbuf_[1].data(), n);
    done += n;
  }
  const uint64_t end = pos + frames;
  if (end > written_end_.load(std::memory_order_relaxed)) written_end_.store(end, std::memory_order_release);
}

void Aes67Output::set_delay(uint32_t frames) { delay_.store(frames, std::memory_order_relaxed); }

Aes67Stats Aes67Output::stats() const {
  const TxStats s = tx_->stats();
  return Aes67Stats{s.packets, gaps_.load(std::memory_order_relaxed), s.errors};
}

void Aes67Output::align(uint64_t now) {
  out_ = now;
  cur_delay_ = delay_.load(std::memory_order_relaxed);
  gate_ = Gate::FadeIn;
  level_ = 0;
  // TxStream starts with the newest complete packet, which lies before out_
  const uint64_t from = now >= 2 * uint64_t(fpp_) ? now - 2 * uint64_t(fpp_) : 0;
  txr_.ring[0].write(from, nullptr, now - from);
  txr_.ring[1].write(from, nullptr, now - from);
  aligned_ = true;
}

bool Aes67Output::content_missing(uint64_t p, size_t n) {
  if (p + n > written_end_.load(std::memory_order_acquire)) return true;
  for (uint64_t b = p - p % kBlockFrames; b < p + n; b += kBlockFrames) {
    std::atomic_ref<uint32_t> tag(content_.tags[0][(b & kRingMask) / kBlockFrames]);
    if (tag.load(std::memory_order_acquire) != lap_tag(b)) return true;
  }
  return false;
}

void Aes67Output::copy(uint64_t t, size_t n) {
  const uint64_t p = t - cur_delay_;
  if (content_missing(p, n) && t / fpp_ != gap_chunk_) {
    gap_chunk_ = t / fpp_;
    gaps_.fetch_add(1, std::memory_order_relaxed);
  }
  int32_t* l = rbuf_[0].data();
  int32_t* r = rbuf_[1].data();
  content_.ring[0].read(p, l, n);
  content_.ring[1].read(p, r, n);
  if (gate_ != Gate::Run) {
    const bool in = gate_ == Gate::FadeIn;
    for (size_t f = 0; f < n; ++f) {
      level_ = in ? level_ + 1 : level_ - 1;
      l[f] = static_cast<int32_t>(int64_t(l[f]) * level_ / kRampFrames);
      r[f] = static_cast<int32_t>(int64_t(r[f]) * level_ / kRampFrames);
    }
  }
  txr_.ring[0].write(t, l, n);
  txr_.ring[1].write(t, r, n);
  sent_end_ = std::max(sent_end_, p + n);
  started_ = true;
}

void Aes67Output::pump(uint64_t now, uint64_t generation, const PacketSink& send) {
  const uint64_t end = now + fpp_;
  if (!aligned_ || generation != generation_ || out_ > end + kMaxFillFrames) {
    generation_ = generation;
    tx_->reset();
    align(now);
  } else if (end - std::min(end, out_) > kMaxFillFrames) {
    align(now);  // the sender stalled: TxStream skips (and counts) the backlog itself
  }
  while (out_ < end) {
    const uint32_t want = delay_.load(std::memory_order_relaxed);
    if (want != cur_delay_) {
      if (!started_ || gate_ == Gate::Hold) {
        cur_delay_ = want;
      } else if (gate_ != Gate::FadeOut) {
        gate_ = Gate::FadeOut;  // from the level reached
      }
    }
    if (gate_ == Gate::FadeOut && level_ == 0) {
      cur_delay_ = want;
      gate_ = Gate::Hold;
    }
    if (gate_ == Gate::Hold) {
      // silence until the new mapping reaches content not sent yet
      const uint64_t resume = sent_end_ + cur_delay_;
      if (out_ < resume) {
        const uint64_t n = std::min(end, resume) - out_;
        txr_.ring[0].write(out_, nullptr, n);
        txr_.ring[1].write(out_, nullptr, n);
        out_ += n;
        continue;
      }
      gate_ = Gate::FadeIn;
    }
    if (gate_ == Gate::FadeIn && level_ == kRampFrames) gate_ = Gate::Run;
    // one TX packet's chunk at most, so gaps count once per packet
    size_t n = static_cast<size_t>(std::min<uint64_t>(end - out_, fpp_ - out_ % fpp_));
    if (gate_ == Gate::FadeIn) n = std::min<size_t>(n, kRampFrames - level_);
    if (gate_ == Gate::FadeOut) n = std::min<size_t>(n, level_);
    copy(out_, n);
    out_ += n;
  }
  tx_->pump(now, send);
}

}  // namespace bam::encoder::detail
