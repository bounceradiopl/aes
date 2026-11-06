// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>

#include "bam/encoder/dsp.hpp"

namespace bam::encoder {

namespace {
bool valid(int input) { return input >= 0 && input < kStationInputs; }
}  // namespace

void StationMixer::retarget(Input& in) { in.ramp.set_target(in.enabled && !in.muted ? in.gain : 0.0); }

void StationMixer::set_gain(int input, double linear) {
  if (!valid(input)) return;
  inputs_[input].gain = linear;
  retarget(inputs_[input]);
}

void StationMixer::set_muted(int input, bool muted) {
  if (!valid(input)) return;
  inputs_[input].muted = muted;
  retarget(inputs_[input]);
}

void StationMixer::set_enabled(int input, bool enabled) {
  if (!valid(input)) return;
  inputs_[input].enabled = enabled;
  retarget(inputs_[input]);
}

bool StationMixer::audible(int input) const {
  if (!valid(input)) return false;
  const GainRamp& ramp = inputs_[input].ramp;
  return ramp.level() != 0.0 || ramp.ramping();
}

void StationMixer::hold_peak(std::atomic<float>& peak, float value) {
  float seen = peak.load(std::memory_order_relaxed);
  while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

void StationMixer::mix(const float* const inputs[kStationInputs], size_t frames, float* out) {
  std::fill(out, out + frames * 2, 0.0f);
  for (int i = 0; i < kStationInputs; ++i) {
    Input& in = inputs_[i];
    const float* src = inputs[i];
    if (!src) {  // silence: only the ramp moves on
      in.ramp.skip(frames);
      continue;
    }
    float peak_l = 0.0f, peak_r = 0.0f;
    for (size_t f = 0; f < frames; ++f) {
      const float l = src[f * 2], r = src[f * 2 + 1];
      peak_l = std::max(peak_l, std::abs(l));
      peak_r = std::max(peak_r, std::abs(r));
      const double level = in.ramp.next();
      if (level == 0.0) continue;
      const float g = static_cast<float>(level);
      out[f * 2] += l * g;
      out[f * 2 + 1] += r * g;
    }
    hold_peak(input_peaks_[i * 2], peak_l);
    hold_peak(input_peaks_[i * 2 + 1], peak_r);
  }
  float peak_l = 0.0f, peak_r = 0.0f;
  for (size_t f = 0; f < frames; ++f) {
    peak_l = std::max(peak_l, std::abs(out[f * 2]));
    peak_r = std::max(peak_r, std::abs(out[f * 2 + 1]));
  }
  hold_peak(mix_peaks_[0], peak_l);
  hold_peak(mix_peaks_[1], peak_r);
}

StationMixer::Peaks StationMixer::take_peaks() {
  Peaks p;
  for (int i = 0; i < kStationInputs; ++i)
    for (int s = 0; s < 2; ++s) p.input[i][s] = input_peaks_[i * 2 + s].exchange(0.0f, std::memory_order_relaxed);
  for (int s = 0; s < 2; ++s) p.mix[s] = mix_peaks_[s].exchange(0.0f, std::memory_order_relaxed);
  return p;
}

}  // namespace bam::encoder
