// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/mixer.hpp"

#include <algorithm>
#include <cmath>

#include "bam/rate_control.hpp"

namespace bam {

namespace {
constexpr uint64_t kRestartFrames = 4800;  // 100 ms off: start over at "now"

int32_t saturate(int64_t v) { return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX)); }
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

// Carries every member's level over from the previous plan (matched by its left ring) and starts a
// fade wherever the plan's gain differs from where the member is heading.
void Mixer::follow_plan(State& st, const MixBusPlan& bus) {
  const size_t n = std::min(bus.inputs.size(), kMaxInputs);
  for (size_t i = 0; i < n; ++i) {
    Level l;
    l.key = bus.inputs[i].left.samples();
    for (size_t j = 0; j < st.count; ++j)
      if (st.levels[j].key == l.key) {
        l = st.levels[j];
        break;
      }
    const double target = bus.inputs[i].gain;
    if (target != l.target) {
      l.from = l.gain;
      l.target = target;
      l.left = l.gain == target ? 0 : kMixRampFrames;
    }
    next_levels_[i] = l;
  }
  std::copy_n(next_levels_.begin(), n, st.levels.begin());
  st.count = n;
}

void Mixer::mix_block(State& st, const MixBusPlan& bus, float peak[2]) {
  bool exact = true;  // every audible member at exactly 0 dB, none fading: integer sum
  for (size_t k = 0; k < st.count; ++k) {
    const Level& l = st.levels[k];
    if (l.audible() && (l.left != 0 || l.gain != 1.0)) exact = false;
  }
  if (exact) {
    sum_l_.fill(0);
    sum_r_.fill(0);
    for (size_t k = 0; k < st.count; ++k) {
      if (!st.levels[k].audible()) continue;
      bus.inputs[k].left.read(st.mixed_upto, in_l_.data(), kBlock);
      bus.inputs[k].right.read(st.mixed_upto, in_r_.data(), kBlock);
      for (uint32_t i = 0; i < kBlock; ++i) {
        sum_l_[i] += in_l_[i];
        sum_r_[i] += in_r_[i];
      }
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
      out_l_[i] = saturate(sum_l_[i]);
      out_r_[i] = saturate(sum_r_[i]);
    }
  } else {
    acc_l_.fill(0.0);
    acc_r_.fill(0.0);
    for (size_t k = 0; k < st.count; ++k) {
      Level& l = st.levels[k];
      if (!l.audible()) continue;  // muted and faded out: not even read
      bus.inputs[k].left.read(st.mixed_upto, in_l_.data(), kBlock);
      bus.inputs[k].right.read(st.mixed_upto, in_r_.data(), kBlock);
      if (l.left == 0) {
        for (uint32_t i = 0; i < kBlock; ++i) {
          acc_l_[i] += static_cast<double>(in_l_[i]) * l.gain;
          acc_r_[i] += static_cast<double>(in_r_[i]) * l.gain;
        }
      } else {
        for (uint32_t i = 0; i < kBlock; ++i) {
          l.advance();
          acc_l_[i] += static_cast<double>(in_l_[i]) * l.gain;
          acc_r_[i] += static_cast<double>(in_r_[i]) * l.gain;
        }
      }
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
      out_l_[i] = saturate(std::llround(acc_l_[i]));
      out_r_[i] = saturate(std::llround(acc_r_[i]));
    }
  }
  for (uint32_t i = 0; i < kBlock; ++i) {
    peak[0] = std::max(peak[0], std::abs(to_float(out_l_[i])));
    peak[1] = std::max(peak[1], std::abs(to_float(out_r_[i])));
  }
}

void Mixer::process(uint64_t now_s, uint64_t generation) {
  std::shared_ptr<const std::vector<MixBusPlan>> plan;
  {
    std::lock_guard lock(plan_mutex_);
    plan = plan_;
  }
  if (generation != generation_) {
    // a clock step restarts the timing of every bus; the members keep their levels
    generation_ = generation;
    for (auto& s : state_) s.started = false;
  }
  std::array<bool, kMixBuses> in_plan{};
  for (const auto& bus : *plan) {
    if (bus.bus < 0 || bus.bus >= kMixBuses) continue;
    in_plan[bus.bus] = true;
    State& st = state_[bus.bus];
    if (bus.owner != st.owner) {
      st = State{};  // another destination took the bus: nothing of the previous mix carries over
      st.owner = bus.owner;
    }
    follow_plan(st, bus);
    const uint64_t limit = now_s > bus.max_delay ? now_s - bus.max_delay : 0;
    if (!st.started || st.mixed_upto + kRestartFrames < limit || st.mixed_upto > limit + kRestartFrames) {
      // Members are complete up to limit and consumers read D_mix = max_delay + kMixMarginFrames
      // behind "now": starting the margin back gives them mixed audio from their very first read.
      st.mixed_upto = limit > kMixMarginFrames ? limit - kMixMarginFrames : 0;
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
      mix_block(st, bus, peak);
      out_left.write(st.mixed_upto, out_l_.data(), kBlock);
      out_right.write(st.mixed_upto, out_r_.data(), kBlock);
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
  // a bus that left the plan starts clean when it is used again
  for (int b = 0; b < kMixBuses; ++b)
    if (!in_plan[b] && (state_[b].started || state_[b].count != 0)) state_[b] = State{};
}

}  // namespace bam
