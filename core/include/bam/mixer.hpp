// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "bam/config.hpp"
#include "bam/constants.hpp"
#include "bam/rate_control.hpp"
#include "bam/shm_layout.hpp"
#include "bam/timeline.hpp"

namespace bam {

inline constexpr uint32_t kMixMarginFrames = 96;  // 2 ms: a mix is ready this long after its slowest member
inline constexpr uint32_t kMixRampFrames = 240;   // 5 ms: gain, mute, join and leave fade over this long

struct MixInput {
  ChannelRing left, right;
  double gain = 1.0;   // linear; 0 = muted
  uint32_t delay = 0;  // D_src of the member
};

struct MixBusPlan {
  int bus = 0;         // 0..kMixBuses-1
  uint64_t owner = 0;  // the destination using the bus (a hash of its id); a new owner starts the bus clean
  ChannelRing out_left, out_right;
  std::vector<MixInput> inputs;  // at most kMaxMixSources are mixed
  uint32_t max_delay = 0;        // max D_src of the inputs
};

// Sums the members of every mix bus onto the bus rings, in 48-frame blocks, up to
// "now - max_delay" of each bus. process() runs on one thread; set_plan() from any thread.
//
// Levels never step: each member's gain moves linearly to its new value over kMixRampFrames, per
// sample. A member is recognised across plans by its left ring (MixInput::left.samples()), so the
// order of the inputs does not matter; a member that joins fades in from silence, one that leaves
// is dropped, and a muted member is skipped once its fade has reached zero. While every audible
// member sits at exactly 0 dB the bus sums in integers, bit-exact.
class Mixer {
 public:
  Mixer();
  void set_plan(std::vector<MixBusPlan> plan);
  void process(uint64_t now_s, uint64_t generation);
  uint64_t late() const { return late_.load(std::memory_order_relaxed); }
  float peak_db(int bus, int side) const;

 private:
  static constexpr size_t kMaxInputs = kMaxMixSources;
  static constexpr uint32_t kBlock = 48;
  struct Level {
    const int32_t* key = nullptr;  // the member's left ring
    double gain = 0.0;             // applied to the last mixed sample
    double from = 0.0;             // where the current fade started
    double target = 0.0;
    uint32_t left = 0;  // samples still to go in the current fade
    bool audible() const { return gain != 0.0 || left != 0; }
    void advance() {
      if (left == 0) return;
      --left;
      gain = left == 0 ? target : target + (from - target) * (static_cast<double>(left) / kMixRampFrames);
    }
  };
  struct State {
    bool started = false;
    uint64_t mixed_upto = 0;
    uint64_t owner = 0;
    size_t count = 0;  // members in `levels`, in the order of the plan's inputs
    std::array<Level, kMaxInputs> levels{};
  };
  void follow_plan(State& st, const MixBusPlan& bus);
  void mix_block(State& st, const MixBusPlan& bus, float peak[2]);

  std::mutex plan_mutex_;
  std::shared_ptr<const std::vector<MixBusPlan>> plan_;
  std::array<State, kMixBuses> state_{};
  uint64_t generation_ = 0;
  std::atomic<uint64_t> late_{0};
  std::array<std::atomic<float>, kMixBuses * 2> peak_;
  // scratch for one block, kept here so process() never allocates
  std::array<Level, kMaxInputs> next_levels_{};
  std::array<int32_t, kBlock> in_l_{}, in_r_{}, out_l_{}, out_r_{};
  std::array<double, kBlock> acc_l_{}, acc_r_{};
  std::array<int64_t, kBlock> sum_l_{}, sum_r_{};
};

}  // namespace bam
