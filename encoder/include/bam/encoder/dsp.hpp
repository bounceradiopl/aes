// SPDX-License-Identifier: GPL-3.0-or-later
// Signal path of a Bounce Encoder station (spec §4): per-input delay line, station mixer, and per
// stream gain + fixed-ratio resampling to the stream's sample rate. Audio is interleaved float
// stereo, full scale ±1.0, at 48 kHz on the station side.
//
// Threading: each object belongs to one worker thread (the station thread for DelayLine and
// StationMixer, the stream worker for StreamConditioner). Every method, the parameter setters
// included, must be called from that thread, setters between blocks; nothing locks and the per
// block calls (write/read/mix/process) never allocate. The one exception is
// StationMixer::take_peaks(), which any thread may call (lock-free atomics).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bam {
class DriftResampler;
}

namespace bam::encoder {

inline constexpr uint32_t kStationRate = 48000;  // the station side of the signal path
inline constexpr uint32_t kRampFrames = 240;     // 5 ms: every gain, mute, join, leave and delay change
inline constexpr int kStationInputs = 3;
inline constexpr std::array<uint32_t, 5> kStreamRates = {48000, 44100, 32000, 24000, 22050};  // spec §2

// A gain that moves linearly, per frame, to each new target over kRampFrames; a new target set
// mid-ramp starts from the level reached. Shared by the mixer inputs and the stream gain.
class GainRamp {
 public:
  GainRamp() : GainRamp(0.0) {}
  explicit GainRamp(double level) : level_(level), from_(level), target_(level) {}
  void set_target(double target) {
    if (target == target_) return;
    from_ = level_;
    target_ = target;
    left_ = kRampFrames;
  }
  double next() {  // the gain for the next frame
    if (left_ != 0) skip(1);
    return level_;
  }
  void skip(size_t frames) {  // moves the ramp on by `frames` without applying it
    left_ -= static_cast<uint32_t>(std::min<size_t>(left_, frames));
    level_ = left_ == 0 ? target_ : target_ + (from_ - target_) * (static_cast<double>(left_) / kRampFrames);
  }
  double level() const { return level_; }  // the gain of the last frame
  bool ramping() const { return left_ != 0; }

 private:
  double level_, from_, target_;
  uint32_t left_ = 0;  // frames still to go
};

// Delays a stereo signal by a whole number of frames (spec §4.2). After write(n) the next read(n)
// returns the frames written delay_frames() earlier; before that much has been written it
// returns silence. A delay change while running never steps the level: the output fades out over
// kRampFrames, the read position jumps (a shorter delay skips audio, a longer one inserts silence
// of the difference), and it fades back in over kRampFrames. Before the first read() the delay
// is set at once, with no fade; setting the delay the line is at or heading to does nothing.
class DelayLine {
 public:
  // Room for the frames written ahead of a read on top of the longest delay (the station thread
  // writes a pass of up to ~0.5 s, then reads it).
  static constexpr uint32_t kHeadroomFrames = kStationRate / 2;

  explicit DelayLine(double max_seconds = 60.5);  // ring capacity; the longest delay is 0.5 s shorter

  void write(const float* stereo, size_t frames);
  void read(float* out, size_t frames);

  // Rounds to whole frames; false (delay unchanged) when negative or longer than max_delay_frames().
  bool set_delay_seconds(double seconds);
  uint32_t delay_frames() const { return target_; }  // the delay set last (reached after a fade)
  uint32_t capacity_frames() const { return capacity_; }
  uint32_t max_delay_frames() const { return capacity_ > kHeadroomFrames ? capacity_ - kHeadroomFrames : 0; }

 private:
  enum class Phase { Steady, FadeOut, Silence, FadeIn };
  std::vector<float> ring_;   // capacity_ stereo frames
  uint32_t capacity_;
  int64_t written_ = 0;       // frames written in total
  int64_t cursor_;            // absolute index of the next frame to read (negative = not yet written)
  uint32_t current_;          // delay in effect for the cursor (written_ - cursor_ between write/read pairs)
  uint32_t target_;           // delay to reach
  uint32_t level_ = kRampFrames;  // gain = level_ / kRampFrames
  Phase phase_ = Phase::Steady;
  bool started_ = false;      // read() was called: delay changes fade from now on
};

// Sums up to kStationInputs stereo inputs, each with a linear gain (spec §4.3). The effective gain
// of an input is gain when it is enabled and not muted, else 0; whenever it changes the input's
// level moves linearly, per frame, to the new value over kRampFrames. Inputs start disabled (an
// input that is enabled fades in from silence). The sum is float with no clipping: saturation
// happens where the mix is converted to PCM.
class StationMixer {
 public:
  struct Peaks {
    std::array<std::array<float, 2>, kStationInputs> input{};  // per input and side, before its gain
    std::array<float, 2> mix{};                                // the mix, per side
  };

  StationMixer() = default;

  void set_gain(int input, double linear);
  void set_muted(int input, bool muted);
  void set_enabled(int input, bool enabled);
  // True while the input is heard or still fading out: until then the caller keeps feeding it.
  bool audible(int input) const;

  // inputs[i] is a block of `frames` stereo frames, or nullptr for silence; writes the sum to out.
  void mix(const float* const inputs[kStationInputs], size_t frames, float* out);

  // Largest absolute sample per side since the last call, linear (1.0 = full scale); clears them.
  Peaks take_peaks();

 private:
  struct Input {
    double gain = 1.0;
    bool muted = false;
    bool enabled = false;
    GainRamp ramp;  // toward gain while enabled and not muted, else 0
  };
  static void retarget(Input& in);
  static void hold_peak(std::atomic<float>& peak, float value);

  std::array<Input, kStationInputs> inputs_{};
  std::array<std::atomic<float>, kStationInputs * 2> input_peaks_{};
  std::array<std::atomic<float>, 2> mix_peaks_{};
};

// Conditions the station mix for one stream (spec §4.4): the stream gain (linear, ramped over
// kRampFrames at 48 kHz), then a fixed-ratio resampling 48000 -> out_rate (libsamplerate
// SRC_SINC_MEDIUM_QUALITY). At 48000 the samples pass through unchanged (bit-exact at gain 1).
// Output frames track input × out_rate / 48000 to within a frame from the first block on; the
// resampler's look-ahead is primed at construction. No clipping: the encoder's PCM conversion
// saturates.
class StreamConditioner {
 public:
  // out_rate: one of kStreamRates, else std::invalid_argument; starts at `gain` with no ramp.
  explicit StreamConditioner(uint32_t out_rate, double gain = 1.0);
  ~StreamConditioner();
  StreamConditioner(const StreamConditioner&) = delete;
  StreamConditioner& operator=(const StreamConditioner&) = delete;

  void set_gain(double linear);
  uint32_t out_rate() const { return out_rate_; }
  // Output frames process() can write for in_frames input frames; size `out` for this.
  size_t max_out_frames(size_t in_frames) const;
  // Returns the number of stereo frames written to out (at most out_capacity). Input that does
  // not fit (out_capacity below max_out_frames) is dropped and counted in dropped_frames().
  size_t process(const float* in, size_t in_frames, float* out, size_t out_capacity);
  uint64_t dropped_frames() const { return dropped_; }  // input frames, since construction

 private:
  static constexpr size_t kChunkFrames = 1024;
  static constexpr size_t kOutSlackFrames = 4;  // max_out_frames() margin over in × ratio
  uint32_t out_rate_;
  double ratio_;
  std::unique_ptr<DriftResampler> resampler_;  // null at 48000
  std::vector<float> scratch_;                 // kChunkFrames stereo frames after the gain
  GainRamp gain_;
  uint64_t dropped_ = 0;
};

}  // namespace bam::encoder
