// SPDX-License-Identifier: GPL-3.0-or-later
// The station thread (spec §4.1–§4.3): every 10 ms it reads each input ring up to
// "now - receive latency" on the media clock, feeds the input's delay line, mixes the delayed
// inputs and hands the mix to the queue of each stream worker. It never blocks on I/O, never
// allocates, and never releases what it lets go of: replaced plans and inputs go to a graveyard
// emptied by the API thread.
#include <algorithm>

#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam::encoder::detail {

namespace {
constexpr float kInt32Scale = 1.0f / 2147483648.0f;  // left-aligned int32 ring sample -> ±1.0
constexpr size_t kGraveyardSlots = 64;
constexpr uint64_t kPeakFrames = kStationRate / 10;  // ~100 ms meter window
constexpr uint64_t kResyncBackFrames = kStationRate * 2;  // "now" went back this far: start over
}  // namespace

Station::Station(const ClockSource& clock, std::shared_ptr<const StationPlan> plan)
    : clock_(clock), started_ns_(platform::monotonic_ns()), pending_(std::move(plan)), has_pending_(true) {
  graveyard_.reserve(kGraveyardSlots);
  for (auto& p : pcm_) p.assign(kPassFrames, 0);
  for (auto& b : in_) b.assign(size_t(kPassFrames) * 2, 0.0f);
  mix_.assign(size_t(kPassFrames) * 2, 0.0f);
  thread_ = std::thread([this] { run(); });
}

Station::~Station() { stop(); }

void Station::publish(std::shared_ptr<const StationPlan> plan) {
  std::shared_ptr<const StationPlan> unused;
  std::vector<std::shared_ptr<const void>> dead;
  dead.reserve(kGraveyardSlots);
  {
    std::lock_guard lock(plan_m_);
    unused = std::move(pending_);  // a plan the thread never took
    pending_ = std::move(plan);
    graveyard_.swap(dead);  // `dead` was reserved here, so the station's next retire() never allocates
    has_pending_.store(true, std::memory_order_release);
  }
  // `dead` and `unused` are released here, on the caller's thread
}

void Station::collect() {
  std::vector<std::shared_ptr<const void>> dead;
  dead.reserve(kGraveyardSlots);
  std::lock_guard lock(plan_m_);
  if (graveyard_.empty()) return;  // `dead` (still empty) goes on this thread
  graveyard_.swap(dead);
  // `dead` holds the released objects now; they go when it leaves scope, after the lock
}

void Station::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  // everything the thread held is released on the caller's thread
  plan_.reset();
  for (auto& a : active_) a = Active{};
  std::lock_guard lock(plan_m_);
  pending_.reset();
  graveyard_.clear();
}

void Station::retire(std::shared_ptr<const void> p) {
  if (!p) return;
  std::lock_guard lock(plan_m_);
  if (graveyard_.size() < graveyard_.capacity()) graveyard_.push_back(std::move(p));
  // else (more than kGraveyardSlots releases between two publishes): released here after all
}

// The plan input with the same identity (delay line) as `delay`, or nullptr.
static const std::shared_ptr<const StationPlan::Input>* same_input(const StationPlan& plan, const DelayLine* delay) {
  for (const auto& w : plan.inputs)
    if (w && w->delay.get() == delay) return &w;
  return nullptr;
}

void Station::apply_settings(int slot, const StationPlan::Input& w) {
  // only changed values reach the delay line and the mixer (a repeated set would re-fade)
  Active& a = active_[slot];
  if (w.delay_s != a.delay_s) {
    w.delay->set_delay_seconds(w.delay_s);
    a.delay_s = w.delay_s;
  }
  if (w.gain != a.gain) {
    mixer_.set_gain(slot, w.gain);
    a.gain = w.gain;
  }
}

void Station::take_plan() {
  std::shared_ptr<const StationPlan> next;
  {
    std::lock_guard lock(plan_m_);
    next = std::move(pending_);
    has_pending_.store(false, std::memory_order_relaxed);
  }
  if (!next) return;
  // An input is the same input as long as its delay line is (the engine keys it by its SDP):
  // its settings change in place. One no longer in the plan leaves: it fades out over
  // kRampFrames, still feeding its delay line, and frees its slot once inaudible.
  for (int i = 0; i < kStationInputs; ++i) {
    Active& a = active_[i];
    if (!a.input) continue;
    if (const auto* w = same_input(*next, a.input->delay.get())) {
      apply_settings(i, **w);
      if (a.input != *w) {
        retire(std::move(a.input));
        a.input = *w;  // e.g. the same input on a new socket (a retried receiver)
      }
      if (a.leaving) {  // back before it had faded out
        mixer_.set_enabled(i, true);
        a.leaving = false;
      }
    } else if (!a.leaving) {
      mixer_.set_enabled(i, false);
      a.leaving = true;
    }
  }
  retire(std::move(plan_));
  plan_ = std::move(next);
  place_joins();
}

void Station::place_joins() {
  for (const auto& w : plan_->inputs) {
    if (!w) continue;
    bool placed = false;
    for (const Active& a : active_) placed = placed || (a.input && a.input->delay == w->delay);
    if (placed) continue;
    int free = -1;
    for (int i = 0; i < kStationInputs && free < 0; ++i)
      if (!active_[i].input) free = i;
    if (free < 0) return;  // every slot busy (one still fading out): joins after that fade
    Active& a = active_[free];
    a = Active{};
    a.input = w;
    owner_[free].store(w->delay.get(), std::memory_order_relaxed);
    apply_settings(free, *w);
    mixer_.set_enabled(free, true);  // fades in from silence (a fresh delay line)
  }
}

void Station::run() {
  platform::set_realtime_priority("Pro Audio");
  int64_t next_wake = platform::monotonic_ns();
  while (running_) {
    next_wake += kTickNs;
    const int64_t before = platform::monotonic_ns();
    if (before - next_wake > 5 * kTickNs) next_wake = before;  // fell far behind: no burst of ticks
    platform::sleep_until_ns(next_wake);

    if (has_pending_.load(std::memory_order_acquire)) take_plan();
    if (!plan_) continue;

    uint64_t generation = 0;
    const MediaClock clock = clock_.get(&generation);  // the mapping and its generation together
    const uint64_t now = clock.sample_at(platform::monotonic_ns());
    if (now < plan_->latency_frames) continue;
    const uint64_t end = now - plan_->latency_frames;
    if (!positioned_ || generation != step_generation_) {
      // first tick, or the media clock jumped (PTP step): start mixing from here
      pos_ = end;
      positioned_ = true;
      step_generation_ = generation;
      continue;
    }
    if (end <= pos_) {
      if (pos_ - end > kResyncBackFrames) pos_ = end;  // clock far behind without a step: start over
      continue;  // a longer receive latency: wait until the inputs are that far again
    }
    if (end - pos_ > kMaxCatchUpFrames) {  // stalled for longer than the rings can bridge
      skipped_.fetch_add(end - pos_ - kMaxCatchUpFrames, std::memory_order_relaxed);
      pos_ = end - kMaxCatchUpFrames;
    }
    while (pos_ < end && running_) {  // catch-up in passes the delay lines can hold
      const size_t n = static_cast<size_t>(std::min<uint64_t>(end - pos_, kPassFrames));
      process(pos_, n);
      pos_ += n;
    }
  }
}

void Station::process(uint64_t from, size_t frames) {
  const float* inputs[kStationInputs] = {nullptr, nullptr, nullptr};
  for (int i = 0; i < kStationInputs; ++i) {
    Active& a = active_[i];
    if (!a.input) continue;
    float* buf = in_[i].data();
    const InputRx& rx = *a.input->rx;
    if (rx.stream) {
      rx.rings[0].read(from, pcm_[0].data(), frames);
      rx.rings[1].read(from, pcm_[1].data(), frames);
      for (size_t f = 0; f < frames; ++f) {
        buf[f * 2] = static_cast<float>(pcm_[0][f]) * kInt32Scale;
        buf[f * 2 + 1] = static_cast<float>(pcm_[1][f]) * kInt32Scale;
      }
    } else {
      std::fill(buf, buf + frames * 2, 0.0f);  // not receiving: silence keeps the delay timeline
    }
    a.input->delay->write(buf, frames);
    a.input->delay->read(buf, frames);
    inputs[i] = buf;
  }
  mixer_.mix(inputs, frames, mix_.data());
  bool freed = false;
  for (int i = 0; i < kStationInputs; ++i) {
    Active& a = active_[i];
    if (a.leaving && !mixer_.audible(i)) {  // faded out: let go of it
      retire(std::move(a.input));
      a = Active{};
      owner_[i].store(nullptr, std::memory_order_relaxed);
      freed = true;
    }
  }
  if (freed) place_joins();
  for (const auto& stream : plan_->streams) stream->push(mix_.data(), frames);

  frames_since_peaks_ += frames;
  if (frames_since_peaks_ >= kPeakFrames) publish_peaks();
}

void Station::publish_peaks() {
  const StationMixer::Peaks p = mixer_.take_peaks();
  for (int i = 0; i < kStationInputs; ++i)
    for (int s = 0; s < 2; ++s) input_peak_[i * 2 + s].store(p.input[i][s], std::memory_order_relaxed);
  for (int s = 0; s < 2; ++s) mix_peak_[s].store(p.mix[s], std::memory_order_relaxed);
  frames_since_peaks_ = 0;
}

std::optional<std::array<float, 2>> Station::input_peak(const DelayLine* input) const {
  for (int i = 0; i < kStationInputs; ++i)
    if (input && owner_[i].load(std::memory_order_relaxed) == input)
      return std::array<float, 2>{input_peak_[i * 2].load(std::memory_order_relaxed),
                                  input_peak_[i * 2 + 1].load(std::memory_order_relaxed)};
  return std::nullopt;
}

std::array<float, 2> Station::mix_peak() const {
  return {mix_peak_[0].load(std::memory_order_relaxed), mix_peak_[1].load(std::memory_order_relaxed)};
}

}  // namespace bam::encoder::detail
