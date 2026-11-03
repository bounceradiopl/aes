// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "bam/constants.hpp"
#include "bam/rate_control.hpp"
#include "bam/shm_layout.hpp"
#include "bam/timeline.hpp"

namespace bam {

inline constexpr uint32_t kMixMarginFrames = 96;  // 2 ms: a mix is ready this long after its slowest member

struct MixInput {
  ChannelRing left, right;
  double gain = 1.0;   // linear; 0 = muted
  uint32_t delay = 0;  // D_src of the member
};

struct MixBusPlan {
  int bus = 0;  // 0..kMixBuses-1
  ChannelRing out_left, out_right;
  std::vector<MixInput> inputs;
  uint32_t max_delay = 0;  // max D_src of the inputs
};

// Sums the members of every mix bus onto the bus rings, in 48-frame blocks, up to
// "now - max_delay" of each bus. process() runs on one thread; set_plan() from any thread.
class Mixer {
 public:
  Mixer();
  void set_plan(std::vector<MixBusPlan> plan);
  void process(uint64_t now_s, uint64_t generation);
  uint64_t late() const { return late_.load(std::memory_order_relaxed); }
  float peak_db(int bus, int side) const;

 private:
  struct State {
    bool started = false;
    uint64_t mixed_upto = 0;
  };
  std::mutex plan_mutex_;
  std::shared_ptr<const std::vector<MixBusPlan>> plan_;
  std::array<State, kMixBuses> state_{};
  uint64_t generation_ = 0;
  std::atomic<uint64_t> late_{0};
  std::array<std::atomic<float>, kMixBuses * 2> peak_;
};

}  // namespace bam
