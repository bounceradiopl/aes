// SPDX-License-Identifier: GPL-3.0-or-later
// Stereo Tool processing of one station (spec §3), see ProcessingWorker in engine_internal.hpp.
//
// Two threads besides the station's. The output thread owns the timeline: it takes the
// station's mix from the queue, keeps it in the dry ring (the bypass), hands each block to the
// Stereo Tool thread through a one-block mailbox and waits for it (in slices, never longer than
// the worker may fall behind), stores the result in the processed ring, and emits both rings
// crossfaded, delay_frames() behind the input. The Stereo Tool thread owns the instance and
// makes every call into the library; it may outlive the worker when a call never returns
// (Core is shared).
//
// Mailbox: the output thread posts a command only when none is pending, and the block buffer
// (Core::job) belongs to the Stereo Tool thread from the post until its result is taken. The
// mutex is held only to post or take; nothing is copied or called under it.
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "bam/encoder/stereotool.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam::encoder::detail {

namespace {

using PW = ProcessingWorker;

constexpr uint32_t kRingFrames = 1u << 15;  // dry and processed rings, indexed by input frame
constexpr int64_t kRingMask = kRingFrames - 1;
static_assert(kRingFrames >= PW::kMaxLatencyFrames + kRampFrames + 2 * PW::kBlockFrames,
              "the rings hold the longest delay and a block on either side");
constexpr int kPopWaitMs = 20;                         // the output thread looks at its flags this often
constexpr auto kWaitSlice = std::chrono::milliseconds(5);  // and at the queue while Stereo Tool works
constexpr int kDefaultStopTimeoutMs = 5000;
constexpr size_t kGraveyardSlots = 64;

std::atomic<int> g_stop_timeout_ms{0};

enum class Cmd { None, Setup, Restart, Process };
enum class Done { None, Ready, SetupFailed, Processed, ProcessFailed };
enum class Verdict { None, Valid, Invalid };

// Text that may come from the library must not carry the licence key.
std::string without_key(std::string text, const std::string& key) {
  if (key.empty()) return text;
  for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 3)) text.replace(at, key.size(), "***");
  return text;
}

}  // namespace

namespace testing {
void set_processing_stop_timeout_ms(int ms) { g_stop_timeout_ms.store(ms); }
}  // namespace testing

struct ProcessingWorker::Core {
  // fixed
  std::string key, preset, home, name, station_id;
  int web_port = 0;
  std::function<void()> block_tap;

  // the mailbox, under m
  std::mutex m;
  std::condition_variable cv;  // both ways: a command posted, a command done, the thread ended
  Cmd cmd = Cmd::None;
  Done done = Done::None;
  bool quit = false;
  bool finished = false;
  std::vector<float> job;  // kBlockFrames stereo, processed in place
  size_t job_frames = 0;
  uint32_t latency = 0;              // Ready
  Verdict verdict = Verdict::None;   // Processed: a licence check came with this block
  bool crashed = false;              // ProcessFailed: a crash (nothing more until a service restart)

  // published for status()
  std::atomic<double> cpu_pct{0};
  std::atomic<int> licence{-1};  // the last check: -1 none yet, 0 invalid, 1 valid
  mutable std::mutex text_m;
  std::string failure;     // Polish, the last failure (no key)
  std::string unlicensed;  // the features the last check found unlicensed

  // Stereo Tool thread only
  std::unique_ptr<StereoToolInstance> st;
  uint64_t session_frames = 0;  // processed since the last (re)start
  uint64_t next_check = 0;      // session_frames of the next licence check
  int64_t cpu_ns = 0;
  uint64_t cpu_frames = 0;
};

namespace {

using Core = ProcessingWorker::Core;

void set_failure(Core& c, std::string text) {
  text = without_key(std::move(text), c.key);
  log_warning("Processing stacji " + c.station_id + ": " + text);
  std::lock_guard lock(c.text_m);
  c.failure = std::move(text);
}

// Builds the instance (or keeps a working one on restart), loads the preset, measures the latency.
Done setup(Core& c, bool restart, uint32_t* latency) {
  c.licence.store(-1);
  c.cpu_pct.store(0);
  c.cpu_ns = 0;
  c.cpu_frames = 0;
  {
    std::lock_guard lock(c.text_m);
    c.unlicensed.clear();
  }
  if (!restart || !c.st || c.st->failed()) {
    c.st.reset();  // a failed one goes first: its memory and web interface port
    if (restart) stereotool_reload();  // a library put in place since (nothing after a crash)
    std::string err;
    auto st = StereoToolInstance::create(c.key, c.name, c.home, &err);
    if (!st) {
      set_failure(c, err);
      return Done::SetupFailed;
    }
    if (c.web_port > 0 && !st->set_web_interface(true, c.web_port) && !st->failed())
      log_warning("Processing stacji " + c.station_id + ": Stereo Tool nie przyjął portu interfejsu WWW " +
                  std::to_string(c.web_port) + " (interfejs WWW wyłączony)");
    if (!st->failed() && !c.preset.empty() && !st->load_preset(c.preset, &err)) {
      set_failure(c, err);
      return Done::SetupFailed;  // the instance is deleted here: no ~600 MB kept for nothing
    }
    if (st->failed()) {
      set_failure(c, st->failure());
      return Done::SetupFailed;
    }
    c.st = std::move(st);
  }
  // feeds silence through the chain: only here, never on live audio
  const int frames = c.st->latency_frames();
  if (c.st->failed()) {
    set_failure(c, c.st->failure());
    c.st.reset();
    return Done::SetupFailed;
  }
  if (frames > int(PW::kMaxLatencyFrames)) {
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.0f", frames * 1000.0 / kStationRate);
    set_failure(c, std::string("opóźnienie Stereo Tool ") + ms + " ms jest większe niż dopuszczalne " +
                       std::to_string(PW::kMaxLatencyFrames * 1000 / kStationRate) + " ms");
    c.st.reset();
    return Done::SetupFailed;
  }
  c.session_frames = 0;
  c.next_check = PW::kLicenceFirstCheckFrames;
  *latency = uint32_t(frames);
  return Done::Ready;
}

Done process_failed(Core& c, bool* crashed) {
  *crashed = stereotool_info().abandoned > 0;
  set_failure(c, c.st->failure());
  c.st.reset();  // after a C++ exception deleted now (memory, web port); after a crash abandoned
  c.cpu_pct.store(0);
  return Done::ProcessFailed;
}

// One block in place, timed; the licence is checked when due. Allocates only on a failure.
Done process_block(Core& c, Verdict* verdict, bool* crashed) {
  *verdict = Verdict::None;
  *crashed = false;
  if (!c.st) return Done::ProcessFailed;  // never posted without an instance
  const int64_t t0 = platform::monotonic_ns();
  const bool ok = c.st->process(c.job.data(), int32_t(c.job_frames));
  const int64_t t1 = platform::monotonic_ns();
  if (!ok) return process_failed(c, crashed);
  c.cpu_ns += t1 - t0;
  c.cpu_frames += c.job_frames;
  if (c.cpu_frames >= kStationRate) {  // processing time / audio time, per second of audio
    c.cpu_pct.store(100.0 * double(c.cpu_ns) * kStationRate / (double(c.cpu_frames) * 1e9));
    c.cpu_ns = 0;
    c.cpu_frames = 0;
  }
  c.session_frames += c.job_frames;
  if (c.session_frames >= c.next_check) {
    c.next_check = c.session_frames + PW::kLicenceCheckFrames;
    const bool valid = c.st->license_valid();
    if (c.st->failed()) return process_failed(c, crashed);
    c.licence.store(valid ? 1 : 0);
    *verdict = valid ? Verdict::Valid : Verdict::Invalid;
    if (!valid) {
      std::string features = without_key(c.st->unlicensed_features(), c.key);
      log_warning("Processing stacji " + c.station_id + ": licencja Stereo Tool nie obejmuje funkcji: " +
                  (features.empty() ? std::string("(Stereo Tool nie podał listy)") : features));
      std::lock_guard lock(c.text_m);
      c.unlicensed = std::move(features);
    }
  }
  return Done::Processed;
}

void st_main(std::shared_ptr<Core> c) {
  for (;;) {
    Cmd cmd;
    {
      std::unique_lock lock(c->m);
      c->cv.wait(lock, [&] { return c->quit || c->cmd != Cmd::None; });
      if (c->quit) break;
      cmd = c->cmd;
      c->cmd = Cmd::None;
    }
    Done done;
    uint32_t latency = 0;
    Verdict verdict = Verdict::None;
    bool crashed = false;
    if (cmd == Cmd::Process) {
      done = process_block(*c, &verdict, &crashed);
      if (c->block_tap) c->block_tap();
    } else {
      done = setup(*c, cmd == Cmd::Restart, &latency);
    }
    {
      std::lock_guard lock(c->m);
      c->done = done;
      c->latency = latency;
      c->verdict = verdict;
      c->crashed = crashed;
    }
    c->cv.notify_all();
  }
  c->st.reset();  // stereoTool_Delete on this thread (it may hang after a crash: stop() does not wait for ever)
  {
    std::lock_guard lock(c->m);
    c->finished = true;
  }
  c->cv.notify_all();
}

}  // namespace

// ---- ProcessingWorker ----

ProcessingWorker::ProcessingWorker(const ProcessingSettings& s, std::string station_id)
    : station_id_(std::move(station_id)),
      in_(kQueueFrames),
      core_(std::make_shared<Core>()),
      output_tap_(s.output_tap) {
  Core& c = *core_;
  c.key = s.license_key;
  c.preset = s.preset_path;
  c.home = s.home_dir;
  c.web_port = s.web_port;
  c.station_id = station_id_;
  c.name = "station-" + station_id_;
  c.block_tap = s.st_block_tap;
  c.job.assign(kBlockFrames * 2, 0.0f);
  block_.assign(kBlockFrames * 2, 0.0f);
  out_.assign(kBlockFrames * 2, 0.0f);
  dry_.assign(size_t(kRingFrames) * 2, 0.0f);
  proc_.assign(size_t(kRingFrames) * 2, 0.0f);
  graveyard_.reserve(kGraveyardSlots);
  c.cmd = Cmd::Setup;  // the first thing the Stereo Tool thread does
  pending_ = Pending::Setup;
  st_thread_ = std::thread(st_main, core_);
  out_thread_ = std::thread([this] { run(); });
}

ProcessingWorker::~ProcessingWorker() { stop(); }

void ProcessingWorker::set_streams(std::vector<std::shared_ptr<StreamWorker>> streams) {
  auto list = std::make_shared<const std::vector<std::shared_ptr<StreamWorker>>>(std::move(streams));
  std::shared_ptr<const std::vector<std::shared_ptr<StreamWorker>>> unused;
  std::vector<std::shared_ptr<const void>> dead;
  dead.reserve(kGraveyardSlots);
  {
    std::lock_guard lock(streams_m_);
    unused = std::move(pending_streams_);  // a list the output thread never took
    pending_streams_ = std::move(list);
    graveyard_.swap(dead);  // reserved here, so the output thread's next retire never allocates
    has_pending_streams_.store(true, std::memory_order_release);
  }
  // `dead` and `unused` are released here, on the caller's thread
}

void ProcessingWorker::restart() { restart_.store(true); }

void ProcessingWorker::stop() {
  if (stopped_.exchange(true)) return;
  running_ = false;
  in_.wake();
  if (out_thread_.joinable()) out_thread_.join();
  {
    std::lock_guard lock(core_->m);
    core_->quit = true;
  }
  core_->cv.notify_all();
  const int configured = g_stop_timeout_ms.load();
  const int timeout_ms = configured > 0 ? configured : kDefaultStopTimeoutMs;
  bool finished;
  {
    std::unique_lock lock(core_->m);
    finished = core_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return core_->finished; });
  }
  if (finished) {
    st_thread_.join();
  } else {
    // stuck in a library call: the thread keeps the shared Core and ends when (if) the call returns
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.1f", timeout_ms / 1000.0);
    log_warning("Processing stacji " + station_id_ + ": Stereo Tool nie zakończył pracy w ciągu " + secs +
                " s; jego wątek zostaje do zamknięcia usługi");
    st_thread_.detach();
  }
  streams_.reset();  // what the output thread held, released on the caller's thread
  {
    std::lock_guard lock(streams_m_);
    pending_streams_.reset();
    graveyard_.clear();
  }
  set_state(ProcessingState::Off, Reason::None);
}

ProcessingStatus ProcessingWorker::status() const {
  ProcessingStatus s;
  s.state = ProcessingState(state_.load());
  const auto why = Reason(reason_.load());
  std::string failure, unlicensed;
  {
    std::lock_guard lock(core_->text_m);
    failure = core_->failure;
    unlicensed = core_->unlicensed;
  }
  switch (why) {
    case Reason::None: break;
    case Reason::Starting: s.reason = "uruchamianie Stereo Tool"; break;
    case Reason::LicenceCheck: s.reason = "sprawdzanie licencji Stereo Tool (do tego czasu sygnał bez processingu)"; break;
    case Reason::Setup: s.reason = failure; break;
    case Reason::Licence:
      s.reason = "licencja Stereo Tool nie obejmuje funkcji używanych w ustawieniach — sygnał bez processingu";
      break;
    case Reason::TooSlow: s.reason = "Stereo Tool nie nadąża (ponad 200 ms zaległości) — sygnał bez processingu"; break;
    case Reason::Process: s.reason = failure + " — sygnał bez processingu"; break;
    case Reason::Crash:
      s.reason = "Stereo Tool uległ awarii — uruchom ponownie usługę Bounce Encoder (" + failure + ")";
      break;
    case Reason::RestartWaits:
      s.reason = "Stereo Tool nie odpowiada — ponowne uruchomienie czeka, aż zakończy bieżące wywołanie";
      break;
  }
  s.latency_ms = delay_frames() * 1000.0 / kStationRate;
  const bool feeding = s.state == ProcessingState::Running || why == Reason::LicenceCheck;
  s.cpu_pct = feeding ? core_->cpu_pct.load() : 0.0;
  const int licence = core_->licence.load();
  s.license_valid = licence == 1;
  if (licence == 0) s.unlicensed_features = std::move(unlicensed);
  s.queue_dropped_frames = in_.dropped_frames();
  return s;
}

// ---- output thread ----

void ProcessingWorker::set_state(ProcessingState state, Reason why) {
  state_.store(int(state));
  reason_.store(int(why));
}

void ProcessingWorker::take_streams() {
  if (!has_pending_streams_.load(std::memory_order_acquire)) return;
  std::lock_guard lock(streams_m_);
  has_pending_streams_.store(false, std::memory_order_relaxed);
  if (streams_ && graveyard_.size() < graveyard_.capacity()) graveyard_.push_back(std::move(streams_));
  // else (more than kGraveyardSlots lists between two set_streams calls): released here after all
  streams_ = std::move(pending_streams_);
}

void ProcessingWorker::note_backlog(int64_t now) {
  if (in_.size_frames() == 0)
    behind_since_ = 0;
  else if (behind_since_ == 0)
    behind_since_ = now;
}

void ProcessingWorker::go_dry(Reason why, ProcessingState state) {
  // a crossfade back over kRampFrames: the processed ring holds that much beyond what was emitted
  if (mode_ == Mode::Processed) mix_.set_target(0.0);
  mode_ = Mode::Dry;
  licence_ok_ = false;
  set_state(state, why);
}

void ProcessingWorker::begin_restart() {
  restart_wanted_ = false;
  go_dry(Reason::Starting, ProcessingState::Bypass);
  {
    std::lock_guard lock(core_->m);
    core_->cmd = Cmd::Restart;
  }
  core_->cv.notify_all();
  pending_ = Pending::Setup;
}

bool ProcessingWorker::take_result(bool wait) {
  Done done;
  uint32_t latency;
  Verdict verdict;
  bool crashed;
  {
    std::unique_lock lock(core_->m);
    if (wait) core_->cv.wait_for(lock, kWaitSlice, [&] { return core_->done != Done::None; });
    if (core_->done == Done::None) return false;
    done = core_->done;
    core_->done = Done::None;
    latency = core_->latency;
    verdict = core_->verdict;
    crashed = core_->crashed;
  }
  pending_ = Pending::None;
  switch (done) {
    case Done::None: break;
    case Done::Ready: {
      // a new session: Stereo Tool is fed from the next input frame on; its output for frame f
      // comes back `latency` frames later. The delay never shrinks (that would skip audio).
      session_latency_ = latency;
      session_start_ = written_;
      proc_end_ = written_;
      if (latency + kRampFrames > delay_.load(std::memory_order_relaxed))
        delay_.store(latency + kRampFrames, std::memory_order_relaxed);
      mode_ = Mode::Probation;
      licence_ok_ = false;
      set_state(ProcessingState::Bypass, Reason::LicenceCheck);
      break;
    }
    case Done::SetupFailed:
      mode_ = Mode::Dry;
      set_state(ProcessingState::Error, Reason::Setup);
      break;
    case Done::Processed: {
      if (mode_ == Mode::Dry) break;  // a block that came back too late: the bypass took over
      const float* job = core_->job.data();  // the Stereo Tool thread is done with it
      for (size_t k = 0; k < job_frames_; ++k) {
        const int64_t f = job_start_ + int64_t(k) - int64_t(session_latency_);
        if (f < session_start_) continue;  // what Stereo Tool had in its delay before this session
        const size_t i = size_t(f & kRingMask) * 2;
        proc_[i] = job[k * 2];
        proc_[i + 1] = job[k * 2 + 1];
      }
      proc_end_ = std::max(proc_end_, job_start_ + int64_t(job_frames_) - int64_t(session_latency_));
      if (verdict == Verdict::Valid) licence_ok_ = true;
      if (verdict == Verdict::Invalid) go_dry(Reason::Licence, ProcessingState::Bypass);
      break;
    }
    case Done::ProcessFailed:
      if (crashed)
        go_dry(Reason::Crash, ProcessingState::Error);
      else
        go_dry(Reason::Process, ProcessingState::Bypass);
      break;
  }
  return true;
}

void ProcessingWorker::wait_job() {
  while (running_.load(std::memory_order_relaxed)) {
    if (take_result(true)) return;
    const int64_t now = platform::monotonic_ns();
    note_backlog(now);
    if (behind_since_ != 0 && now - behind_since_ > kMaxBehindNs) {
      // "kolejka > 200 ms": the block stays with Stereo Tool; its result, if any, is dropped
      go_dry(Reason::TooSlow, ProcessingState::Bypass);
      return;
    }
  }
}

void ProcessingWorker::deliver(const float* stereo, size_t frames) {
  if (output_tap_) output_tap_(stereo, frames);
  if (streams_)
    for (const auto& s : *streams_) s->push(stereo, frames);
}

void ProcessingWorker::emit() {
  // into processing once the licence check passed and the output has reached the session (the
  // processed ring holds nothing before it)
  if (mode_ == Mode::Probation && licence_ok_ && emitted_ >= session_start_) {
    mode_ = Mode::Processed;
    mix_.set_target(1.0);
    set_state(ProcessingState::Running, Reason::None);
  }
  const int64_t end = written_ - int64_t(delay_.load(std::memory_order_relaxed));
  while (emitted_ < end) {
    const size_t n = size_t(std::min<int64_t>(end - emitted_, int64_t(kBlockFrames)));
    float* o = out_.data();
    for (size_t k = 0; k < n; ++k) {
      const size_t i = size_t((emitted_ + int64_t(k)) & kRingMask) * 2;
      const double a = mix_.ramping() ? mix_.next() : mix_.level();
      if (a <= 0.0) {
        o[k * 2] = dry_[i];
        o[k * 2 + 1] = dry_[i + 1];
      } else if (a >= 1.0) {
        o[k * 2] = proc_[i];
        o[k * 2 + 1] = proc_[i + 1];
      } else {
        for (int c = 0; c < 2; ++c)
          o[k * 2 + c] = static_cast<float>(a * double(proc_[i + c]) + (1.0 - a) * double(dry_[i + c]));
      }
    }
    deliver(o, n);
    emitted_ += int64_t(n);
  }
}

void ProcessingWorker::run() {
  while (running_.load(std::memory_order_relaxed)) {
    take_streams();
    if (restart_.exchange(false)) restart_wanted_ = true;
    if (pending_ != Pending::None) take_result(false);
    if (restart_wanted_) {
      if (pending_ == Pending::None)
        begin_restart();
      else if (pending_ == Pending::Setup)
        restart_wanted_ = false;  // a (re)start is under way already
      else
        set_state(ProcessingState(state_.load()), Reason::RestartWaits);  // a block stuck in Stereo Tool
    }

    const size_t n = in_.pop(block_.data(), kBlockFrames, kPopWaitMs);
    const int64_t now = platform::monotonic_ns();
    if (n == 0) {
      behind_since_ = 0;
      continue;
    }
    for (size_t k = 0; k < n; ++k) {
      const size_t i = size_t((written_ + int64_t(k)) & kRingMask) * 2;
      dry_[i] = block_[k * 2];
      dry_[i + 1] = block_[k * 2 + 1];
    }
    const int64_t start = written_;
    written_ += int64_t(n);
    note_backlog(now);

    if (mode_ != Mode::Dry) {
      if (pending_ == Pending::None) {
        // the block buffer is the Stereo Tool thread's from the post until its result is taken
        std::copy(block_.begin(), block_.begin() + std::ptrdiff_t(n * 2), core_->job.begin());
        job_start_ = start;
        job_frames_ = n;
        {
          std::lock_guard lock(core_->m);
          core_->job_frames = n;
          core_->cmd = Cmd::Process;
        }
        core_->cv.notify_all();
        pending_ = Pending::Job;
        wait_job();
      } else {
        go_dry(Reason::TooSlow, ProcessingState::Bypass);  // not reached: every block is waited for
      }
    }
    emit();
  }
}

}  // namespace bam::encoder::detail
