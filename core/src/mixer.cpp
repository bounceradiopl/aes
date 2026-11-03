// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/mixer.hpp"

#include <algorithm>
#include <cmath>

#include "bam/rate_control.hpp"

namespace bam {

namespace {
constexpr uint32_t kBlock = 48;
constexpr uint64_t kRestartFrames = 4800;  // 100 ms off: start over at "now"
}  // namespace

Mixer::Mixer() : plan_(std::make_shared<std::vector<MixBusPlan>>()) {
  for (auto& p : peak_) p.store(-120.0f);
}

void Mixer::set_plan(std::vector<MixBusPlan> plan) {
  auto next = std::make_shared<const std::vector<MixBusPlan>>(std::move(plan));
  std::lock_guard lock(plan_mutex_);
  plan_ = std::move(next);
}

float Mixer::peak_db(int bus, int side) const {
  if (bus < 0 || bus >= kMixBuses) return -120.0f;
  return peak_[bus * 2 + side].load(std::memory_order_relaxed);
}

void Mixer::process(uint64_t now_s, uint64_t generation) {
  std::shared_ptr<const std::vector<MixBusPlan>> plan;
  {
    std::lock_guard lock(plan_mutex_);
    plan = plan_;
  }
  if (generation != generation_) {
    generation_ = generation;
    for (auto& s : state_) s.started = false;
  }
  std::vector<int32_t> in(kBlock), out_l(kBlock), out_r(kBlock);
  std::vector<double> acc_l(kBlock), acc_r(kBlock);
  for (const auto& bus : *plan) {
    if (bus.bus < 0 || bus.bus >= kMixBuses) continue;
    State& st = state_[bus.bus];
    const uint64_t limit = now_s > bus.max_delay ? now_s - bus.max_delay : 0;
    if (!st.started || st.mixed_upto + kRestartFrames < limit || st.mixed_upto > limit + kRestartFrames) {
      st.mixed_upto = limit;
      st.started = true;
    } else if (limit > st.mixed_upto + kMixMarginFrames) {
      late_.fetch_add(1, std::memory_order_relaxed);  // consumers may already have read unmixed frames
    }
    // Local non-const copies: ChannelRing::write() is non-const (it mutates the pointed-to ring
    // storage, not the ChannelRing object itself), but *plan is a const vector, so bus is const.
    ChannelRing out_left = bus.out_left;
    ChannelRing out_right = bus.out_right;
    float peak[2] = {0, 0};
    bool mixed_any = false;
    while (st.mixed_upto + kBlock <= limit) {
      mixed_any = true;
      std::fill(acc_l.begin(), acc_l.end(), 0.0);
      std::fill(acc_r.begin(), acc_r.end(), 0.0);
      for (const auto& m : bus.inputs) {
        if (m.gain == 0.0) continue;
        m.left.read(st.mixed_upto, in.data(), kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) acc_l[i] += static_cast<double>(in[i]) * m.gain;
        m.right.read(st.mixed_upto, in.data(), kBlock);
        for (uint32_t i = 0; i < kBlock; ++i) acc_r[i] += static_cast<double>(in[i]) * m.gain;
      }
      for (uint32_t i = 0; i < kBlock; ++i) {
        out_l[i] = from_float(static_cast<float>(acc_l[i] / 2147483648.0));
        out_r[i] = from_float(static_cast<float>(acc_r[i] / 2147483648.0));
        // exact for sums that fit: keep integer arithmetic when no clipping happens
        if (std::abs(acc_l[i]) < 2147483392.0) out_l[i] = static_cast<int32_t>(std::lround(acc_l[i]));
        if (std::abs(acc_r[i]) < 2147483392.0) out_r[i] = static_cast<int32_t>(std::lround(acc_r[i]));
        peak[0] = std::max(peak[0], std::abs(to_float(out_l[i])));
        peak[1] = std::max(peak[1], std::abs(to_float(out_r[i])));
      }
      out_left.write(st.mixed_upto, out_l.data(), kBlock);
      out_right.write(st.mixed_upto, out_r.data(), kBlock);
      st.mixed_upto += kBlock;
    }
    // Only touch the reported peak when this call actually mixed a block: a pass that mixes
    // nothing (less than one block of new "now" since the last call) must leave the last real
    // peak in place, not report silence.
    if (mixed_any) {
      for (int c = 0; c < 2; ++c)
        peak_[bus.bus * 2 + c].store(peak[c] > 1e-6f ? 20.0f * std::log10(peak[c]) : -120.0f, std::memory_order_relaxed);
    }
  }
}

}  // namespace bam
