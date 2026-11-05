// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder signal path (spec §4.2-4.4): delay line, station mixer, stream gain and
// resampling.
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

#include "bam/encoder/dsp.hpp"

namespace be = bam::encoder;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kBlock = 480;              // the station thread's 10 ms pass
constexpr double kLsb16 = 1.0 / 32768.0;    // one LSB of the 16-bit stream PCM

// Deterministic noise addressed by frame index, so a delayed frame can be checked exactly
// without keeping the input.
float noise(int64_t frame, int side) {
  uint64_t x = static_cast<uint64_t>(frame) * 2 + static_cast<uint64_t>(side) + 0x9E3779B97F4A7C15ull;
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return static_cast<float>(static_cast<double>(x >> 40) / static_cast<double>(1ull << 23) - 1.0);  // [-1, 1)
}

// A full-scale sine so slow that consecutive samples differ by less than one 16-bit LSB.
float slow_sine(int64_t frame, int side) {
  return static_cast<float>(std::sin(2 * kPi * 0.1 * static_cast<double>(frame) / 48000.0 + side * kPi / 2));
}

using Signal = float (*)(int64_t, int);

// Runs `line` for `frames` frames in kBlock passes (write a pass, read it back), appending the
// output to `out`; `pos` is the absolute input frame index, advanced.
void run(be::DelayLine& line, Signal sig, int64_t& pos, size_t frames, std::vector<float>& out) {
  std::vector<float> in(kBlock * 2), block(kBlock * 2);
  for (size_t done = 0; done < frames; done += kBlock) {
    const size_t n = std::min(kBlock, frames - done);
    for (size_t i = 0; i < n; ++i) {
      in[i * 2] = sig(pos + static_cast<int64_t>(i), 0);
      in[i * 2 + 1] = sig(pos + static_cast<int64_t>(i), 1);
    }
    line.write(in.data(), n);
    line.read(block.data(), n);
    out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n * 2));
    pos += static_cast<int64_t>(n);
  }
}

// Output frames [from, to) equal the input delayed by `delay` frames, bit for bit.
void check_aligned(const std::vector<float>& out, Signal sig, int64_t from, int64_t to, int64_t delay) {
  int64_t bad = 0, first_bad = -1;
  for (int64_t n = from; n < to; ++n) {
    for (int s = 0; s < 2; ++s) {
      const float want = n - delay < 0 ? 0.0f : sig(n - delay, s);
      if (out[static_cast<size_t>(n * 2 + s)] != want) {
        if (first_bad < 0) first_bad = n;
        ++bad;
      }
    }
  }
  INFO("first mismatch at frame " << first_bad);
  CHECK(bad == 0);
}

// Largest frame-to-frame step on either side from frame `from` on (the signal's own start,
// out of the initial silence, is not the delay line's doing).
double max_step(const std::vector<float>& out, int64_t from) {
  double worst = 0;
  for (size_t i = static_cast<size_t>(from) * 2 + 2; i < out.size(); ++i)
    worst = std::max(worst, std::abs(static_cast<double>(out[i]) - out[i - 2]));
  return worst;
}

}  // namespace

TEST_CASE("delay line delays exactly to the sample") {
  for (double seconds : {0.0, 0.1, 2.5, 60.0}) {
    CAPTURE(seconds);
    be::DelayLine line;
    REQUIRE(line.set_delay_seconds(seconds));
    const int64_t delay = std::llround(seconds * 48000);
    CHECK(line.delay_frames() == delay);
    std::vector<float> out;
    int64_t pos = 0;
    const size_t frames = static_cast<size_t>(delay) + 48000;
    out.reserve(frames * 2);
    run(line, noise, pos, frames, out);
    check_aligned(out, noise, 0, static_cast<int64_t>(frames), delay);
  }
}

TEST_CASE("delay line keeps the delay within its capacity") {
  be::DelayLine line;
  CHECK(line.capacity_frames() == 60.5 * 48000);
  CHECK(line.max_delay_frames() == 60 * 48000);
  CHECK(line.set_delay_seconds(60.0));
  CHECK_FALSE(line.set_delay_seconds(60.1));
  CHECK_FALSE(line.set_delay_seconds(-0.1));
  CHECK(line.delay_frames() == 60 * 48000);

  be::DelayLine small(2.0);
  CHECK(small.max_delay_frames() == 1.5 * 48000);
  CHECK(small.set_delay_seconds(1.5));
  CHECK_FALSE(small.set_delay_seconds(1.6));
}

TEST_CASE("delay line change 1 s -> 3 s mid-stream fades, inserts silence, then aligns exactly") {
  for (Signal sig : {slow_sine, noise}) {
    be::DelayLine line;
    REQUIRE(line.set_delay_seconds(1.0));
    std::vector<float> out;
    int64_t pos = 0;
    run(line, sig, pos, 5 * 48000, out);
    const int64_t change = pos;
    REQUIRE(line.set_delay_seconds(3.0));
    CHECK(line.delay_frames() == 3 * 48000);
    run(line, sig, pos, 8 * 48000, out);

    check_aligned(out, sig, 48000, change, 48000);  // before the change
    // fade-out over 240 frames, 2 s of silence, fade-in over 240 frames, then exactly 3 s
    const int64_t silence_from = change + be::kRampFrames;
    const int64_t silence_to = silence_from + 2 * 48000;
    int64_t loud = 0;
    for (int64_t n = silence_from; n < silence_to; ++n) loud += out[n * 2] != 0.0f || out[n * 2 + 1] != 0.0f;
    CHECK(loud == 0);
    check_aligned(out, sig, silence_to + be::kRampFrames, pos, 3 * 48000);
    if (sig == slow_sine) CHECK(max_step(out, change - 1) <= 1.0 / be::kRampFrames + kLsb16);
  }
}

TEST_CASE("delay line change 3 s -> 1 s skips audio with a fade, then aligns exactly") {
  for (Signal sig : {slow_sine, noise}) {
    be::DelayLine line;
    REQUIRE(line.set_delay_seconds(3.0));
    std::vector<float> out;
    int64_t pos = 0;
    run(line, sig, pos, 5 * 48000, out);
    const int64_t change = pos;
    REQUIRE(line.set_delay_seconds(1.0));
    run(line, sig, pos, 3 * 48000, out);
    check_aligned(out, sig, 3 * 48000, change, 3 * 48000);
    check_aligned(out, sig, change + 2 * be::kRampFrames, pos, 48000);
    if (sig == slow_sine) CHECK(max_step(out, change - 1) <= 1.0 / be::kRampFrames + kLsb16);
  }
}

TEST_CASE("delay line change during a fade keeps the level smooth and lands on the last delay") {
  be::DelayLine line;
  REQUIRE(line.set_delay_seconds(1.0));
  std::vector<float> out;
  int64_t pos = 0;
  run(line, slow_sine, pos, 2 * 48000, out);
  const int64_t change = pos;
  REQUIRE(line.set_delay_seconds(2.0));
  run(line, slow_sine, pos, 100, out);  // mid fade-out
  REQUIRE(line.set_delay_seconds(0.5));
  run(line, slow_sine, pos, 100 + be::kRampFrames + 100, out);  // into the fade-in
  REQUIRE(line.set_delay_seconds(1.5));
  run(line, slow_sine, pos, 4 * 48000, out);
  CHECK(max_step(out, change - 1) <= 1.0 / be::kRampFrames + kLsb16);
  check_aligned(out, slow_sine, pos - 48000, pos, 72000);
}

TEST_CASE("station mixer sums inputs with their gains") {
  be::StationMixer mixer;
  const double gains[3] = {1.0, 0.5, 0.25};
  for (int i = 0; i < 3; ++i) {
    mixer.set_gain(i, gains[i]);
    mixer.set_enabled(i, true);
  }
  std::vector<float> in[3], out(kBlock * 2);
  for (int i = 0; i < 3; ++i) {
    in[i].resize(kBlock * 2);
    for (size_t f = 0; f < kBlock; ++f)
      for (int s = 0; s < 2; ++s) in[i][f * 2 + s] = noise(static_cast<int64_t>(f) + i * 100000, s) * 0.5f;
  }
  const float* ptrs[3] = {in[0].data(), in[1].data(), in[2].data()};
  mixer.mix(ptrs, kBlock, out.data());  // the joins fade in over the first 240 frames
  mixer.mix(ptrs, kBlock, out.data());
  double worst = 0;
  for (size_t k = 0; k < kBlock * 2; ++k) {
    const double want = in[0][k] * gains[0] + in[1][k] * gains[1] + in[2][k] * gains[2];
    worst = std::max(worst, std::abs(out[k] - want));
  }
  CHECK(worst < 1e-6);

  // a missing block is silence for that input
  const float* only_first[3] = {in[0].data(), nullptr, nullptr};
  mixer.mix(only_first, kBlock, out.data());
  CHECK(std::equal(out.begin(), out.end(), in[0].begin()));
}

TEST_CASE("station mixer passes a single 0 dB input bit for bit") {
  be::StationMixer mixer;
  mixer.set_enabled(1, true);
  std::vector<float> in(kBlock * 2), out(kBlock * 2);
  for (size_t k = 0; k < in.size(); ++k) in[k] = noise(static_cast<int64_t>(k / 2), static_cast<int>(k % 2));
  const float* ptrs[3] = {nullptr, in.data(), nullptr};
  mixer.mix(ptrs, kBlock, out.data());
  mixer.mix(ptrs, kBlock, out.data());
  CHECK(std::equal(out.begin(), out.end(), in.begin()));
}

TEST_CASE("station mixer ramps every level change over 240 frames") {
  be::StationMixer mixer;
  std::vector<float> dc(kBlock * 2, 1.0f), out(kBlock * 2);
  const float* ptrs[3] = {dc.data(), nullptr, nullptr};

  // checks one block: from `from` to `to` in equal steps over 240 frames, then flat at `to`
  auto check_ramp = [&](double from, double to) {
    mixer.mix(ptrs, kBlock, out.data());
    const double step = std::abs(to - from) / be::kRampFrames;
    double prev = from, worst_step = 0;
    int wrong = 0;  // frames off the ramp: both sides equal, at `to` only from frame 240 on
    for (size_t f = 0; f < kBlock; ++f) {
      worst_step = std::max(worst_step, std::abs(out[f * 2] - prev));
      prev = out[f * 2];
      const bool at_target = out[f * 2] == static_cast<float>(to);
      wrong += out[f * 2] != out[f * 2 + 1] || at_target != (f + 1 >= be::kRampFrames);
    }
    CHECK(wrong == 0);
    CHECK(worst_step <= step + 1e-6);
  };

  CHECK_FALSE(mixer.audible(0));
  mixer.set_enabled(0, true);  // join
  CHECK(mixer.audible(0));
  check_ramp(0.0, 1.0);
  mixer.set_gain(0, 0.5);  // gain change
  check_ramp(1.0, 0.5);
  mixer.set_muted(0, true);  // mute
  check_ramp(0.5, 0.0);
  CHECK_FALSE(mixer.audible(0));
  mixer.set_muted(0, false);  // unmute
  check_ramp(0.0, 0.5);
  mixer.set_enabled(0, false);  // leave
  CHECK(mixer.audible(0));
  check_ramp(0.5, 0.0);
  CHECK_FALSE(mixer.audible(0));
}

TEST_CASE("station mixer reports peaks per input and for the mix") {
  be::StationMixer mixer;
  mixer.set_enabled(0, true);
  mixer.set_enabled(1, true);
  mixer.set_gain(1, 0.5);
  mixer.set_muted(2, true);
  std::vector<float> a(kBlock * 2, 0.0f), b(kBlock * 2, 0.0f), c(kBlock * 2, 0.0f), out(kBlock * 2);
  a[300 * 2] = 0.25f;       // left
  b[300 * 2 + 1] = -0.8f;   // right
  c[10 * 2] = 0.9f;         // muted: metered, not mixed
  const float* ptrs[3] = {a.data(), b.data(), c.data()};
  mixer.mix(ptrs, kBlock, out.data());
  mixer.mix(ptrs, kBlock, out.data());

  const be::StationMixer::Peaks p = mixer.take_peaks();
  CHECK(p.input[0][0] == 0.25f);
  CHECK(p.input[0][1] == 0.0f);
  CHECK(p.input[1][0] == 0.0f);
  CHECK(p.input[1][1] == 0.8f);
  CHECK(p.input[2][0] == 0.9f);
  CHECK(p.mix[0] == 0.25f);
  CHECK(p.mix[1] == 0.4f);

  const be::StationMixer::Peaks cleared = mixer.take_peaks();
  CHECK(cleared.input[0][0] == 0.0f);
  CHECK(cleared.mix[1] == 0.0f);
}

TEST_CASE("stream conditioner keeps a 1 kHz sine's level at every stream rate") {
  for (uint32_t rate : {48000u, 44100u, 32000u, 24000u, 22050u}) {
    CAPTURE(rate);
    be::StreamConditioner cond(rate);
    CHECK(cond.out_rate() == rate);
    const double ratio = rate / 48000.0;
    constexpr double kAmp = 0.5;
    std::vector<float> in, out;
    size_t in_total = 0, out_total = 0;
    double worst_len_error = 0;
    std::vector<float> collected;
    // irregular block sizes, as the stream worker drains its queue
    const size_t sizes[] = {480, 480, 1, 7, 480, 2000, 333, 480, 4800};
    for (int b = 0; in_total < 3 * 48000; ++b) {
      const size_t n = sizes[b % std::size(sizes)];
      in.resize(n * 2);
      for (size_t i = 0; i < n; ++i)
        in[i * 2] = in[i * 2 + 1] = static_cast<float>(kAmp * std::sin(2 * kPi * 1000.0 * static_cast<double>(in_total + i) / 48000.0));
      out.resize(cond.max_out_frames(n) * 2);
      const size_t got = cond.process(in.data(), n, out.data(), cond.max_out_frames(n));
      in_total += n;
      out_total += got;
      worst_len_error = std::max(worst_len_error, std::abs(static_cast<double>(out_total) - in_total * ratio));
      collected.insert(collected.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(got * 2));
    }
    CHECK(worst_len_error <= 2.0);

    double sum = 0;
    size_t counted = 0;
    for (size_t f = rate / 10; f < out_total; ++f) {  // past the resampler's start-up
      sum += collected[f * 2] * collected[f * 2] + collected[f * 2 + 1] * collected[f * 2 + 1];
      counted += 2;
    }
    const double rms = std::sqrt(sum / static_cast<double>(counted));
    const double error_db = 20 * std::log10(rms / (kAmp / std::sqrt(2.0)));
    CAPTURE(error_db);
    CHECK(std::abs(error_db) <= 0.1);
  }
}

TEST_CASE("stream conditioner at 48 kHz and 0 dB passes samples bit for bit") {
  be::StreamConditioner cond(48000);
  std::vector<float> in(5000 * 2), out(cond.max_out_frames(5000) * 2);
  for (size_t k = 0; k < in.size(); ++k) in[k] = noise(static_cast<int64_t>(k / 2), static_cast<int>(k % 2)) * 1.5f;
  REQUIRE(cond.process(in.data(), 5000, out.data(), cond.max_out_frames(5000)) == 5000);
  CHECK(std::equal(in.begin(), in.end(), out.begin()));  // no clipping here either
}

TEST_CASE("stream conditioner ramps a gain change over 240 frames") {
  be::StreamConditioner cond(48000, 0.5);
  std::vector<float> dc(kBlock * 2, 1.0f), out(kBlock * 2);
  REQUIRE(cond.process(dc.data(), kBlock, out.data(), kBlock) == kBlock);
  CHECK(out[0] == 0.5f);  // the initial gain applies at once
  cond.set_gain(0.25);
  REQUIRE(cond.process(dc.data(), kBlock, out.data(), kBlock) == kBlock);
  double prev = 0.5, worst_step = 0;
  for (size_t f = 0; f < kBlock; ++f) {
    worst_step = std::max(worst_step, std::abs(out[f * 2] - prev));
    prev = out[f * 2];
  }
  CHECK(worst_step <= 0.25 / be::kRampFrames + 1e-6);
  CHECK(out[(be::kRampFrames - 2) * 2] != 0.25f);
  CHECK(out[(be::kRampFrames - 1) * 2] == 0.25f);
  CHECK(out[(kBlock - 1) * 2 + 1] == 0.25f);
}

#endif  // BAM_WITH_CODECS
