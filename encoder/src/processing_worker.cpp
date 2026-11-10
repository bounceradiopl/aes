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

#include "aes67_output.hpp"
#include "bam/auth.hpp"  // random_hex: the web interface password
#include "bam/encoder/stereotool.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam::encoder::detail {

// Stereo Tool threads stop() gave up on that have not ended yet (stereotool_threads_left_behind).
std::atomic<int> g_threads_left{0};

namespace {

using PW = ProcessingWorker;

constexpr uint32_t kRingFrames = 1u << 15;  // dry and processed rings, indexed by input frame
constexpr int64_t kRingMask = kRingFrames - 1;
static_assert(kRingFrames >= PW::kMaxLatencyFrames + kRampFrames + 2 * PW::kBlockFrames,
              "the rings hold the longest delay and a block on either side");
constexpr int kPopWaitMs = 20;                         // the output thread looks at its flags this often
constexpr auto kWaitSlice = std::chrono::milliseconds(5);  // and at the queue while Stereo Tool works
constexpr int kDefaultStopTimeoutMs = 5000;
constexpr size_t kGraveyardSlots = 2;  // see ProcessingWorker::graveyard_
constexpr size_t kFailureTexts = 8;    // failure texts kept for the events (Core::failures)

std::atomic<int> g_stop_timeout_ms{0};
std::atomic<int> g_save_timeout_ms{0};  // tests
constexpr int kDefaultSaveTimeoutMs = 10000;

int stop_timeout_ms() {
  const int configured = g_stop_timeout_ms.load();
  return configured > 0 ? configured : kDefaultStopTimeoutMs;
}

// "10 s", "0,4 s" (Polish decimal comma)
std::string seconds_text(int ms) {
  char buf[32];
  if (ms % 1000 == 0)
    std::snprintf(buf, sizeof buf, "%d s", ms / 1000);
  else
    std::snprintf(buf, sizeof buf, "%d,%d s", ms / 1000, (ms % 1000) / 100);
  return buf;
}

enum class Cmd { None, Setup, Restart, Process };
enum class Done { None, Ready, SetupFailed, Processed, ProcessFailed };
enum class Verdict { None, Valid, Invalid };

// Text that may come from the library must not carry the licence key (nor its inner text).
std::string without_key(std::string text, const std::string& key) { return without_stereotool_key(std::move(text), key); }

constexpr size_t kWebPasswordBytes = 10;  // 20 hex digits

// A save_preset() call waiting for the Stereo Tool thread (shared: the caller may give up first).
struct SaveRequest {
  std::string path;
  bool done = false;  // under Core::m
  bool ok = false;
  std::string error;
};

}  // namespace

namespace testing {
void set_processing_stop_timeout_ms(int ms) { g_stop_timeout_ms.store(ms); }
void set_save_preset_timeout_ms(int ms) { g_save_timeout_ms.store(ms); }
}  // namespace testing

int save_preset_timeout_ms() {
  const int configured = g_save_timeout_ms.load();
  return configured > 0 ? configured : kDefaultSaveTimeoutMs;
}

struct ProcessingWorker::Core {
  // fixed
  std::string key, preset, home, name, station_id, setup_error;
  int web_port = 0;
  std::string web_password;  // Stereo Tool's web interface password, the same for every instance of the worker
  std::function<void()> block_tap;
  // set when the Stereo Tool thread has let go of the instance for good (instance_gone())
  const std::shared_ptr<std::atomic<bool>> gone = std::make_shared<std::atomic<bool>>(false);
  // Stereo Tool thread only: instances to wait for before the first create (ProcessingSettings)
  std::vector<std::shared_ptr<const std::atomic<bool>>> wait_for_gone;

  // the mailbox, under m
  std::mutex m;
  std::condition_variable cv;  // both ways: a command posted, a command done, the thread ended
  Cmd cmd = Cmd::None;
  Done done = Done::None;
  bool quit = false;
  bool finished = false;
  bool left_behind = false;  // stop() gave up waiting for this thread (counted in g_threads_left)
  std::vector<float> job;  // kBlockFrames stereo, processed in place
  size_t job_frames = 0;
  uint32_t latency = 0;              // Ready
  Verdict verdict = Verdict::None;   // Processed: a licence check came with this block
  bool crashed = false;              // ProcessFailed: a crash (nothing more until a service restart)
  uint64_t failure_id = 0;           // SetupFailed / ProcessFailed: the failure's text (failures)
  std::string requested_preset;      // set_preset(): loaded by the next (re)start
  bool preset_change = false;
  std::shared_ptr<SaveRequest> save;  // save_preset(): done before the next command

  // published for status()
  std::atomic<double> cpu_pct{0};
  std::atomic<int> licence{-1};  // the last check: -1 none yet, 0 invalid, 1 valid
  std::atomic<int> web_port_active{0};  // the instance is up with its web interface on this port
  std::atomic<bool> has_instance{false};  // the Stereo Tool thread holds an instance
  std::atomic<bool> side_busy{false};   // a save is under way (the output thread waits without bypassing)
  std::atomic<bool> keep_instance{false};  // keep_instance_at_stop(): no stereoTool_Delete when it ends
  // the save under way was given up by its caller after this many ms (0: not given up / none)
  std::atomic<int> save_overdue_ms{0};
  mutable std::mutex text_m;
  // Polish, the last kFailureTexts failures (no key), by id (1, 2, ...): failure id f is
  // failures[(f - 1) % kFailureTexts] while f > failure_count - kFailureTexts
  std::array<std::string, kFailureTexts> failures;
  uint64_t failure_count = 0;
  std::string unlicensed;  // the features the last check found unlicensed
  std::string station_name;  // for the log (set_station_name()); empty: station_id

  // Stereo Tool thread only
  std::unique_ptr<StereoToolInstance> st;
  uint64_t session_frames = 0;  // processed since the last (re)start
  uint64_t next_check = 0;      // session_frames of the next licence check
  int64_t cpu_ns = 0;
  uint64_t cpu_frames = 0;
};

namespace {

using Core = ProcessingWorker::Core;

// Records the failure text; returns its id (the output thread puts it into the state it sets,
// whose event the engine logs with it).
uint64_t set_failure(Core& c, std::string text) {
  text = without_key(std::move(text), c.key);
  std::lock_guard lock(c.text_m);
  c.failures[c.failure_count % kFailureTexts] = std::move(text);
  return ++c.failure_count;
}

// "Processing stacji „<name>”: " for the worker's log lines, like the engine's (station id when
// the name is empty).
std::string log_who(const Core& c) {
  std::lock_guard lock(c.text_m);
  return "Processing stacji „" + (c.station_name.empty() ? c.station_id : c.station_name) + "”: ";
}

uint64_t last_failure(const Core& c) {
  std::lock_guard lock(c.text_m);
  return c.failure_count;
}

// Lets go of the instance (deleted, or left behind when it crashed) and its web interface.
void drop_instance(Core& c) {
  c.web_port_active.store(0);
  c.st.reset();
  c.has_instance.store(false);
}

// Builds the instance (or keeps a working one on restart, loading a new preset into it when one
// was set), loads the preset, measures the latency.
// A preset set with set_preset() since the last setup: it becomes the one to load. Taken right
// before a load (after Create3, which may take long), so a preset set meanwhile is not missed.
bool take_requested_preset(Core& c) {
  std::lock_guard lock(c.m);
  if (!c.preset_change) return false;
  c.preset = std::move(c.requested_preset);
  c.preset_change = false;
  return true;
}

// Another worker's instance on the same web interface port, or in the same home folder, goes first
// (ProcessingSettings::wait_for_gone): bounded, and given up when this worker stops.
void wait_for_port(Core& c) {
  if (c.wait_for_gone.empty()) return;
  const int64_t until = platform::monotonic_ns() + (int64_t(stop_timeout_ms()) + 1000) * 1'000'000;
  for (const auto& gone : c.wait_for_gone) {
    while (!gone->load() && platform::monotonic_ns() < until) {
      std::unique_lock lock(c.m);
      if (c.cv.wait_for(lock, kWaitSlice, [&] { return c.quit; })) return;
    }
  }
  c.wait_for_gone.clear();  // once: later instances of this worker are its own
}

Done setup(Core& c, bool restart, uint32_t* latency) {
  if (!c.setup_error.empty()) {
    set_failure(c, c.setup_error);
    return Done::SetupFailed;
  }
  c.licence.store(-1);
  c.cpu_pct.store(0);
  c.cpu_ns = 0;
  c.cpu_frames = 0;
  {
    std::lock_guard lock(c.text_m);
    c.unlicensed.clear();
  }
  if (!restart || !c.st || c.st->failed()) {
    drop_instance(c);  // a failed one goes first: its memory and web interface port
    if (restart) stereotool_reload();  // a library put in place since (nothing after a crash)
    wait_for_port(c);  // the station's old instance (its home folder) and its web port, if any
    std::string err;
    auto st = StereoToolInstance::create(c.key, c.name, c.home, &err);
    if (!st) {
      set_failure(c, err);
      return Done::SetupFailed;
    }
    bool web = false;
    if (c.web_port > 0) {
      web = st->set_web_interface(true, c.web_port, c.web_password);
      if (!web && !st->failed())
        log_warning(log_who(c) + "Stereo Tool nie przyjął portu interfejsu WWW " + std::to_string(c.web_port) +
                    " albo jego hasła (interfejs WWW wyłączony)");
    }
    take_requested_preset(c);
    if (!st->failed() && !c.preset.empty() && !st->load_preset(c.preset, &err)) {
      set_failure(c, err);
      return Done::SetupFailed;  // the instance is deleted here: no ~600 MB kept for nothing
    }
    if (st->failed()) {
      set_failure(c, st->failure());
      return Done::SetupFailed;
    }
    c.st = std::move(st);
    c.has_instance.store(true);
    c.web_port_active.store(web ? c.web_port : 0);
  } else if (take_requested_preset(c)) {
    std::string err;
    if (!c.st->load_preset(c.preset, &err)) {
      set_failure(c, err);
      drop_instance(c);  // as on a first load: a new one is built by the next restart
      return Done::SetupFailed;
    }
  }
  // feeds silence through the chain: only here, never on live audio
  const int frames = c.st->latency_frames();
  if (c.st->failed()) {
    set_failure(c, c.st->failure());
    drop_instance(c);
    return Done::SetupFailed;
  }
  if (frames > int(PW::kMaxLatencyFrames)) {
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.0f", frames * 1000.0 / kStationRate);
    set_failure(c, std::string("opóźnienie Stereo Tool ") + ms + " ms jest większe niż dopuszczalne " +
                       std::to_string(PW::kMaxLatencyFrames * 1000 / kStationRate) + " ms");
    drop_instance(c);
    return Done::SetupFailed;
  }
  c.session_frames = 0;
  c.next_check = PW::kLicenceFirstCheckFrames;
  *latency = uint32_t(frames);
  return Done::Ready;
}

Done process_failed(Core& c, bool* crashed) {
  *crashed = c.st->abandoned();  // this instance crashed (not another station's)
  set_failure(c, c.st->failure());
  drop_instance(c);  // after a C++ exception deleted now (memory, web port); after a crash abandoned
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
      log_warning(log_who(c) + "licencja Stereo Tool nie obejmuje funkcji: " +
                  (features.empty() ? std::string("(Stereo Tool nie podał listy)") : features));
      std::lock_guard lock(c.text_m);
      c.unlicensed = std::move(features);
    }
  }
  return Done::Processed;
}

// save_preset(): the instance's settings to the requested file, between two commands.
void save(Core& c, SaveRequest& r) {
  c.side_busy.store(true);
  std::string err;
  bool ok = false;
  if (!c.st || c.st->failed()) {
    err = "Stereo Tool nie działa na tej stacji (brak instancji)";
  } else {
    ok = c.st->save_preset(r.path, &err);
    // the binding names the temporary file; an exception's text is kept (without the key)
    err = ok ? std::string() : c.st->failed() ? without_key(c.st->failure(), c.key) : "Stereo Tool nie zapisał presetu";
  }
  c.side_busy.store(false);
  {
    std::lock_guard lock(c.m);
    r.ok = ok;
    r.error = std::move(err);
    r.done = true;
    c.save_overdue_ms.store(0);  // under the lock save_preset() gives up under: never left set
  }
  c.cv.notify_all();
}

void st_main(std::shared_ptr<Core> c) {
  for (;;) {
    Cmd cmd;
    std::shared_ptr<SaveRequest> save_request;
    {
      std::unique_lock lock(c->m);
      c->cv.wait(lock, [&] { return c->quit || c->cmd != Cmd::None || c->save; });
      if (c->quit) break;
      cmd = c->cmd;
      c->cmd = Cmd::None;
      save_request = std::move(c->save);
    }
    if (save_request) save(*c, *save_request);
    if (cmd == Cmd::None) continue;
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
    // the text set_failure recorded for this command (this thread is the only one recording)
    const uint64_t failure_id = done == Done::SetupFailed || done == Done::ProcessFailed ? last_failure(*c) : 0;
    {
      std::lock_guard lock(c->m);
      c->done = done;
      c->latency = latency;
      c->verdict = verdict;
      c->crashed = crashed;
      c->failure_id = failure_id;
    }
    c->cv.notify_all();
  }
  const bool keep = c->keep_instance.load() && c->st;
  if (keep) c->st->leave_to_process_exit();  // ruling S7: the end of the process frees it
  drop_instance(*c);  // else stereoTool_Delete on this thread (it may hang after a crash: stop() does not wait for ever)
  if (!keep) c->gone->store(true);  // its web interface port is free for the next instance
  {
    std::lock_guard lock(c->m);
    c->finished = true;
    if (c->left_behind) --g_threads_left;  // it did end after all
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
  // before the output thread exists: its first block already has somewhere to go
  streams_ = std::make_shared<const std::vector<std::shared_ptr<StreamWorker>>>(s.streams);
  Core& c = *core_;
  c.key = s.license_key;
  c.preset = s.preset_path;
  c.home = s.home_dir;
  c.web_port = s.web_port;
  c.web_password = random_hex(kWebPasswordBytes);
  c.setup_error = s.setup_error;
  c.wait_for_gone = s.wait_for_gone;
  c.station_id = station_id_;
  c.station_name = s.station_name;
  c.name = "station-" + station_id_;
  c.block_tap = s.st_block_tap;
  c.job.assign(kBlockFrames * 2, 0.0f);
  block_.assign(kBlockFrames * 2, 0.0f);
  out_.assign(kBlockFrames * 2, 0.0f);
  dry_.assign(size_t(kRingFrames) * 2, 0.0f);
  proc_.assign(size_t(kRingFrames) * 2, 0.0f);
  at_.assign(kRingFrames, 0);
  graveyard_.reserve(kGraveyardSlots);
  c.cmd = Cmd::Setup;  // the first thing the Stereo Tool thread does
  pending_ = Pending::Setup;
  set_state(ProcessingState::Bypass, Reason::Starting);
  st_thread_ = std::thread(st_main, core_);
  out_thread_ = std::thread([this] { run(); });
}

ProcessingWorker::~ProcessingWorker() { stop(); }

int ProcessingWorker::configured_web_port() const { return core_->web_port; }

const std::string& ProcessingWorker::web_password() const { return core_->web_password; }

bool ProcessingWorker::has_instance() const { return core_->has_instance.load(); }

std::shared_ptr<const std::atomic<bool>> ProcessingWorker::instance_gone() const { return core_->gone; }

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
    has_pending_outputs_.store(true, std::memory_order_release);
  }
  // `dead` and `unused` are released here, on the caller's thread
}

void ProcessingWorker::set_aes67(std::shared_ptr<Aes67Output> out) {
  std::shared_ptr<Aes67Output> unused;
  std::vector<std::shared_ptr<const void>> dead;
  dead.reserve(kGraveyardSlots);
  {
    std::lock_guard lock(streams_m_);
    unused = std::move(pending_aes67_);  // one the output thread never took
    pending_aes67_ = std::move(out);
    aes67_pending_ = true;
    graveyard_.swap(dead);
    has_pending_outputs_.store(true, std::memory_order_release);
  }
}

void ProcessingWorker::restart() { restart_.store(true); }

void ProcessingWorker::set_preset(std::string sts_path) {
  {
    std::lock_guard lock(core_->m);
    core_->requested_preset = std::move(sts_path);
    core_->preset_change = true;
  }
  preset_change_.store(true);
}

void ProcessingWorker::keep_instance_at_stop() { core_->keep_instance.store(true); }

void ProcessingWorker::set_station_name(std::string name) {
  std::lock_guard lock(core_->text_m);
  core_->station_name = std::move(name);
}

bool ProcessingWorker::save_preset(const std::string& sts_path, int timeout_ms, std::string* error, bool* busy) {
  if (busy) *busy = false;
  auto request = std::make_shared<SaveRequest>();
  request->path = sts_path;
  std::unique_lock lock(core_->m);
  std::string why;
  if (core_->quit || core_->finished) {
    why = "processing tej stacji jest zatrzymany";
  } else if (core_->save || core_->side_busy.load()) {
    why = "zapis presetu tej stacji już trwa";
    if (busy) *busy = true;
  } else {
    core_->save = request;
    core_->cv.notify_all();
    core_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return request->done || core_->finished; });
    if (request->done) {
      if (request->ok) return true;
      why = request->error;
    } else if (core_->save == request) {
      core_->save.reset();  // not taken yet: it never will be
      why = "Stereo Tool nie odpowiada — preset nie został zapisany";
    } else {
      // taken and still under way: whatever it writes is not a preset (removed at the next start);
      // the status says so until it returns (reason_text)
      core_->save_overdue_ms.store(timeout_ms);
      why = "Stereo Tool nie zakończył zapisu presetu w ciągu " + seconds_text(timeout_ms) +
            " — preset nie został zapisany; do końca zapisu sygnał bez processingu";
    }
  }
  if (error) *error = why;
  return false;
}

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
  const int timeout_ms = stop_timeout_ms();
  bool finished;
  {
    std::unique_lock lock(core_->m);
    finished = core_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return core_->finished; });
    if (!finished) {
      core_->left_behind = true;  // under the lock the thread sets finished under
      ++g_threads_left;
    }
  }
  if (finished) {
    st_thread_.join();
  } else {
    // stuck in a library call: the thread keeps the shared Core and ends when (if) the call returns
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.1f", timeout_ms / 1000.0);
    log_warning(log_who(*core_) + "Stereo Tool nie zakończył pracy w ciągu " + secs +
                " s; jego wątek zostaje do zamknięcia usługi");
    st_thread_.detach();
  }
  streams_.reset();  // what the output thread held, released on the caller's thread
  aes67_.reset();
  {
    std::lock_guard lock(streams_m_);
    pending_streams_.reset();
    pending_aes67_.reset();
    graveyard_.clear();
  }
  set_state(ProcessingState::Off, Reason::None);
}

namespace {
constexpr int kStateShift = 56, kReasonShift = 48;
constexpr uint64_t kFailureMask = (uint64_t(1) << kReasonShift) - 1;
}  // namespace

std::string ProcessingWorker::reason_text(Reason why, uint64_t failure_id) const {
  std::string failure;
  if (why == Reason::Setup || why == Reason::Process || why == Reason::Crash) {
    std::lock_guard lock(core_->text_m);
    const uint64_t count = core_->failure_count;
    // the text of that failure while it is kept, else the newest one
    const uint64_t id = failure_id > 0 && failure_id <= count && failure_id + kFailureTexts > count ? failure_id : count;
    if (id > 0) failure = core_->failures[(id - 1) % kFailureTexts];
  }
  switch (why) {
    case Reason::None: break;
    case Reason::Starting: return "uruchamianie Stereo Tool";
    case Reason::LicenceCheck: return "sprawdzanie licencji Stereo Tool (do tego czasu sygnał bez processingu)";
    case Reason::Setup: return failure;
    case Reason::Licence:
      return "licencja Stereo Tool nie obejmuje funkcji używanych w ustawieniach — sygnał bez processingu";
    case Reason::TooSlow: return "Stereo Tool nie nadąża (ponad 200 ms zaległości) — sygnał bez processingu";
    case Reason::Process: return failure + " — sygnał bez processingu";
    case Reason::Crash: return "Stereo Tool uległ awarii — uruchom ponownie usługę Bounce Encoder (" + failure + ")";
    case Reason::RestartWaits:
      return "Stereo Tool nie odpowiada — ponowne uruchomienie czeka, aż zakończy bieżące wywołanie";
    case Reason::Saving: {
      if (const int overdue_ms = core_->save_overdue_ms.load(); overdue_ms > 0)
        return "Stereo Tool nie zakończył zapisu presetu w ciągu " + seconds_text(overdue_ms) +
               " (nie odpowiada) — do końca zapisu sygnał bez processingu";
      return "zapis presetu Stereo Tool (do jego końca sygnał bez processingu)";
    }
    case Reason::PreviousCrash: return kPreviousCrashReason;  // the engine's (a held station has no worker)
  }
  return {};
}

ProcessingStatus ProcessingWorker::status() const {
  ProcessingStatus s;
  const uint64_t packed = status_.load();
  s.state = ProcessingState(packed >> kStateShift);
  const auto why = Reason((packed >> kReasonShift) & 0xFF);
  s.reason_code = why;
  s.reason = reason_text(why, packed & kFailureMask);
  s.web_port = core_->web_port_active.load();
  s.latency_ms = delay_frames() * 1000.0 / kStationRate;
  const bool feeding = s.state == ProcessingState::Running || why == Reason::LicenceCheck;
  s.cpu_pct = feeding ? core_->cpu_pct.load() : 0.0;
  const int licence = core_->licence.load();
  s.license_valid = licence == 1;
  if (licence == 0) {
    std::lock_guard lock(core_->text_m);
    s.unlicensed_features = core_->unlicensed;
  }
  s.queue_dropped_frames = in_.dropped_frames();
  return s;
}

uint64_t ProcessingWorker::events_since(uint64_t since, std::vector<ProcessingEvent>* out) const {
  std::vector<std::pair<uint64_t, uint64_t>> raw;
  const uint64_t next = events_.read(since, out ? &raw : nullptr);
  for (const auto& [seq, packed] : raw) {
    ProcessingEvent e;
    e.seq = seq;
    e.state = ProcessingState(packed >> kStateShift);
    e.reason_code = Reason((packed >> kReasonShift) & 0xFF);
    e.reason = reason_text(e.reason_code, packed & kFailureMask);
    out->push_back(std::move(e));
  }
  return next;
}

// ---- EventRing ----

void EventRing::push(uint64_t value) {
  const uint64_t seq = seq_.load(std::memory_order_relaxed);
  // a reader that sees this value in the slot then also sees seq_ >= seq (its acquire fence pairs
  // with this one), so it knows the event the slot held before is gone
  std::atomic_thread_fence(std::memory_order_release);
  slots_[seq % kSlots].store(value, std::memory_order_relaxed);
  seq_.store(seq + 1, std::memory_order_release);
}

uint64_t EventRing::read(uint64_t since, std::vector<std::pair<uint64_t, uint64_t>>* out,
                         const std::function<void()>& between) const {
  const uint64_t seq = seq_.load(std::memory_order_acquire);
  uint64_t from = std::min(since, seq);
  if (seq - from > kSlots) from = seq - kSlots;
  std::array<uint64_t, kSlots> values;
  for (uint64_t i = from; i < seq; ++i) values[i - from] = slots_[i % kSlots].load(std::memory_order_relaxed);
  if (between) between();
  // Event i is overwritten by event i + kSlots: intact while seq_ (the events written) is below
  // i + kSlots, since the writer may be storing event seq_ right now (it publishes seq_ after).
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint64_t now = seq_.load(std::memory_order_relaxed);
  if (out)
    for (uint64_t i = from; i < seq; ++i)
      if (i + kSlots > now) out->emplace_back(i + 1, values[i - from]);
  return seq;
}

// ---- output thread ----

void ProcessingWorker::set_state(ProcessingState state, Reason why) {
  const bool failure = why == Reason::Setup || why == Reason::Process || why == Reason::Crash;
  const uint64_t packed = uint64_t(state) << kStateShift | uint64_t(why) << kReasonShift |
                          (failure ? failure_id_ & kFailureMask : 0);
  if (status_.load(std::memory_order_relaxed) == packed) return;
  status_.store(packed);
  events_.push(packed);  // one writer at a time (the output thread; the constructor before it, stop() after it)
}

void ProcessingWorker::take_outputs() {
  if (!has_pending_outputs_.load(std::memory_order_acquire)) return;
  std::lock_guard lock(streams_m_);
  has_pending_outputs_.store(false, std::memory_order_relaxed);
  // at most one take of each per set_streams()/set_aes67(), so the slots reserved there are free
  // (else released here after all)
  if (pending_streams_) {
    if (streams_ && graveyard_.size() < graveyard_.capacity()) graveyard_.push_back(std::move(streams_));
    streams_ = std::move(pending_streams_);
  }
  if (aes67_pending_) {
    aes67_pending_ = false;
    if (aes67_ && graveyard_.size() < graveyard_.capacity()) graveyard_.push_back(std::move(aes67_));
    aes67_ = std::move(pending_aes67_);
  }
}

bool ProcessingWorker::falling_behind(int64_t now) {
  if (pending_ == Pending::Job && now - job_posted_ns_ > kMaxBehindNs) return true;  // stuck in a call
  const size_t backlog = in_.size_frames();
  // the queue would drop audio soon, unless Stereo Tool is draining it (a burst after a stall)
  if (backlog < kMaxBacklogFrames) {
    cap_ns_ = 0;
  } else if (cap_ns_ == 0) {
    cap_ns_ = now;
    cap_backlog_ = backlog;
  } else if (now - cap_ns_ >= kCapWindowNs) {
    if (backlog >= cap_backlog_) return true;
    cap_ns_ = now;
    cap_backlog_ = backlog;
  }
  if (backlog <= kBehindFrames) {
    window_ns_ = 0;
    return false;
  }
  // more than 200 ms waiting: slower than real time only if it has not shrunk over the window (a
  // burst after a station stall shrinks while Stereo Tool drains it)
  if (window_ns_ == 0) {
    window_ns_ = now;
    window_backlog_ = backlog;
    return false;
  }
  if (now - window_ns_ < kBehindWindowNs) return false;
  if (backlog >= window_backlog_) return true;
  window_ns_ = now;
  window_backlog_ = backlog;
  return false;
}

void ProcessingWorker::go_dry(Reason why, ProcessingState state) {
  resume_after_save_ = false;
  // a crossfade back over kRampFrames: the processed ring holds that much beyond what was emitted
  if (mode_ == Mode::Processed) mix_.set_target(0.0);
  mode_ = Mode::Dry;
  licence_ok_ = false;
  window_ns_ = 0;
  cap_ns_ = 0;
  set_state(state, why);
}

bool ProcessingWorker::preset_pending() const {
  std::lock_guard lock(core_->m);
  return core_->preset_change;
}

// Back into processing after a save that took the output into the bypass: the instance and its
// licence check are still good, only the blocks of the save went past it. A new session starts at
// the next input frame (what Stereo Tool still holds from before is not used); no restart, no
// setup, no new licence check unless none had passed yet.
void ProcessingWorker::resume_session() {
  resume_after_save_ = false;
  if (mode_ != Mode::Dry || ProcessingReason((status_.load() >> kReasonShift) & 0xFF) != Reason::Saving) return;
  session_start_ = written_;
  proc_end_ = written_;
  mode_ = Mode::Probation;
  licence_ok_ = core_->licence.load() == 1;
  if (!licence_ok_) set_state(ProcessingState::Bypass, Reason::LicenceCheck);
}

void ProcessingWorker::begin_restart() {
  restart_wanted_ = false;
  reload_wanted_ = false;  // the Stereo Tool thread takes the preset set last
  resume_after_save_ = false;
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
    if (core_->failure_id) failure_id_ = core_->failure_id;
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
  int64_t save_from = 0;  // the Stereo Tool thread is saving a preset since (0: not)
  while (running_.load(std::memory_order_relaxed)) {
    const bool done = take_result(true);
    if (mode_ == Mode::Dry) return;  // the result ended processing
    const int64_t now = platform::monotonic_ns();
    if (core_->side_busy.load(std::memory_order_relaxed)) {
      // a save before this block: not "too slow"; past kMaxBehindNs the unprocessed signal goes on
      // the air until the save is over (resume_session), and being stuck and the backlog window
      // count from its end
      if (save_from == 0) save_from = now;
      if (now - save_from > kMaxBehindNs) {
        go_dry(Reason::Saving, ProcessingState::Bypass);
        resume_after_save_ = true;
        return;
      }
      job_posted_ns_ = now;
      window_ns_ = 0;
      if (done) return;
      continue;
    }
    if (falling_behind(now)) {
      // "kolejka > 200 ms": a block still at Stereo Tool stays there; its result is dropped
      go_dry(Reason::TooSlow, ProcessingState::Bypass);
      return;
    }
    if (done) return;
  }
}

void ProcessingWorker::deliver(const float* stereo, size_t frames, uint64_t pos) {
  if (output_tap_) output_tap_(stereo, frames, pos);
  if (streams_)
    for (const auto& s : *streams_) s->push(stereo, frames);
  if (aes67_) aes67_->write(stereo, frames, pos);
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
  // Faded out (fade_out()): nothing more for the streams nor the AES67 output. faded_out() is
  // stored (release) after the last delivery, and the engine sees it (or stops this thread)
  // before the station writes the AES67 output itself: one writer at a time.
  if (faded_) {
    emitted_ = std::max(emitted_, end);
    return;
  }
  if (!fading_ && fade_wanted_.load(std::memory_order_relaxed)) {
    fading_ = true;
    fade_.set_target(0.0);
  }
  while (emitted_ < end) {
    const size_t n = size_t(std::min<int64_t>(end - emitted_, int64_t(kBlockFrames)));
    float* o = out_.data();
    size_t out_n = n;
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
      if (fading_) {
        const double g = fade_.next();
        o[k * 2] = static_cast<float>(g * double(o[k * 2]));
        o[k * 2 + 1] = static_cast<float>(g * double(o[k * 2 + 1]));
        if (!fade_.ramping()) {  // silent from here: the rest of the block is not handed over
          out_n = k + 1;
          break;
        }
      }
    }
    // in runs of contiguous positions (a jump in the station's position splits a block)
    for (size_t from = 0, k = 1; k <= out_n; ++k) {
      const uint64_t first = at_[size_t((emitted_ + int64_t(from)) & kRingMask)];
      if (k < out_n && at_[size_t((emitted_ + int64_t(k)) & kRingMask)] == first + (k - from)) continue;
      deliver(o + from * 2, k - from, first);
      from = k;
    }
    emitted_ += int64_t(n);
    if (fading_ && !fade_.ramping()) {
      fading_ = false;
      faded_ = true;
      faded_out_.store(true, std::memory_order_release);
      emitted_ = std::max(emitted_, end);
      return;
    }
  }
}

void ProcessingWorker::run() {
  while (running_.load(std::memory_order_relaxed)) {
    take_outputs();
    if (restart_.exchange(false)) restart_wanted_ = true;
    if (preset_change_.exchange(false)) reload_wanted_ = true;
    if (pending_ != Pending::None) take_result(false);
    if (resume_after_save_ && pending_ == Pending::None && !core_->side_busy.load(std::memory_order_relaxed))
      resume_session();
    if (restart_wanted_ || reload_wanted_) {
      if (pending_ == Pending::None) {
        if (restart_wanted_ || preset_pending())
          begin_restart();
        else
          reload_wanted_ = false;  // the setup that just ran loaded it already
      } else if (pending_ == Pending::Setup) {
        restart_wanted_ = false;  // a (re)start is under way already; a new preset waits for its end
      } else
        set_state(ProcessingState(status_.load() >> kStateShift), Reason::RestartWaits);  // a block stuck in Stereo Tool
    }

    uint64_t pos = 0;
    const size_t n = in_.pop(block_.data(), kBlockFrames, kPopWaitMs, &pos);
    if (n == 0) continue;
    take_outputs();  // outputs set while it waited go with this block already
    for (size_t k = 0; k < n; ++k) {
      const size_t f = size_t((written_ + int64_t(k)) & kRingMask);
      dry_[f * 2] = block_[k * 2];
      dry_[f * 2 + 1] = block_[k * 2 + 1];
      at_[f] = pos + k;
    }
    const int64_t start = written_;
    written_ += int64_t(n);

    // Probation and Processed always have the Stereo Tool thread idle here: every block is waited
    // for, and any way out of waiting (failure, too slow, restart) goes to Dry first
    if (mode_ != Mode::Dry && pending_ == Pending::None) {
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
      job_posted_ns_ = platform::monotonic_ns();
      wait_job();
    }
    emit();
  }
}

}  // namespace bam::encoder::detail

namespace bam::encoder {
int stereotool_threads_left_behind() { return detail::g_threads_left.load(); }
}  // namespace bam::encoder
