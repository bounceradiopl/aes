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

inline constexpr uint32_t kMixMarginFrames = 192;  // 4 ms: a mix is ready this long after its slowest member
// margin - block(48) = 144 frames (~3 ms) of slack for the mixer thread's wake-up scheduling
// jitter (engine/src/stream_workers.cpp mix_loop, 1 ms cadence); 96 (2 ms, 1 ms slack) was not
// enough for Windows timer jitter on a loaded CI runner (ruling R16: Windows CI saw mixer_late > 0
// and consumers reading frames not yet mixed; Linux/macOS, with tighter scheduling, did not).
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
// order of the inputs does not matter; a member that joins fades in from silence, a muted member
// is skipped once its fade has reached zero, and a member missing from a new plan fades out over
// kMixRampFrames instead of dropping at once (it keeps playing its own ring, bounded to
// kMaxInputs "leaving" members per bus — the oldest is dropped first if that fills up) and is
// matched again, continuing its level with no jump, if it reappears in a later plan before the
// fade finishes. While every audible member sits at exactly 0 dB and none are leaving, the bus
// sums in integers, bit-exact.
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
  struct Leaving {
    MixInput input;  // the member's own rings, kept so it can still be read once it is off the plan
    Level level;     // always fading toward 0 while held here
  };
  struct State {
    bool started = false;
    uint64_t mixed_upto = 0;
    uint64_t owner = 0;
    size_t count = 0;  // members in `levels`/`inputs`, in the order of the plan's inputs
    std::array<Level, kMaxInputs> levels{};
    std::array<MixInput, kMaxInputs> inputs{};  // this bus's current members' rings, parallel to `levels`
    size_t leaving_count = 0;
    std::array<Leaving, kMaxInputs> leaving{};  // members no longer in the plan, still fading to 0
  };
  void follow_plan(State& st, const MixBusPlan& bus);
  void mix_block(State& st, const MixBusPlan& bus, float peak[2]);
  // Moves a member that just fell out of the plan into the leaving set so it keeps fading out.
  // Bounded by kMaxInputs: if already full, the oldest leaving member is dropped (cut short)
  // to make room, rather than growing or allocating.
  static void start_leaving(State& st, const MixInput& input, const Level& level);

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
