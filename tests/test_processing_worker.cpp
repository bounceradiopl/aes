// SPDX-License-Identifier: GPL-3.0-or-later
// The per-station Stereo Tool worker (encoder/src/processing_worker.cpp) against the fake library
// of tests/fake_stereotool (output = input delayed by `latency` frames × `gain`, failure modes).
// The worker's output is read through ProcessingSettings::output_tap; the input is pushed in
// blocks of 10 ms like the station thread does, faster than real time unless a test needs real
// time.
#include <doctest/doctest.h>

#ifdef BAM_FAKE_STEREOTOOL_PATH

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../encoder/src/engine_internal.hpp"
#include "../encoder/src/stereotool_api.hpp"  // the web interface parameter indices
#include "bam/encoder/stereotool.hpp"
#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "fake_stereotool/fake_control.hpp"

// Allocation counter for the "no allocation per block" test: every operator new of this test
// binary counts on the calling thread (a thread_local of constant initialisation: nothing runs
// to set it up). The replacement forwards to malloc/free and changes nothing else.
namespace {
thread_local long long t_new_calls = 0;
}  // namespace

// GCC inlines these and then sees free() of a pointer from operator new: the pair is this one.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t size) {
  ++t_new_calls;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  ++t_new_calls;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace be = bam::encoder;
namespace bd = bam::encoder::detail;
namespace bp = bam::platform;
namespace fs = std::filesystem;

namespace {

using fake_st::fake;
using fake_st::UseFake;
using PW = bd::ProcessingWorker;
constexpr uint32_t kRamp = be::kRampFrames;
const std::string kUnlicensed = "Fake: Declipper, Fake: Multiband";

// The worker's output, as the streams would get it.
struct Collector {
  std::mutex m;
  std::vector<float> out;
  explicit Collector(size_t max_frames = 48000 * 20) { out.reserve(max_frames * 2); }
  std::function<void(const float*, size_t)> tap() {
    return [this](const float* s, size_t n) {
      std::lock_guard lock(m);
      if (out.size() + n * 2 <= out.capacity()) out.insert(out.end(), s, s + n * 2);  // never reallocates
    };
  }
  size_t frames() {
    std::lock_guard lock(m);
    return out.size() / 2;
  }
  std::vector<float> copy() {
    std::lock_guard lock(m);
    return out;
  }
};

// The input: a 997 Hz sine (never a silent 10 ms) with an offset, a different phase per side.
struct Input {
  std::vector<float> in;
  explicit Input(size_t max_frames = 48000 * 20) { in.reserve(max_frames * 2); }
  float dc = -1;  // >= 0: a constant signal instead of the sine
  size_t frames() const { return in.size() / 2; }
  void extend(size_t frames) {
    for (size_t k = 0; k < frames; ++k) {
      const size_t j = frames_now();
      for (int c = 0; c < 2; ++c) {
        const double ph = 2.0 * 3.14159265358979 * 997.0 * double(j) / 48000.0 + c;
        in.push_back(dc >= 0 ? dc : float(0.4 * std::sin(ph) + 0.05));
      }
    }
  }
  size_t frames_now() const { return in.size() / 2; }
};

// Pushes `frames` more of the input in 10 ms blocks, one every `ns_per_block` (10 ms: real time).
void feed(PW& w, Input& input, size_t frames, int64_t ns_per_block = 2'000'000, size_t block = 480) {
  int64_t next = bp::monotonic_ns();
  for (size_t done = 0; done < frames;) {
    const size_t n = std::min(block, frames - done);
    const size_t from = input.frames();
    input.extend(n);
    w.push(input.in.data() + from * 2, n);
    done += n;
    next += ns_per_block;
    bp::sleep_until_ns(next);
  }
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 5000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

// Feeds until the worker reaches `state` (at most `max_frames`); true when it did.
bool feed_until(PW& w, Input& input, be::ProcessingState state, int64_t ns_per_block = 2'000'000,
                size_t max_frames = 48000 * 4) {
  for (size_t fed = 0; fed < max_frames; fed += 4800) {
    if (w.status().state == state) return true;
    feed(w, input, 4800, ns_per_block);
  }
  return w.status().state == state;
}

// The frames after the input has all come out: everything but the worker's delay.
bool drained(PW& w, Collector& col, const Input& input) {
  return wait_until([&] { return col.frames() + w.delay_frames() == input.frames(); });
}

// Walks the output against the input: each frame is the input (dry), the input × gain (the fake
// Stereo Tool), or a frame of a kRampFrames crossfade between the two, computed exactly as the
// worker does (GainRamp levels, mixed in double). Counts the frames that are none of these.
struct Path {
  size_t mismatches = 0;
  size_t first_mismatch = SIZE_MAX;
  std::vector<std::pair<size_t, bool>> switches;  // frame where a crossfade starts, true = to processed
};

// `gains`: the fake's gain in each stretch of processing, in order (the last one stays).
Path analyse(const std::vector<float>& in, const std::vector<float>& out, const std::vector<float>& gains,
             bool start_processed = false) {
  Path p;
  bool processed = start_processed;
  size_t episode = start_processed ? 0 : size_t(-1);
  float gain = gains.front();
  const size_t frames = std::min(in.size(), out.size()) / 2;
  auto proc = [&](size_t j, int c) { return in[j * 2 + c] * gain; };
  auto dry = [&](size_t j, int c) { return in[j * 2 + c]; };
  auto ramp = [&](size_t j, int c, uint32_t k, bool to_processed) {  // k = 1..kRamp
    const double from = to_processed ? 0.0 : 1.0, target = to_processed ? 1.0 : 0.0;
    const uint32_t left = kRamp - k;
    const double a = left == 0 ? target : target + (from - target) * (static_cast<double>(left) / kRamp);
    return static_cast<float>(a * double(proc(j, c)) + (1.0 - a) * double(dry(j, c)));
  };
  auto bad = [&](size_t j) {
    ++p.mismatches;
    if (p.first_mismatch == SIZE_MAX) p.first_mismatch = j;
  };
  for (size_t j = 0; j < frames;) {
    auto is = [&](size_t f, auto&& value) { return out[f * 2] == value(f, 0) && out[f * 2 + 1] == value(f, 1); };
    if (processed ? is(j, proc) : is(j, dry)) {
      ++j;
      continue;
    }
    const bool to = !processed;
    if (to) gain = gains[std::min(++episode, gains.size() - 1)];
    if (is(j, [&](size_t f, int c) { return ramp(f, c, 1, to); })) {
      p.switches.push_back({j, to});
      for (uint32_t k = 1; k <= kRamp && j < frames; ++k, ++j)
        if (!is(j, [&](size_t f, int c) { return ramp(f, c, k, to); })) bad(j);
      processed = to;
      continue;
    }
    if (to) --episode;  // not a switch after all
    bad(j);
    ++j;
  }
  return p;
}

Path analyse(const std::vector<float>& in, const std::vector<float>& out, float gain, bool start_processed = false) {
  return analyse(in, out, std::vector<float>{gain}, start_processed);
}

// No 10 ms of the output is silent (the input never is).
size_t silent_blocks(const std::vector<float>& out, float floor = 0.1f) {
  size_t silent = 0;
  for (size_t b = 0; b + 960 <= out.size(); b += 960) {
    float peak = 0;
    for (size_t i = b; i < b + 960; ++i) peak = std::max(peak, std::abs(out[i]));
    if (peak < floor) ++silent;
  }
  return silent;
}

be::ProcessingStatus status_when(PW& w, const std::function<bool(const be::ProcessingStatus&)>& pred,
                                 int timeout_ms = 5000) {
  be::ProcessingStatus s;
  wait_until([&] { return pred(s = w.status()); }, timeout_ms);
  return s;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// The worker's events as "state: reason" lines, oldest first.
std::vector<std::string> event_lines(const bd::ProcessingWorker& w) {
  std::vector<bd::ProcessingEvent> ev;
  const uint64_t seq = w.events_since(0, &ev);
  std::vector<std::string> lines;
  uint64_t expect = ev.empty() ? seq : ev.front().seq;
  for (const auto& e : ev) {
    CHECK(e.seq == expect++);
    lines.push_back(std::string(be::processing_state_name(e.state)) + ": " + e.reason);
  }
  CHECK(expect == (ev.empty() ? seq : seq + 1));  // nothing missing up to the returned seq
  return lines;
}

struct TempDir {
  std::string path;
  explicit TempDir(const std::string& tag) {
    path = (fs::temp_directory_path() / ("bam_pw_" + tag + "_" + std::to_string(bp::current_pid()))).string();
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

struct StopTimeout {
  explicit StopTimeout(int ms) { bd::testing::set_processing_stop_timeout_ms(ms); }
  ~StopTimeout() { bd::testing::set_processing_stop_timeout_ms(0); }
  StopTimeout(const StopTimeout&) = delete;
  StopTimeout& operator=(const StopTimeout&) = delete;
};

// The fake's time per Process call of one block with the current options, as this runner
// manages it: the mean of 30 calls through the binding, in ms.
double fake_block_ms() {
  std::string err;
  auto st = be::StereoToolInstance::create("", "calibration", "", &err);
  REQUIRE_MESSAGE(st, err);
  std::vector<float> block(PW::kBlockFrames * 2, 0.0f);
  constexpr int kCalls = 30;
  const int64_t t0 = bp::monotonic_ns();
  for (int i = 0; i < kCalls; ++i) REQUIRE(st->process(block.data(), int32_t(PW::kBlockFrames)));
  return double(bp::monotonic_ns() - t0) / 1e6 / kCalls;
}

// The tests with a load (process_work_us) are about the worker's rules for a Stereo Tool with a
// known headroom: a call of work_ms for each block, which arrives every budget_ms. On a runner short
// of CPU the fake's thread loses the CPU while it works, and the headroom shrinks or goes. Most of
// it gone means the case was not run: the test then stops with a message instead of a verdict.
// "Most" is more than half: then the calls took longer than work_ms + (budget_ms - work_ms) / 2.
double headroom_limit_ms(double work_ms, double budget_ms) { return work_ms + (budget_ms - work_ms) / 2.0; }

// Samples the fake's calls and their wall time every 20 ms on a thread of its own, from its
// construction to stop().
class CallSampler {
 public:
  CallSampler() {
    samples_.reserve(4000);
    thread_ = std::thread([this] {
      while (on_.load() && samples_.size() < samples_.capacity()) {
        samples_.push_back({bp::monotonic_ns(), fake().value("process_calls"), fake().value("process_busy_us")});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }
  ~CallSampler() { stop(); }
  CallSampler(const CallSampler&) = delete;
  CallSampler& operator=(const CallSampler&) = delete;
  void stop() {
    on_.store(false);
    if (thread_.joinable()) thread_.join();
  }
  // The longest mean time of a call over any 0.5 s with at least 10 calls, in ms (stops first).
  double slowest_ms() {
    stop();
    double slowest = 0;
    for (size_t i = 0, j = 0; i < samples_.size(); ++i) {
      while (j < samples_.size() && samples_[j].ns - samples_[i].ns < 500'000'000) ++j;
      if (j == samples_.size()) break;
      const long long calls = samples_[j].calls - samples_[i].calls;
      if (calls >= 10)
        slowest = std::max(slowest, double(samples_[j].busy_us - samples_[i].busy_us) / 1000.0 / double(calls));
    }
    return slowest;
  }

 private:
  struct Sample {
    int64_t ns;
    long long calls, busy_us;
  };
  std::vector<Sample> samples_;
  std::atomic<bool> on_{true};
  std::thread thread_;
};

// Why a bypass of the fake with process_work_us is the runner's doing, "" when it is not: one call
// took more than the worker's 200 ms (stuck), or over some 0.5 s the calls lost more than half of
// the headroom (headroom_limit_ms). Draining a backlog keeps the fake busy all the time, but each
// call still takes about work_ms: no excuse, so a bypass while it drains still fails.
std::string runner_excuse(CallSampler& calls, double work_ms, double budget_ms) {
  const double slowest = calls.slowest_ms();
  const double longest = double(fake().value("process_busy_max_us")) / 1000.0;
  std::ostringstream why;
  if (longest >= 200.0)
    why << "one call of Stereo Tool took " << longest << " ms on this runner";
  else if (slowest > headroom_limit_ms(work_ms, budget_ms))
    why << "calls of Stereo Tool took " << slowest << " ms on average over 0.5 s on this runner (" << work_ms
        << " ms of work per " << budget_ms << " ms: most of the headroom lost)";
  return why.str();
}

bd::ProcessingSettings settings(Collector& col) {
  bd::ProcessingSettings s;
  s.output_tap = col.tap();
  return s;
}

}  // namespace

TEST_CASE("processing: the output is the input through Stereo Tool, aligned with the bypass, bit-exact") {
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 512);
  Collector col;
  Input input;
  PW w(settings(col), "s1");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 48000);
  REQUIRE(drained(w, col, input));
  const auto st = w.status();
  CHECK(st.state == be::ProcessingState::Running);
  CHECK(st.reason.empty());
  CHECK(st.license_valid);
  CHECK(st.unlicensed_features.empty());
  // both paths are delayed by the Stereo Tool latency plus the crossfade margin
  CHECK(w.delay_frames() == 512 + kRamp);
  CHECK(st.latency_ms == doctest::Approx((512.0 + kRamp) / 48.0));
  const auto out = col.copy();
  const Path p = analyse(input.in, out, 0.5f);
  CHECK_MESSAGE(p.mismatches == 0, "first mismatch at frame " << p.first_mismatch);
  REQUIRE(p.switches.size() == 1);  // unprocessed until the licence check, then processed for good
  CHECK(p.switches[0].second);
  // not before the licence check: it comes with the block that completes kLicenceFirstCheckFrames
  // of the session, and the crossfade starts where the output stood before that block
  CHECK(p.switches[0].first + PW::kBlockFrames + w.delay_frames() >= PW::kLicenceFirstCheckFrames);
  CHECK(silent_blocks(out) == 0);
  CHECK(fake().value("process_channels") == 2);
  CHECK(fake().value("process_rate") == 48000);
  CHECK(fake().value("latency_calls") == 1);  // measured once, before any audio
  w.stop();
  CHECK(w.status().state == be::ProcessingState::Off);
  CHECK(fake().value("live") == 0);  // the instance is deleted on stop
}

TEST_CASE("processing: switching processed <-> bypass crossfades over 240 frames, never a step above the ramp") {
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 300);
  Collector col;
  Input input;
  input.dc = 0.5f;  // processed 0.25, unprocessed 0.5: every step is the crossfade's
  PW w(settings(col), "s2");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 9600);
  w.restart();  // a working instance is kept: out of processing and back in
  REQUIRE(status_when(w, [](auto& s) { return s.state != be::ProcessingState::Running; }).state ==
          be::ProcessingState::Bypass);
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 9600);
  fake().set("process_throws", 1);  // and out of processing on a failure
  feed(w, input, 9600);
  CHECK(status_when(w, [](auto& s) { return s.state == be::ProcessingState::Bypass; }).state ==
        be::ProcessingState::Bypass);
  feed(w, input, 9600);
  REQUIRE(drained(w, col, input));
  const auto out = col.copy();
  const Path p = analyse(input.in, out, 0.5f);
  CHECK(p.mismatches == 0);
  REQUIRE(p.switches.size() == 4);
  CHECK(p.switches[0].second);
  CHECK_FALSE(p.switches[1].second);
  CHECK(p.switches[2].second);
  CHECK_FALSE(p.switches[3].second);
  // the largest step between two frames is one crossfade step: (0.5 - 0.25) / 240
  float step = 0;
  size_t between = 0;
  for (size_t i = 2; i < out.size(); ++i) step = std::max(step, std::abs(out[i] - out[i - 2]));
  for (size_t i = 0; i < out.size(); i += 2)
    if (out[i] > 0.25f && out[i] < 0.5f) ++between;
  CHECK(step <= 0.25f / kRamp + 1e-6f);
  CHECK(between == 4 * (kRamp - 1));  // each switch is kRamp frames, the last one at the new level
  CHECK(fake().value("created") == 1);
}

TEST_CASE("processing: without the library or an instance the input passes unchanged, state error") {
  SUBCASE("no library") {
    UseFake use;
    TempDir empty("nolib");
    be::set_stereotool_library_dirs({empty.path});
    Collector col;
    Input input;
    PW w(settings(col), "s3");
    const auto st = status_when(w, [](auto& s) { return s.state == be::ProcessingState::Error; });
    CHECK(st.state == be::ProcessingState::Error);
    CHECK_MESSAGE(contains(st.reason, "brak biblioteki Stereo Tool"), st.reason);
    feed(w, input, 48000);
    REQUIRE(drained(w, col, input));
    CHECK(w.delay_frames() == 0);  // no Stereo Tool latency to align with
    CHECK(col.copy() == input.in);
    CHECK(w.status().cpu_pct == 0);
    CHECK_FALSE(w.status().license_valid);
  }
  SUBCASE("the instance cannot be created") {
    UseFake use;
    fake().set("create_fails", 1);
    Collector col;
    Input input;
    PW w(settings(col), "s3");
    feed(w, input, 24000);
    const auto st = status_when(w, [](auto& s) { return s.state == be::ProcessingState::Error; });
    CHECK(st.state == be::ProcessingState::Error);
    CHECK_MESSAGE(contains(st.reason, "nie udało się utworzyć instancji Stereo Tool"), st.reason);
    feed(w, input, 24000);
    REQUIRE(drained(w, col, input));
    CHECK(col.copy() == input.in);
    CHECK(fake().value("process_calls") == 0);
  }
  SUBCASE("the preset cannot be loaded") {
    UseFake use;
    Collector col;
    Input input;
    auto s = settings(col);
    s.preset_path = (fs::temp_directory_path() / "bam_pw_missing_preset.sts").string();
    PW w(s, "s3");
    const auto st = status_when(w, [](auto& x) { return x.state == be::ProcessingState::Error; });
    CHECK_MESSAGE(contains(st.reason, "nie wczytał presetu"), st.reason);
    feed(w, input, 24000);
    REQUIRE(drained(w, col, input));
    CHECK(col.copy() == input.in);
    CHECK(fake().value("live") == 0);  // not kept for nothing (~600 MB with the real library)
  }
}

TEST_CASE("processing: an invalid licence bypasses, aligned, and names the unlicensed features") {
  SUBCASE("invalid from the start: Stereo Tool is never heard") {
    UseFake use;
    fake().set("gain", 0.5);
    fake().set("latency", 512);
    fake().set("license_valid", 0);
    Collector col;
    Input input;
    auto named = settings(col);
    named.station_name = "Radio Przed";
    PW w(named, "s4");
    w.set_station_name("Radio Licencja");  // renamed: the worker's log lines follow
    feed(w, input, 48000);
    const auto st = status_when(w, [](auto& s) { return !s.unlicensed_features.empty(); });
    // logged before the features are published
    bool logged = false;
    for (const auto& e : bam::Logger::instance().recent())
      logged = logged || e.message == "Processing stacji „Radio Licencja”: licencja Stereo Tool nie obejmuje funkcji: " + kUnlicensed;
    CHECK(logged);
    CHECK(st.state == be::ProcessingState::Bypass);
    CHECK_MESSAGE(contains(st.reason, "licencja"), st.reason);
    CHECK_FALSE(st.license_valid);
    CHECK(st.unlicensed_features == kUnlicensed);
    feed(w, input, 24000);
    REQUIRE(drained(w, col, input));
    CHECK(w.delay_frames() == 512 + kRamp);  // the bypass keeps the Stereo Tool latency
    const auto out = col.copy();
    REQUIRE(out.size() == input.in.size() - size_t(512 + kRamp) * 2);
    CHECK(std::equal(out.begin(), out.end(), input.in.begin()));  // never a processed frame
    const long long calls = fake().value("process_calls");
    feed(w, input, 9600);
    CHECK(fake().value("process_calls") == calls);  // not fed any more
  }
  SUBCASE("invalid later (e.g. a feature switched on in the web interface): a crossfade to the bypass") {
    UseFake use;
    fake().set("gain", 0.5);
    fake().set("latency", 512);
    Collector col;
    Input input;
    PW w(settings(col), "s4");
    REQUIRE(feed_until(w, input, be::ProcessingState::Running));
    fake().set("license_valid", 0);
    feed(w, input, 2 * PW::kLicenceCheckFrames);
    const auto st = w.status();
    CHECK(st.state == be::ProcessingState::Bypass);
    CHECK(st.unlicensed_features == kUnlicensed);
    REQUIRE(drained(w, col, input));
    const auto out = col.copy();
    const Path p = analyse(input.in, out, 0.5f);
    CHECK(p.mismatches == 0);
    REQUIRE(p.switches.size() == 2);
    CHECK_FALSE(p.switches[1].second);
    CHECK(silent_blocks(out) == 0);
  }
}

TEST_CASE("processing: an exception in process bypasses with a crossfade, restart builds a new instance") {
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 256);
  Collector col;
  Input input;
  PW w(settings(col), "s5");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  fake().set("process_throws", 1);
  feed(w, input, 9600);
  auto st = status_when(w, [](auto& s) { return s.state == be::ProcessingState::Bypass; });
  CHECK(st.state == be::ProcessingState::Bypass);
  CHECK_MESSAGE(contains(st.reason, "wyjątek"), st.reason);
  CHECK_MESSAGE(contains(st.reason, "fake Stereo Tool process failure"), st.reason);
  CHECK(st.cpu_pct == 0);
  CHECK(wait_until([] { return fake().value("deleted") == 1; }));  // a failed instance is let go
  feed(w, input, 9600);
  // restart: a new instance, processing again
  fake().set("process_throws", 0);
  w.restart();
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  CHECK(fake().value("created") == 2);
  // every change was an event, in order (the engine logs them)
  const auto lines = event_lines(w);
  REQUIRE(lines.size() == 7);
  CHECK(lines[0] == "bypass: uruchamianie Stereo Tool");
  CHECK(contains(lines[1], "bypass: sprawdzanie licencji"));
  CHECK(lines[2] == "running: ");
  CHECK_MESSAGE(contains(lines[3], "bypass: Stereo Tool zgłosił wyjątek w stereoTool_Process"), lines[3]);
  CHECK(lines[4] == "bypass: uruchamianie Stereo Tool");
  CHECK(contains(lines[5], "bypass: sprawdzanie licencji"));
  CHECK(lines[6] == "running: ");
  std::vector<bd::ProcessingEvent> none;
  CHECK(w.events_since(w.events_since(0, nullptr), &none) == 7);
  CHECK(none.empty());
  feed(w, input, 9600);
  REQUIRE(drained(w, col, input));
  const auto out = col.copy();
  const Path p = analyse(input.in, out, 0.5f);
  CHECK(p.mismatches == 0);
  REQUIRE(p.switches.size() == 3);
  CHECK(silent_blocks(out) == 0);
}

TEST_CASE("processing: each event keeps the failure text of its moment") {
  UseFake use;
  fake().set("latency", 256);
  Collector col;
  Input input;
  PW w(settings(col), "s5c");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  fake().set("process_throws", 1);
  feed(w, input, 9600);
  REQUIRE(status_when(w, [](auto& s) { return s.state == be::ProcessingState::Bypass; }).state ==
          be::ProcessingState::Bypass);
  // another failure before the engine reads the events: the first one still has its own text
  fake().set("create_fails", 1);
  w.restart();
  feed(w, input, 4800);
  const auto st = status_when(w, [](auto& s) { return s.state == be::ProcessingState::Error; });
  REQUIRE(st.state == be::ProcessingState::Error);
  CHECK_MESSAGE(contains(st.reason, "nie udało się utworzyć instancji"), st.reason);
  const auto lines = event_lines(w);
  REQUIRE(lines.size() == 6);
  CHECK_MESSAGE(contains(lines[3], "bypass: Stereo Tool zgłosił wyjątek w stereoTool_Process"), lines[3]);
  CHECK(lines[4] == "bypass: uruchamianie Stereo Tool");
  CHECK_MESSAGE(contains(lines[5], "error: nie udało się utworzyć instancji"), lines[5]);
}

TEST_CASE("processing: events overwritten while they are read are left out") {
  bd::EventRing ring;
  std::vector<std::pair<uint64_t, uint64_t>> got;
  for (uint64_t v = 1; v <= 5; ++v) ring.push(v);
  // the writer laps the reader between reading the slots and checking them: nothing of it is used
  uint64_t next = ring.read(0, &got, [&] {
    for (uint64_t k = 0; k < 40; ++k) ring.push(100 + k);
  });
  CHECK(next == 5);
  CHECK(got.empty());
  // the next read sees the newest slots, all but the one the writer may be overwriting right now
  next = ring.read(next, &got);
  CHECK(next == 45);
  REQUIRE(got.size() == bd::EventRing::kSlots - 1);
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].first == 15 + i);   // seq
    CHECK(got[i].second == 109 + i);  // the value pushed as event 15 + i
  }
  // partly overwritten: the slots still intact are used
  bd::EventRing r2;
  got.clear();
  for (uint64_t v = 1; v <= 10; ++v) r2.push(v);
  CHECK(r2.read(0, &got, [&] {
    for (uint64_t k = 0; k < 25; ++k) r2.push(100 + k);
  }) == 10);
  REQUIRE(got.size() == 6);
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].first == 5 + i);
    CHECK(got[i].second == 5 + i);
  }
  // nothing new: nothing
  got.clear();
  CHECK(r2.read(35, &got) == 35);
  CHECK(got.empty());
}

TEST_CASE("processing: a Stereo Tool too slow or stuck bypasses after 200 ms behind, nothing lost") {
  UseFake use;
  StopTimeout quick(300);
  fake().set("gain", 0.5);
  fake().set("latency", 512);
  Collector col;
  Input input;
  PW w(settings(col), "s6");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  SUBCASE("too slow: 15 ms per 10 ms block") { fake().set("process_sleep_us", 15000); }
  SUBCASE("stuck: one call takes 1.5 s") { fake().set("process_sleep_us", 1500000); }
  const int64_t t0 = bp::monotonic_ns();
  feed(w, input, 96000, 10'000'000);  // real time, as the station pushes
  const auto st = w.status();
  CHECK(st.state == be::ProcessingState::Bypass);
  CHECK_MESSAGE(contains(st.reason, "nie nadąża"), st.reason);
  CHECK(bp::monotonic_ns() - t0 < 4'000'000'000);  // the station was never held up
  const auto lines = event_lines(w);
  REQUIRE(lines.size() >= 4);
  CHECK(lines[lines.size() - 2] == "running: ");
  CHECK(contains(lines.back(), "bypass: Stereo Tool nie nadąża"));
  REQUIRE(drained(w, col, input));
  const auto out = col.copy();
  const Path p = analyse(input.in, out, 0.5f);
  CHECK(p.mismatches == 0);
  REQUIRE(p.switches.size() == 2);
  CHECK_FALSE(p.switches[1].second);
  CHECK(silent_blocks(out) == 0);
  CHECK(st.queue_dropped_frames == 0);
  fake().set("process_sleep_us", 0);
  w.stop();
  CHECK(wait_until([] { return fake().value("live") == 0; }, 5000));
}

TEST_CASE("processing: a station catch-up burst that Stereo Tool drains keeps processing") {
  // The station pushes nothing during a stall, then the backlog in passes of up to 0.5 s; a
  // Stereo Tool that is faster than real time drains it, so processing goes on.
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 512);
  int work_us = 0, stall_ms = 0;
  // the fake works to a deadline (process_work_us), so the load is what is asked for, also where
  // a sleep overshoots (a sleep of 8 ms measured 91-95 % on a Mac, slower than real time on a
  // macOS CI VM)
  SUBCASE("50 % CPU, 0.3 s stall") { work_us = 5000, stall_ms = 300; }
  SUBCASE("50 % CPU, 0.5 s stall") { work_us = 5000, stall_ms = 500; }
  SUBCASE("80 % CPU, 0.1 s stall") { work_us = 8000, stall_ms = 100; }
  SUBCASE("80 % CPU, 0.3 s stall") { work_us = 8000, stall_ms = 300; }
  SUBCASE("90 % CPU (the review's case measured 91-95 %), 0.1 s stall") { work_us = 9000, stall_ms = 100; }
  // more than the 1.5 s backlog cap at once: drained fast, so it is no reason to bypass
  SUBCASE("30 % CPU, 1.6 s stall") { work_us = 3000, stall_ms = 1600; }
  CAPTURE(work_us);
  CAPTURE(stall_ms);
  fake().set("process_work_us", work_us);
  const double work_ms = work_us / 1000.0;
  const double block_ms = fake_block_ms();
  CAPTURE(block_ms);
  if (block_ms > headroom_limit_ms(work_ms, 10.0)) {
    MESSAGE("skipped: a " << work_ms << " ms block takes " << block_ms << " ms on this runner");
    return;
  }
  Collector col;
  Input input;
  PW w(settings(col), "burst");
  CallSampler calls;  // how fast Stereo Tool really is (runner_excuse)
  if (!feed_until(w, input, be::ProcessingState::Running, 10'000'000)) {
    if (const std::string why = runner_excuse(calls, work_ms, 10.0); !why.empty()) {
      MESSAGE("skipped: " << why << " (" << w.status().reason << ")");
      return;
    }
    FAIL("never running although Stereo Tool kept up: " << w.status().reason);
  }
  feed(w, input, 48000, 10'000'000);
  const double cpu_before = w.status().cpu_pct;
  CAPTURE(cpu_before);
  MESSAGE("Stereo Tool CPU " << cpu_before << " %, stall " << stall_ms << " ms");
  std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
  for (size_t backlog = size_t(stall_ms) * 48; backlog > 0;) {
    const size_t n = std::min<size_t>(backlog, 24000);
    const size_t from = input.frames();
    input.extend(n);
    w.push(input.in.data() + from * 2, n);
    backlog -= n;
  }
  feed(w, input, 72000, 10'000'000);  // 1.5 s of real time after it
  const auto st = w.status();
  if (st.state != be::ProcessingState::Running) {
    if (const std::string why = runner_excuse(calls, work_ms, 10.0); !why.empty()) {
      MESSAGE("skipped: " << why << " (" << st.reason << ")");
      fake().set("process_work_us", 0);
      return;
    }
    FAIL_CHECK("bypassed although Stereo Tool kept up: " << st.reason << " (cpu " << st.cpu_pct << " %, slowest call "
                                                          << calls.slowest_ms() << " ms)");
  }
  fake().set("process_work_us", 0);
  REQUIRE(drained(w, col, input));
  const Path p = analyse(input.in, col.copy(), 0.5f);
  CHECK(p.mismatches == 0);
  CHECK(p.switches.size() == 1);
}

TEST_CASE("processing: an exception after another station's crash is an exception, not a crash") {
  UseFake use;
  Collector col;
  Input input;
  PW w(settings(col), "s5b");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  be::testing::set_stereotool_abandoned(1);  // another station's instance crashed meanwhile
  fake().set("process_throws", 1);
  feed(w, input, 9600);
  const auto st = status_when(w, [](auto& s) { return s.state != be::ProcessingState::Running; });
  CHECK(st.state == be::ProcessingState::Bypass);
  CHECK_MESSAGE(contains(st.reason, "wyjątek"), st.reason);
  CHECK_MESSAGE(!contains(st.reason, "uległ awarii"), st.reason);
}

TEST_CASE("processing: after a crash nothing is created again and restart does not loop") {
  UseFake use;
  be::testing::set_stereotool_abandoned(1);  // what a structured exception in the library leaves
  Collector col;
  Input input;
  PW w(settings(col), "s7");
  auto st = status_when(w, [](auto& s) { return s.state == be::ProcessingState::Error; });
  CHECK(st.state == be::ProcessingState::Error);
  CHECK_MESSAGE(contains(st.reason, "Stereo Tool uległ awarii — uruchom ponownie usługę Bounce Encoder"), st.reason);
  for (int i = 0; i < 3; ++i) {
    w.restart();
    feed(w, input, 4800);
  }
  st = status_when(w, [](auto& s) { return s.state == be::ProcessingState::Error; });
  CHECK(st.state == be::ProcessingState::Error);
  CHECK_MESSAGE(contains(st.reason, "uległ awarii"), st.reason);
  CHECK(fake().value("create_calls") == 0);
  REQUIRE(drained(w, col, input));
  CHECK(col.copy() == input.in);
}

TEST_CASE("processing: CPU is the Stereo Tool time over the audio time, per second") {
  UseFake use;
  fake().set("latency", 0);
  fake().set("process_work_us", 2000);  // 2 ms per 10 ms block: 20 %, to a deadline (no sleep overshoot)
  // fed at twice real time below: a block arrives every 5 ms
  const double block_ms = fake_block_ms();
  CAPTURE(block_ms);
  if (block_ms > headroom_limit_ms(2.0, 5.0)) {
    MESSAGE("skipped: a 2 ms block takes " << block_ms << " ms on this runner");
    return;
  }
  Collector col;
  Input input;
  PW w(settings(col), "s8");
  CHECK(w.status().cpu_pct == 0);
  CallSampler calls;
  const bool running = feed_until(w, input, be::ProcessingState::Running, 5'000'000);
  if (running) feed(w, input, 96000, 5'000'000);  // twice real time: Stereo Tool keeps up
  const auto st = w.status();
  if (st.state != be::ProcessingState::Running) {
    if (const std::string why = runner_excuse(calls, 2.0, 5.0); !why.empty()) {
      MESSAGE("skipped: " << why << " (fed at twice real time; " << st.reason << ")");
      return;
    }
  }
  CHECK(st.state == be::ProcessingState::Running);
  CHECK(st.cpu_pct >= 18.0);
  CHECK(st.cpu_pct < 60.0);
}

TEST_CASE("processing: the queue from the station is bounded") {
  UseFake use;
  CHECK(PW::kQueueFrames == 2 * 48000);
  Collector col;
  Input input;
  PW w(settings(col), "s9");
  input.extend(3 * 48000);
  w.push(input.in.data(), input.frames());  // 3 s at once: only the newest 2 s can stay
  CHECK(w.status().queue_dropped_frames >= 48000);
  CHECK(wait_until([&] { return col.frames() + w.delay_frames() + w.status().queue_dropped_frames >= input.frames(); }));
  CHECK(col.frames() <= input.frames() - 48000);
}

TEST_CASE("processing: stop() does not wait for ever on a Stereo Tool stuck in a call") {
  UseFake use;
  StopTimeout quick(200);
  Collector col;
  Input input;
  {
    PW w(settings(col), "s10");
    REQUIRE(feed_until(w, input, be::ProcessingState::Running));
    fake().set("process_sleep_us", 2000000);
    feed(w, input, 9600, 10'000'000);
    const int64_t t0 = bp::monotonic_ns();
    w.stop();
    CHECK(bp::monotonic_ns() - t0 < 1'500'000'000);
    CHECK(w.status().state == be::ProcessingState::Off);
  }  // the worker is gone; its Stereo Tool thread finishes the call on its own
  fake().set("process_sleep_us", 0);
  CHECK(wait_until([] { return fake().value("live") == 0; }, 5000));  // and then deletes the instance
}

TEST_CASE("processing: no allocation per block on the output and Stereo Tool threads") {
  UseFake use;
  fake().set("latency", 512);
  constexpr size_t kMax = 4000;
  static std::array<long long, kMax> out_allocs{}, st_allocs{};
  static std::atomic<size_t> out_calls{0}, st_calls{0};
  out_calls = 0;
  st_calls = 0;
  bd::ProcessingSettings s;
  s.output_tap = [](const float*, size_t) {
    const size_t i = out_calls.load();
    if (i < kMax) out_allocs[i] = t_new_calls;
    out_calls.store(i + 1);
  };
  s.st_block_tap = [] {
    const size_t i = st_calls.load();
    if (i < kMax) st_allocs[i] = t_new_calls;
    st_calls.store(i + 1);
  };
  Input input(48000 * 8);
  PW w(s, "s11");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 9600);
  const size_t out0 = out_calls, st0 = st_calls;
  feed(w, input, 3 * 48000);  // over licence re-checks and CPU updates
  REQUIRE(wait_until([&] { return st_calls >= st0 + 280; }));
  const size_t out1 = std::min(out_calls.load(), kMax), st1 = std::min(st_calls.load(), kMax);
  REQUIRE(out1 > out0 + 200);
  REQUIRE(st1 > st0 + 200);
  CHECK(w.status().state == be::ProcessingState::Running);
  CHECK(out_allocs[out1 - 1] - out_allocs[out0] == 0);
  CHECK(st_allocs[st1 - 1] - st_allocs[st0] == 0);
}

TEST_CASE("processing: streams are handed over like a plan and released off the audio threads") {
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 0);
  be::StreamConfig sc;
  sc.id = "st1";
  sc.codec = be::Codec::Flac;
  sc.sample_rate = 48000;
  sc.host = "127.0.0.1";
  sc.port = 1;  // nothing listens: the stream only encodes
  sc.mount = "/pw";
  auto stream = std::make_shared<bd::StreamWorker>(sc, "Station PW");
  REQUIRE(stream->active());
  Collector col;
  Input input;
  PW w(settings(col), "s12");
  w.set_streams({stream});
  feed(w, input, 24000);
  CHECK(wait_until([&] { return stream->status().peak_db[0] > -20.0f; }));
  w.set_streams({});
  feed(w, input, 4800);
  w.set_streams({});  // the list the output thread let go of is released here
  CHECK(stream.use_count() == 1);
}

TEST_CASE("processing: a new preset is loaded into the running instance, aligned, with crossfades") {
  UseFake use;
  TempDir dir("reload");
  const std::string p1 = (fs::path(dir.path) / "a.sts").string(), p2 = (fs::path(dir.path) / "b.sts").string();
  std::ofstream(p1, std::ios::binary) << "[FakeStereoTool]\ngain=0.5\nlatency=512\n";
  std::ofstream(p2, std::ios::binary) << "[FakeStereoTool]\ngain=0.25\nlatency=300\n";
  Collector col;
  Input input;
  auto s = settings(col);
  s.preset_path = p1;
  s.web_port = 8123;
  PW w(s, "rl");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  CHECK(w.status().web_port == 8123);
  feed(w, input, 9600);
  w.set_preset(p2);
  REQUIRE(status_when(w, [](auto& x) { return x.state != be::ProcessingState::Running; }).reason_code ==
          be::ProcessingReason::Starting);
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 9600);
  REQUIRE(drained(w, col, input));
  CHECK(w.delay_frames() == 512 + kRamp);  // a shorter latency never shrinks the delay
  CHECK(fake().value("created") == 1);     // the same instance
  CHECK(fake().value("load_calls") == 2);
  CHECK(fake().text("load_path") == std::optional<std::string>(p2));
  CHECK(w.status().web_port == 8123);  // the web interface kept its port
  const auto out = col.copy();
  const Path p = analyse(input.in, out, std::vector<float>{0.5f, 0.25f});
  CHECK_MESSAGE(p.mismatches == 0, "first mismatch at frame " << p.first_mismatch);
  REQUIRE(p.switches.size() == 3);  // in, out for the reload, in with the new preset
  CHECK(silent_blocks(out) == 0);
  // a preset that cannot be loaded: an error, the unprocessed signal, the instance let go
  w.set_preset((fs::path(dir.path) / "missing.sts").string());
  const auto st = status_when(w, [](auto& x) { return x.state == be::ProcessingState::Error; });
  CHECK_MESSAGE(contains(st.reason, "nie wczytał presetu"), st.reason);
  CHECK(st.reason_code == be::ProcessingReason::Setup);
  CHECK(st.web_port == 0);
  CHECK(wait_until([] { return fake().value("live") == 0; }));
}

TEST_CASE("processing: the settings are saved on the Stereo Tool thread while processing goes on") {
  UseFake use;
  TempDir dir("save");
  Collector col;
  Input input;
  PW w(settings(col), "sv");
  std::string err;
  // no instance yet (or any more): refused, nothing waits for ever
  fake().set("create_fails", 1);
  REQUIRE(status_when(w, [](auto& x) { return x.state == be::ProcessingState::Error; }).state == be::ProcessingState::Error);
  CHECK_FALSE(w.save_preset((fs::path(dir.path) / "x.sts").string(), 2000, &err));
  CHECK_MESSAGE(contains(err, "brak instancji"), err);
  fake().set("create_fails", 0);
  w.restart();
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  std::atomic<bool> stop{false};
  std::thread feeder([&] {
    while (!stop) feed(w, input, 480, 10'000'000);
  });
  const std::string path = (fs::path(dir.path) / "saved.sts").string();
  CHECK(w.save_preset(path, 5000, &err));
  std::ifstream f(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(text.rfind("[FakeStereoTool]\ngain=0.5\nlatency=512\n", 0) == 0);  // then its .sts values
  fake().set("save_fails", 1);
  CHECK_FALSE(w.save_preset(path + "2", 5000, &err));
  CHECK(err == "Stereo Tool nie zapisał presetu");
  bp::sleep_ns(200'000'000);
  stop = true;
  feeder.join();
  CHECK(w.status().state == be::ProcessingState::Running);
  w.stop();
  CHECK_FALSE(w.save_preset(path + "3", 2000, &err));
  CHECK_MESSAGE(contains(err, "zatrzymany"), err);
}

TEST_CASE("processing: a save longer than 200 ms bypasses as a save and comes back on its own") {
  UseFake use;
  TempDir dir("slowsave");
  fake().set("gain", 0.5);
  fake().set("latency", 512);
  Collector col;
  Input input;
  PW w(settings(col), "ss");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  std::atomic<bool> stop{false};
  std::thread feeder([&] {
    while (!stop) feed(w, input, 480, 10'000'000);  // real time, as the station pushes
  });
  fake().set("save_sleep_us", 500000);
  std::string err;
  const bool saved = w.save_preset((fs::path(dir.path) / "slow.sts").string(), 5000, &err);
  const bool back = wait_until([&] { return w.status().state == be::ProcessingState::Running; }, 3000);
  bp::sleep_ns(300'000'000);
  stop = true;
  feeder.join();
  fake().set("save_sleep_us", 0);
  CHECK_MESSAGE(saved, err);
  CHECK(back);
  const auto lines = event_lines(w);
  size_t saving = SIZE_MAX;
  for (size_t i = 0; i < lines.size(); ++i)
    if (contains(lines[i], "bypass: zapis presetu")) saving = i;
  REQUIRE_MESSAGE(saving != SIZE_MAX, "no save bypass among the events");
  for (const auto& l : lines) CHECK_MESSAGE(!contains(l, "nie nadąża"), l);
  for (size_t i = saving; i < lines.size(); ++i) CHECK_MESSAGE(!contains(lines[i], "uruchamianie"), lines[i]);  // no restart
  CHECK(lines.back() == "running: ");
  CHECK(fake().value("latency_calls") == 1);
  REQUIRE(drained(w, col, input));
  const Path p = analyse(input.in, col.copy(), 0.5f);
  CHECK_MESSAGE(p.mismatches == 0, "first mismatch at frame " << p.first_mismatch);
  CHECK(p.switches.size() == 3);  // in, out for the save, back in
  CHECK(silent_blocks(col.copy()) == 0);
}

TEST_CASE("processing: a preset set while the instance is being built is loaded by that same setup") {
  UseFake use;
  TempDir dir("during");
  const std::string p1 = (fs::path(dir.path) / "a.sts").string(), p2 = (fs::path(dir.path) / "b.sts").string();
  std::ofstream(p1, std::ios::binary) << "[FakeStereoTool]\ngain=0.5\nlatency=512\n";
  std::ofstream(p2, std::ios::binary) << "[FakeStereoTool]\ngain=0.25\nlatency=512\n";
  fake().set("create_sleep_us", 300000);
  Collector col;
  Input input;
  auto s = settings(col);
  s.preset_path = p1;
  PW w(s, "pd");
  bp::sleep_ns(100'000'000);  // inside Create3
  w.set_preset(p2);
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 9600);
  CHECK(fake().value("load_calls") == 1);
  CHECK(fake().text("load_path") == std::optional<std::string>(p2));
  CHECK(fake().value("latency_calls") == 1);  // one setup, no second restart
  size_t starts = 0;
  for (const auto& l : event_lines(w)) starts += l == "bypass: uruchamianie Stereo Tool";
  CHECK(starts == 1);
}

TEST_CASE("processing: fade_out() ramps the output down over 240 frames, then hands over nothing") {
  UseFake use;
  Collector col;
  Input input;
  input.dc = 0.5f;
  PW w(settings(col), "fo");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 9600);
  REQUIRE(drained(w, col, input));
  const size_t before = col.frames();
  CHECK_FALSE(w.faded_out());
  w.fade_out();
  feed(w, input, 4800);
  REQUIRE(wait_until([&] { return w.faded_out(); }));
  feed(w, input, 4800);
  bp::sleep_ns(100'000'000);
  const auto out = col.copy();
  REQUIRE(out.size() / 2 == before + kRamp);  // the fade, and nothing after it
  for (uint32_t k = 0; k < kRamp; ++k) {
    const double g = double(kRamp - 1 - k) / kRamp;  // GainRamp from 1 to 0
    CHECK(out[(before + k) * 2] == static_cast<float>(g * double(0.25f)));
  }
  CHECK(out.back() == 0.0f);
}

TEST_CASE("processing: key, station name, home folder, web port and preset reach the instance; no key in reasons") {
  UseFake use;
  TempDir dir("settings");
  const std::string preset = (fs::path(dir.path) / "p.sts").string();
  {
    std::ofstream f(preset, std::ios::binary);
    f << "[FakeStereoTool]\ngain=0.25\nlatency=100\n";
  }
  Collector col;
  Input input;
  auto s = settings(col);
  s.license_key = "SECRET-KEY-789";
  s.home_dir = dir.path;
  s.preset_path = preset;
  s.web_port = 8123;
  {
    PW w(s, "abc");
    REQUIRE(feed_until(w, input, be::ProcessingState::Running));
    CHECK(fake().text("create_key") == std::optional<std::string>("SECRET-KEY-789"));
    CHECK(fake().text("create_name") == std::optional<std::string>("station-abc"));
    CHECK(fake().text("create_home") == std::optional<std::string>(dir.path));
    CHECK(fake().text("load_path") == std::optional<std::string>(preset));
    CHECK(fake().sts("station-abc", be::detail::st::kParamWebserverPort) == std::optional<std::string>("8123"));
    CHECK(fake().sts("station-abc", be::detail::st::kParamWebserverEnable) == std::optional<std::string>("1"));
    CHECK(fake().text("bind_web") == std::optional<std::string>("127.0.0.1"));
    feed(w, input, 9600);
    REQUIRE(drained(w, col, input));
    CHECK(w.delay_frames() == 100 + kRamp);
    const Path p = analyse(input.in, col.copy(), 0.25f);
    CHECK(p.mismatches == 0);
    CHECK(p.switches.size() == 1);
  }
  fake().set("create_throws", 1);  // the fake's exception text holds the key
  Collector col2;
  auto s2 = settings(col2);
  s2.license_key = "SECRET-KEY-789";
  PW w2(s2, "abc");
  const auto st = status_when(w2, [](auto& x) { return x.state == be::ProcessingState::Error; });
  CHECK_MESSAGE(contains(st.reason, "fake Create3 failure"), st.reason);
  CHECK_MESSAGE(!contains(st.reason, "SECRET-KEY-789"), st.reason);
}

#endif  // BAM_FAKE_STEREOTOOL_PATH
