// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder engine: owns the network thread, the PTP slave, the stations and their stream
// workers, and rebuilds only what a new configuration changes (see engine.hpp).
#include "bam/encoder/engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "bam/config.hpp"  // ptime_us_to_frames
#include "bam/gain.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "bam/sdp.hpp"
#include "engine_internal.hpp"

namespace bam::encoder {

using detail::InputRx;
using detail::ProcessingEvent;
using detail::ProcessingSettings;
using detail::ProcessingWorker;
using detail::PtpClient;
using detail::RxThread;
using detail::Station;
using detail::StationPlan;
using detail::StreamWorker;

namespace {

constexpr int64_t kTaiOffsetNs = 37LL * 1'000'000'000;  // PTP timescale is TAI
constexpr uint32_t kDefaultLatencyFrames = 240;         // a station without inputs: 5 ms
constexpr int64_t kHousekeepingNs = 100'000'000;        // housekeeping thread period
// A removed input keeps receiving this long, so the station can fade it out on real audio.
constexpr int64_t kLingerNs = 500'000'000;
constexpr int64_t kInterfaceCheckNs = 2'000'000'000;  // the automatic interface, looked up again

// Stops the workers side by side: each IcecastClient::stop() may take a few hundred ms, a
// processing worker up to its Stereo Tool stop timeout (5 s) when Stereo Tool hangs.
template <typename Worker>
void stop_workers(std::vector<std::shared_ptr<Worker>> workers) {
  if (workers.empty()) return;
  if (workers.size() == 1) {
    workers[0]->stop();
    return;
  }
  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  for (auto& w : workers) threads.emplace_back([w] { w->stop(); });
  for (auto& t : threads) t.join();
}

// ---- processing ----

constexpr int64_t kFadeOutWaitNs = 500'000'000;  // an old processing worker's 5 ms fade out, at most
constexpr int kSavePresetTimeoutMs = 10000;
// Ruling S1: a Stereo Tool that fell behind is retried after these waits (the last one repeats),
// and the count starts again after kRetryResetNs of processing.
constexpr std::array<int64_t, 4> kRetryWaitsNs = {30'000'000'000LL, 120'000'000'000LL, 300'000'000'000LL, 600'000'000'000LL};
constexpr int64_t kRetryResetNs = 600'000'000'000LL;
std::mutex g_retry_m;
std::vector<int64_t> g_retry_waits_ns;  // tests (empty: kRetryWaitsNs)
int64_t g_retry_reset_ns = 0;           // tests (0: kRetryResetNs)

int64_t retry_wait_ns(int retries) {
  std::lock_guard lock(g_retry_m);
  const auto& waits = g_retry_waits_ns;
  if (!waits.empty()) return waits[std::min<size_t>(size_t(retries), waits.size() - 1)];
  return kRetryWaitsNs[std::min<size_t>(size_t(retries), kRetryWaitsNs.size() - 1)];
}

int64_t retry_reset_ns() {
  std::lock_guard lock(g_retry_m);
  return g_retry_reset_ns > 0 ? g_retry_reset_ns : kRetryResetNs;
}

// "30 s", "2 min"
std::string wait_text(int64_t ns) {
  const int64_t s = (ns + 999'999'999) / 1'000'000'000;
  if (s >= 60 && s % 60 == 0) return std::to_string(s / 60) + " min";
  if (ns < 1'000'000'000) return std::to_string(ns / 1'000'000) + " ms";
  return std::to_string(s) + " s";
}

std::string ms_text(double ms) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f", ms);
  return buf;
}

// "Processing stacji „Radio X”: " (never the key: the reasons come without it)
std::string processing_who(const std::string& station_name) { return "Processing stacji „" + station_name + "”: "; }

void log_event(const std::string& who, const ProcessingEvent& e, uint32_t delay_frames) {
  using R = ProcessingReason;
  switch (e.state) {
    case ProcessingState::Running:
      log_info(who + "Stereo Tool przetwarza sygnał (opóźnienie " + ms_text(delay_frames * 1000.0 / kStationRate) + " ms)");
      break;
    case ProcessingState::Bypass:
      if (e.reason_code == R::Starting || e.reason_code == R::LicenceCheck || e.reason_code == R::RestartWaits)
        log_info(who + e.reason);
      else
        log_warning(who + e.reason);
      break;
    case ProcessingState::Error:
      if (e.reason_code == R::Crash)
        log_error(who + e.reason);
      else
        log_warning(who + "błąd: " + e.reason + " (sygnał bez processingu)");
      break;
    case ProcessingState::Off: break;  // the engine logs why it stopped
  }
}

}  // namespace

namespace detail::testing {
void set_processing_retry_ms(std::vector<int> waits_ms, int reset_ms) {
  std::lock_guard lock(g_retry_m);
  g_retry_waits_ns.clear();
  for (int ms : waits_ms) g_retry_waits_ns.push_back(int64_t(ms) * 1'000'000);
  g_retry_reset_ns = int64_t(reset_ms) * 1'000'000;
}
}  // namespace detail::testing

// One enabled input of a station. Its identity is its key (SDP | interface): the receiver and
// the delay line live as long as an input with that key stays in the station, at whatever
// position, through gain, delay and latency edits; another key is another input (the old one
// fades out, the new one fades in with an empty delay line).
struct InputEntry {
  std::string key;
  InputConfig cfg;
  std::shared_ptr<InputRx> rx;
  std::shared_ptr<DelayLine> delay;
  std::shared_ptr<const StationPlan::Input> plan_input;
};

// The Stereo Tool processing of a running station (none: worker null).
struct ProcessingRuntime {
  std::shared_ptr<ProcessingWorker> worker;
  std::string key;          // as built (a secret: compared, never logged)
  int web_port = 0;         // as built
  std::string preset;       // the configured name it runs with
  std::string preset_path;  // the file it was given last
  uint64_t events = 0;      // events_since() position (housekeeping logs them)
  // automatic retries after "too slow" (ruling S1)
  int retries = 0;
  int64_t retry_at_ns = 0;  // the next one (0: none due)
  bool retrying = false;    // the last retry's outcome is still to be logged
  int64_t running_since_ns = 0;
};

// The running part of one enabled station.
struct StationRuntime {
  StationConfig cfg;
  std::vector<std::shared_ptr<InputEntry>> inputs;  // per configured input; null when disabled
  std::map<std::string, std::shared_ptr<StreamWorker>> streams;  // by stream id
  std::unique_ptr<Station> station;
  ProcessingRuntime proc;
  uint64_t fade_in = 0;  // StationPlan::fade_in: counts the switches away from a processing worker
};

struct EncoderEngine::Impl {
  EncoderEngineOptions opts;
  std::mutex lifecycle;  // serializes start() and stop()
  mutable std::mutex m;  // everything below; held by every public method and the housekeeper
  EncoderConfig config;
  bool running = false;
  std::string iface;
  int64_t started_ns = 0;
  ClockSource clock;
  std::unique_ptr<PtpClient> ptp;
  std::unique_ptr<RxThread> rx;
  std::map<std::string, std::unique_ptr<StationRuntime>> stations;  // enabled stations by id
  std::vector<std::pair<std::shared_ptr<InputRx>, int64_t>> lingering;  // removed inputs, since when
  // Receive sets the network thread may still use: released here (with the sockets and rings
  // only they still hold) once the engine has the only reference, never on the network thread.
  std::vector<std::shared_ptr<const detail::RxSet>> retired_rx_sets;
  int64_t iface_checked_ns = 0;  // the automatic interface was last looked up (housekeeping)

  std::mutex hk_m;
  std::condition_variable hk_cv;
  bool hk_stop = false;
  std::thread housekeeper;

  PresetStore store;

  explicit Impl(EncoderEngineOptions o) : opts(o), store(o.stereotool_dir, o.protect_stereotool_dir) { set_free_run(); }

  void set_free_run() {
    MediaClock free_run;
    free_run.set_mapping(platform::monotonic_ns(), platform::realtime_ns() + kTaiOffsetNs + opts.free_run_offset_ns, 1.0);
    clock.set(free_run, false);
  }

  void apply(const EncoderConfig& next);
  void update_inputs(StationRuntime& rt, const StationConfig& sc);
  void update_processing(StationRuntime& rt, const EncoderConfig& next, size_t index,
                         std::vector<std::shared_ptr<ProcessingWorker>>& finished);
  void processing_housekeeping(StationRuntime& rt, int64_t now);
  void publish(StationRuntime& rt);
  void update_rx_set();
  void release_rx_sets();
  void check_interface(int64_t now);
  void housekeeping_loop();
  void housekeeping();
  void shutdown();
  StationStatus station_status(const StationConfig& sc) const;
};

// The workers of the current runtime that `next` does not keep exactly as they are.
static bool keeps_worker(const EncoderConfig& next, const std::string& station_id, const StreamWorker& w) {
  for (const StationConfig& sc : next.stations) {
    if (sc.id != station_id) continue;
    if (!sc.enabled) return false;
    for (const StreamConfig& s : sc.streams)
      if (s.id == w.config().id) return s.enabled && detail::stream_key(s, sc.name) == w.key();
    return false;
  }
  return false;
}

void EncoderEngine::Impl::update_inputs(StationRuntime& rt, const StationConfig& sc) {
  const int64_t now = platform::monotonic_ns();
  std::vector<std::shared_ptr<InputEntry>> old = std::move(rt.inputs);
  rt.inputs.assign(sc.inputs.size(), nullptr);
  for (size_t i = 0; i < sc.inputs.size(); ++i) {
    const InputConfig& in = sc.inputs[i];
    if (!in.enabled) continue;
    const std::string key = detail::input_key(in, iface);
    // the same input by key: at the same position first, else wherever it was
    std::shared_ptr<InputEntry>* found = nullptr;
    if (i < old.size() && old[i] && old[i]->key == key) found = &old[i];
    for (size_t j = 0; j < old.size() && !found; ++j)
      if (old[j] && old[j]->key == key) found = &old[j];
    std::shared_ptr<InputEntry> e;
    if (found) {
      e = std::move(*found);
    } else {
      // a new input: socket, rings and its own 23 MB delay line, built here (not on the station thread)
      e = std::make_shared<InputEntry>();
      e->key = key;
      e->rx = detail::make_input_rx(in, iface);
      e->delay = detail::make_delay_line();
      if (!e->rx->error.empty())
        log_warning("Stacja \"" + sc.name + "\", wejście " + std::to_string(i + 1) + ": odbiór nieaktywny: " + e->rx->error);
    }
    if (found && e->cfg.latency_us != in.latency_us) {
      // in place: the socket, the ring and the delay history stay (the station reads all its
      // inputs at the largest latency, so the timeline it mixes stays continuous)
      e->rx->latency_frames = ptime_us_to_frames(in.latency_us);
      if (e->rx->stream) e->rx->stream->set_latency_frames(e->rx->latency_frames);
    }
    e->cfg = in;
    const double gain = db_to_gain(in.gain_db);
    const auto& pi = e->plan_input;
    if (!pi || pi->rx != e->rx || pi->gain != gain || pi->delay_s != in.delay_s)
      e->plan_input = std::make_shared<const StationPlan::Input>(StationPlan::Input{e->rx, e->delay, gain, in.delay_s});
    rt.inputs[i] = std::move(e);
  }
  // inputs no longer there: the station fades them out; they keep receiving meanwhile
  for (auto& e : old)
    if (e) lingering.emplace_back(e->rx, now);
}

// What processing the station gets from `next` (it is next.stations[index]): a worker is built
// when processing is switched on, kept through preset changes (the preset is loaded into it), and
// replaced when the key or the web port change. A worker that goes (switched off, replaced) fades
// its output out first; the station then fades its own output in (StationPlan::fade_in), so the
// jump back by the worker's delay is two 5 ms ramps, like a delay change. Its stop is left to the
// caller (`finished`), after the new plan is out.
void EncoderEngine::Impl::update_processing(StationRuntime& rt, const EncoderConfig& next, size_t index,
                                            std::vector<std::shared_ptr<ProcessingWorker>>& finished) {
  const StationConfig& sc = next.stations[index];
  ProcessingRuntime& p = rt.proc;
  const std::string who = processing_who(sc.name);
  const bool want = sc.processing.enabled;
  std::string key, path;
  int port = 0;
  if (want) {
    key = effective_stereotool_key(next, sc);
    // the station's position in the configuration: stable while the stations before it stay
    // (validate_config keeps base … base + kMaxStations - 1 clear of the panel port)
    port = index < size_t(kMaxStations) ? next.stereotool_web_port_base + int(index) : 0;
    // a preset not there (yet): Stereo Tool cannot load it, the station runs unprocessed and says so
    path = store.path_of(sc.processing.preset);
  }
  if (p.worker && (!want || key != p.key || port != p.web_port)) {
    p.worker->fade_out();
    const int64_t until = platform::monotonic_ns() + kFadeOutWaitNs;
    while (!p.worker->faded_out() && platform::monotonic_ns() < until) platform::sleep_ns(1'000'000);
    if (!p.worker->faded_out()) {
      // its output thread must be done before the station's new path hands anything to the streams
      log_warning(who + "wyjście processingu nie wyciszyło się w " + wait_text(kFadeOutWaitNs) + ", zatrzymane");
      p.worker->stop();
    }
    log_info(who + (want ? "nowa instancja Stereo Tool (zmiana klucza licencji albo portu interfejsu WWW)" : "wyłączony"));
    finished.push_back(std::move(p.worker));
    p = ProcessingRuntime{};
    ++rt.fade_in;
  }
  if (!want) return;
  if (!p.worker) {
    ProcessingSettings s;
    s.license_key = key;
    s.preset_path = path;
    s.web_port = port;
    if (path.empty()) s.setup_error = "brak folderu presetów Stereo Tool";  // never Stereo Tool's defaults on air
    std::string err;
    s.home_dir = store.station_home(sc.id, &err);
    if (s.home_dir.empty() && !store.root().empty()) log_warning(who + err + " (Stereo Tool użyje swojego folderu)");
    p.worker = std::make_shared<ProcessingWorker>(s, sc.id);
    p.key = key;
    p.web_port = port;
    p.preset = sc.processing.preset;
    p.preset_path = path;
    log_info(who + "włączony, preset „" + sc.processing.preset + "”, interfejs WWW Stereo Tool: http://127.0.0.1:" +
             std::to_string(port) + "/ (tylko na tym komputerze)");
  } else if (path != p.preset_path || !same_preset_name(p.preset, sc.processing.preset)) {
    p.worker->set_preset(path);
    p.preset = sc.processing.preset;
    p.preset_path = path;
    log_info(who + "nowy preset „" + sc.processing.preset + "”");
  }
}

void EncoderEngine::Impl::publish(StationRuntime& rt) {
  auto plan = std::make_shared<StationPlan>();
  uint32_t latency = 0;
  size_t n = 0;
  for (const auto& e : rt.inputs) {
    if (!e || n == plan->inputs.size()) continue;
    plan->inputs[n++] = e->plan_input;
    latency = std::max(latency, e->rx->latency_frames);
  }
  plan->latency_frames = latency ? latency : kDefaultLatencyFrames;
  std::vector<std::shared_ptr<StreamWorker>> streams;
  for (const StreamConfig& s : rt.cfg.streams) {
    auto it = rt.streams.find(s.id);
    if (it != rt.streams.end() && it->second->active()) streams.push_back(it->second);
  }
  if (rt.proc.worker) {
    // the worker feeds the streams (taken at its next block, before the station's first push)
    rt.proc.worker->set_streams(std::move(streams));
    plan->processing = rt.proc.worker;
  } else {
    plan->streams = std::move(streams);
  }
  plan->fade_in = rt.fade_in;
  if (rt.station) {
    rt.station->publish(std::move(plan));
  } else {
    rt.station = std::make_unique<Station>(clock, std::move(plan));
    log_info("Stacja \"" + rt.cfg.name + "\" uruchomiona");
  }
}

void EncoderEngine::Impl::update_rx_set() {
  auto set = std::make_shared<detail::RxSet>();
  for (const auto& [id, rt] : stations)
    for (const auto& e : rt->inputs)
      if (e) set->push_back(e->rx);
  for (const auto& [in, since] : lingering) set->push_back(in);
  if (rx) retired_rx_sets.push_back(rx->set_inputs(std::move(set)));
  release_rx_sets();
}

void EncoderEngine::Impl::release_rx_sets() {
  // use_count() == 1: the network thread no longer has it and cannot get it again (it only takes
  // the current set), so nothing can race this release
  std::erase_if(retired_rx_sets, [](const auto& set) { return !set || set.use_count() == 1; });
}

// The automatic interface (interface_ip ""), looked up again about every 2 s like BounceAES
// Master does: at boot the card may have no address yet (or an APIPA one) and the inputs and PTP
// would stay on the wrong one until the next save. A different card found re-applies the current
// configuration (PTP and inputs move to it; the streams keep running).
void EncoderEngine::Impl::check_interface(int64_t now) {
  if (!config.interface_ip.empty() || now - iface_checked_ns < kInterfaceCheckNs) return;
  iface_checked_ns = now;
  const std::string found = detail::default_interface_ip();
  if (found.empty() || found == iface) return;
  log_info("Bounce Encoder: zmiana karty sieciowej (automatycznie): " + (iface.empty() ? std::string("(brak)") : iface) + " -> " +
           found + "; wejścia i PTP przechodzą na nową kartę");
  const EncoderConfig current = config;
  apply(current);
}

void EncoderEngine::Impl::apply(const EncoderConfig& next) {
  const std::string next_iface = next.interface_ip.empty() ? detail::default_interface_ip() : next.interface_ip;
  if (next_iface.empty()) log_warning("Nie znaleziono aktywnej karty sieciowej IPv4");
  if (next_iface != iface) log_info("Bounce Encoder: karta sieciowa " + (next_iface.empty() ? std::string("(brak)") : next_iface));
  iface = next_iface;
  if (opts.enable_ptp && (!ptp || ptp->domain() != next.ptp_domain || ptp->iface() != iface)) {
    ptp.reset();
    ptp = std::make_unique<PtpClient>(clock, next.ptp_domain, iface);
  }

  // Every stream that goes away or changes is stopped first, before any replacement connects:
  // Icecast refuses (403) a second source on a mountpoint whose old source is still connected.
  // Until its station takes the new plan, the mix still goes into the stopped worker's queue.
  {
    std::vector<std::shared_ptr<StreamWorker>> retired;
    for (auto& [id, rt] : stations) {
      for (auto it = rt->streams.begin(); it != rt->streams.end();) {
        if (keeps_worker(next, id, *it->second)) {
          ++it;
        } else {
          retired.push_back(it->second);
          it = rt->streams.erase(it);
        }
      }
    }
    stop_workers(std::move(retired));
  }

  std::map<std::string, std::unique_ptr<StationRuntime>> kept;
  std::vector<std::shared_ptr<ProcessingWorker>> finished;  // processing workers that go
  for (size_t index = 0; index < next.stations.size(); ++index) {
    const StationConfig& sc = next.stations[index];
    if (!sc.enabled) continue;
    std::unique_ptr<StationRuntime> rt;
    if (auto it = stations.find(sc.id); it != stations.end()) {
      rt = std::move(it->second);
      stations.erase(it);
    } else {
      rt = std::make_unique<StationRuntime>();
    }
    rt->cfg = sc;
    update_inputs(*rt, sc);
    for (const StreamConfig& s : sc.streams) {
      if (!s.enabled) continue;
      if (auto it = rt->streams.find(s.id); it != rt->streams.end()) {
        if (it->second->gain_db() != s.gain_db) it->second->set_gain_db(s.gain_db);  // live, ramped
        continue;
      }
      auto w = std::make_shared<StreamWorker>(s, sc.name);
      if (w->active()) {
        log_info("Stacja \"" + sc.name + "\": strumień " + s.mount + " uruchomiony");
      } else {
        log_warning("Stacja \"" + sc.name + "\": strumień " + s.mount + " nieaktywny: " + w->status().encoder_error);
      }
      rt->streams[s.id] = std::move(w);
    }
    update_processing(*rt, next, index, finished);
    publish(*rt);
    kept[sc.id] = std::move(rt);
  }
  // stations removed or disabled (their streams were stopped above)
  for (auto& [id, rt] : stations) {
    rt->station->stop();
    if (rt->proc.worker) finished.push_back(std::move(rt->proc.worker));
    log_info("Stacja \"" + rt->cfg.name + "\" zatrzymana");
  }
  stations = std::move(kept);
  update_rx_set();
  config = next;
  // after the new plans: a station may still push into an old worker until its next tick (a copy
  // into a queue nobody reads); the worker itself goes once the station let go of the plan
  stop_workers(std::move(finished));
}

// Every kHousekeepingNs, off the audio threads: releases what the stations and the network
// thread let go of (old plans, faded-out inputs with their sockets and delay lines), lets removed
// inputs stop receiving once faded, retries inputs whose socket could not be opened or joined,
// and follows the automatic interface (check_interface).
void EncoderEngine::Impl::housekeeping_loop() {
  std::unique_lock hk(hk_m);
  while (!hk_stop) {
    hk_cv.wait_for(hk, std::chrono::nanoseconds(kHousekeepingNs));
    if (hk_stop) break;
    hk.unlock();
    {
      std::lock_guard lock(m);
      if (running) housekeeping();
    }
    hk.lock();
  }
}

void EncoderEngine::Impl::housekeeping() {
  const int64_t now = platform::monotonic_ns();
  bool rx_changed = false;
  const size_t before = lingering.size();
  std::erase_if(lingering, [&](const auto& l) { return now - l.second >= kLingerNs; });
  rx_changed = lingering.size() != before;

  for (auto& [id, rt] : stations) {
    bool replan = false;
    for (size_t i = 0; i < rt->inputs.size(); ++i) {
      auto& e = rt->inputs[i];
      if (!e || e->rx->stream || !e->rx->retryable || now - e->rx->attempted_ns < detail::input_retry_ns()) continue;
      auto fresh = detail::make_input_rx(e->cfg, iface);
      if (fresh->stream) log_info("Stacja \"" + rt->cfg.name + "\", wejście " + std::to_string(i + 1) + ": odbiór działa");
      e->rx = std::move(fresh);  // same input: keeps its delay line
      e->plan_input = std::make_shared<const StationPlan::Input>(
          StationPlan::Input{e->rx, e->delay, db_to_gain(e->cfg.gain_db), e->cfg.delay_s});
      replan = true;
    }
    if (replan) {
      publish(*rt);
      rx_changed = true;
    }
    rt->station->collect();
    processing_housekeeping(*rt, now);
  }
  if (rx_changed) update_rx_set();
  release_rx_sets();
  check_interface(now);
}

// Logs the worker's state changes and runs the automatic retries of ruling S1: a Stereo Tool that
// fell behind ("too slow", also stuck in a call) is restarted after 30 s, then 2, 5 and 10
// minutes (and every 10 minutes after that); 10 minutes of processing start the count again. Every
// other bypass (licence, a failure, a crash) waits for a change or a manual restart.
void EncoderEngine::Impl::processing_housekeeping(StationRuntime& rt, int64_t now) {
  ProcessingRuntime& p = rt.proc;
  if (!p.worker) return;
  const std::string who = processing_who(rt.cfg.name);
  std::vector<ProcessingEvent> events;
  const uint64_t seq = p.worker->events_since(p.events, &events);
  const uint64_t first = events.empty() ? seq + 1 : events.front().seq;
  const bool gap = first > p.events + 1;
  if (gap) log_warning(who + "pominięto zmiany stanu processingu: " + std::to_string(first - p.events - 1));
  p.events = seq;
  auto schedule = [&] {
    const int64_t wait = retry_wait_ns(p.retries);
    p.retry_at_ns = now + wait;
    log_info(who + "automatyczna ponowna próba za " + wait_text(wait));
  };
  for (const ProcessingEvent& e : events) {
    log_event(who, e, p.worker->delay_frames());
    using R = ProcessingReason;
    if (e.state == ProcessingState::Running) {
      if (p.retrying) log_info(who + "ponowna próba udana: Stereo Tool znów przetwarza sygnał");
      p.retrying = false;
      p.retry_at_ns = 0;
      p.running_since_ns = now;
      continue;
    }
    p.running_since_ns = 0;
    if (e.reason_code == R::TooSlow) {
      if (p.retrying) log_warning(who + "ponowna próba nieudana: Stereo Tool znów nie nadąża");
      p.retrying = false;
      schedule();
    } else if (e.reason_code != R::Starting && e.reason_code != R::LicenceCheck && e.reason_code != R::RestartWaits) {
      if (p.retrying) log_warning(who + "ponowna próba nieudana: " + e.reason);
      p.retrying = false;
      p.retry_at_ns = 0;  // not retried automatically
    }
  }
  if (gap && !p.retrying && p.retry_at_ns == 0) {  // events lost: the state says what to do
    const ProcessingStatus st = p.worker->status();
    if (st.state == ProcessingState::Bypass && st.reason_code == ProcessingReason::TooSlow) schedule();
  }
  if (p.retries > 0 && p.running_since_ns != 0 && now - p.running_since_ns >= retry_reset_ns()) p.retries = 0;
  if (p.retry_at_ns != 0 && now >= p.retry_at_ns) {
    p.retry_at_ns = 0;
    p.retrying = true;
    ++p.retries;
    log_info(who + "automatyczna ponowna próba nr " + std::to_string(p.retries) + " (Stereo Tool nie nadążał)");
    p.worker->restart();
  }
}

void EncoderEngine::Impl::shutdown() {
  std::vector<std::shared_ptr<StreamWorker>> workers;
  std::vector<std::shared_ptr<ProcessingWorker>> processing;
  for (auto& [id, rt] : stations) {
    rt->station->stop();
    for (auto& [sid, w] : rt->streams) workers.push_back(w);
    if (rt->proc.worker) processing.push_back(rt->proc.worker);
  }
  // side by side: a Stereo Tool that hangs (e.g. in Delete after another instance crashed) holds
  // the stop for its timeout once, not once per station
  std::thread stop_processing([&] { stop_workers(std::move(processing)); });
  stop_workers(std::move(workers));
  stop_processing.join();
  stations.clear();
  lingering.clear();
  // the network thread lets go of its set first; the receivers are released after it ended
  if (rx) retired_rx_sets.push_back(rx->set_inputs(std::make_shared<detail::RxSet>()));
  rx.reset();
  retired_rx_sets.clear();
  ptp.reset();
}

EncoderEngine::EncoderEngine(EncoderEngineOptions opts) : impl_(std::make_unique<Impl>(opts)) {}

EncoderEngine::~EncoderEngine() { stop(); }

bool EncoderEngine::start(const EncoderConfig& config, std::string* error) {
  auto& m = *impl_;
  std::lock_guard life(m.lifecycle);
  std::lock_guard lock(m.m);
  if (m.running) {
    if (error) *error = "silnik enkodera już działa";
    return false;
  }
  platform::net_init();
  m.set_free_run();  // until PTP takes over
  m.started_ns = platform::monotonic_ns();
  if (!m.opts.stereotool_dir.empty()) {
    std::string dir_error;
    if (!m.store.prepare(&dir_error)) log_warning("Stereo Tool: " + dir_error);
  }
  m.iface.clear();
  m.iface_checked_ns = m.started_ns;
  m.rx = std::make_unique<RxThread>(m.clock);
  m.running = true;
  m.apply(config);
  {
    std::lock_guard hk(m.hk_m);
    m.hk_stop = false;
  }
  m.housekeeper = std::thread([&m] { m.housekeeping_loop(); });
  log_info("Bounce Encoder: silnik uruchomiony, stacji: " + std::to_string(m.stations.size()));
  return true;
}

void EncoderEngine::stop() {
  auto& m = *impl_;
  std::lock_guard life(m.lifecycle);
  {
    std::lock_guard hk(m.hk_m);
    m.hk_stop = true;
  }
  m.hk_cv.notify_all();
  if (m.housekeeper.joinable()) m.housekeeper.join();  // before taking m: it takes m itself
  std::lock_guard lock(m.m);
  if (!m.running) return;
  m.running = false;
  m.shutdown();
  log_info("Bounce Encoder: silnik zatrzymany");
}

void EncoderEngine::apply_config(const EncoderConfig& config) {
  auto& m = *impl_;
  std::lock_guard lock(m.m);
  if (!m.running) {
    m.config = config;
    return;
  }
  m.apply(config);
}

EncoderConfig EncoderEngine::config() const {
  std::lock_guard lock(impl_->m);
  return impl_->config;
}

MediaClock EncoderEngine::media_clock() const { return impl_->clock.get(); }

StationStatus EncoderEngine::Impl::station_status(const StationConfig& sc) const {
  StationStatus st;
  st.id = sc.id;
  st.name = sc.name;
  st.enabled = sc.enabled;
  const StationRuntime* rt = nullptr;
  if (auto it = stations.find(sc.id); running && it != stations.end()) rt = it->second.get();
  st.running = rt != nullptr;
  if (rt) {
    st.started_ns = rt->station->started_ns();
    const auto mix = rt->station->mix_peak();
    for (int s = 0; s < 2; ++s) st.peak_db[s] = detail::level_db(mix[s]);
    st.skipped_frames = rt->station->skipped_frames();
  }
  const int64_t now = platform::monotonic_ns();
  for (size_t i = 0; i < sc.inputs.size(); ++i) {
    const InputConfig& in = sc.inputs[i];
    InputStatus is;
    is.index = int(i);
    is.enabled = in.enabled;
    is.name = in.name;
    is.latency_us = in.latency_us;
    is.gain_db = in.gain_db;
    is.delay_s = in.delay_s;
    const InputEntry* e = rt && i < rt->inputs.size() ? rt->inputs[i].get() : nullptr;
    if (e) {
      const InputRx& rx = *e->rx;
      if (is.name.empty()) is.name = rx.name;
      if (!rx.group.empty()) is.address = rx.group + ":" + std::to_string(rx.port);
      is.error = rx.error;
      is.receiver_id = rx.id;
      if (rx.stream) {
        is.rx = rx.stream->stats();
        is.signal = rx.stream->has_signal(now);
      }
      if (const auto peak = rt->station->input_peak(e->delay.get()))
        for (int s = 0; s < 2; ++s) is.peak_db[s] = detail::level_db((*peak)[s]);
    } else if (auto parsed = parse_sdp(in.sdp); parsed.stream) {
      if (is.name.empty()) is.name = parsed.stream->session_name;
      is.address = parsed.stream->multicast_address + ":" + std::to_string(parsed.stream->port);
    }
    st.inputs.push_back(std::move(is));
  }
  for (const StreamConfig& s : sc.streams) {
    if (rt) {
      if (auto it = rt->streams.find(s.id); it != rt->streams.end()) {
        st.streams.push_back(it->second->status());
        continue;
      }
    }
    StreamStatus ss;  // not running (disabled stream or station)
    ss.id = s.id;
    ss.enabled = s.enabled;
    ss.codec = s.codec;
    ss.bitrate_kbps = s.bitrate_kbps;
    ss.sample_rate = s.sample_rate;
    ss.host = s.host;
    ss.port = s.port;
    ss.mount = s.mount;
    ss.codec_available = codec_available(s.codec);
    ss.codec_library = codec_library_path(s.codec);
    st.streams.push_back(std::move(ss));
  }
  if (rt && rt->proc.worker) {
    const ProcessingRuntime& p = rt->proc;
    st.processing = p.worker->status();
    st.processing.retries = p.retries;
    if (p.retry_at_ns != 0) st.processing.retry_in_s = std::max<int64_t>(0, (p.retry_at_ns - now + 999'999'999) / 1'000'000'000);
  }
  st.processing.enabled = sc.processing.enabled;
  st.processing.preset = sc.processing.preset;
  return st;
}

EncoderStatus EncoderEngine::status() const {
  const auto& m = *impl_;
  std::lock_guard lock(m.m);
  EncoderStatus s;
  s.running = m.running;
  s.interface_ip = m.iface;
  s.ptp_enabled = m.opts.enable_ptp;
  s.ptp.domain = static_cast<uint8_t>(m.config.ptp_domain);
  if (m.ptp) {
    s.ptp = m.ptp->status();
    s.ptp_error = m.ptp->error();
  }
  s.ptp_free_run = !m.ptp || s.ptp.state != PtpState::Locked;
  if (m.running) s.uptime_s = (platform::monotonic_ns() - m.started_ns) / 1'000'000'000;
  for (const StationConfig& sc : m.config.stations) s.stations.push_back(m.station_status(sc));
  s.stereotool = stereotool_peek();  // never loads the library for the status
  return s;
}

std::vector<PresetInfo> EncoderEngine::list_presets() const { return impl_->store.list(); }

PresetResult EncoderEngine::put_preset(std::string_view name, std::string_view data) {
  PresetResult r = impl_->store.put(name, data);
  if (r.ok()) log_info("Stereo Tool: wgrano preset „" + std::string(name) + "”");
  return r;
}

PresetResult EncoderEngine::get_preset(std::string_view name, std::string* data) const { return impl_->store.get(name, data); }

PresetResult EncoderEngine::remove_preset(std::string_view name) {
  auto& m = *impl_;
  std::lock_guard lock(m.m);  // no configuration change in between
  std::string users;
  for (const StationConfig& sc : m.config.stations)
    if (same_preset_name(sc.processing.preset, name)) users += (users.empty() ? "„" : ", „") + sc.name + "”";
  if (!users.empty()) return {PresetError::InUse, "preset jest używany przez stację " + users};
  PresetResult r = m.store.remove(name);
  if (r.ok()) log_info("Stereo Tool: usunięto preset „" + std::string(name) + "”");
  return r;
}

PresetResult EncoderEngine::save_processing_preset(const std::string& station_id, std::string_view name) {
  auto& m = *impl_;
  std::shared_ptr<ProcessingWorker> worker;
  std::string temp, who;
  {
    std::lock_guard lock(m.m);
    auto it = m.stations.find(station_id);
    if (!m.running || it == m.stations.end() || !it->second->proc.worker)
      return {PresetError::NotRunning, "ta stacja nie ma działającego processingu"};
    if (PresetResult r = m.store.reserve(name, &temp); !r.ok()) return r;
    worker = it->second->proc.worker;
    who = processing_who(it->second->cfg.name);
  }
  // the engine is not held while Stereo Tool saves (on its own thread, between two blocks)
  std::string err;
  if (!worker->save_preset(temp, kSavePresetTimeoutMs, &err)) {
    m.store.abandon(temp);  // one written after a timeout goes at the next start
    log_warning(who + "nie zapisano presetu „" + std::string(name) + "”: " + err);
    return {PresetError::Failed, err};
  }
  PresetResult r = m.store.commit(name, temp);
  if (r.ok())
    log_info(who + "ustawienia zapisane jako preset „" + std::string(name) + "”");
  else
    log_warning(who + "nie zapisano presetu „" + std::string(name) + "”: " + r.message);
  return r;
}

bool EncoderEngine::restart_processing(const std::string& station_id, std::string* error) {
  auto& m = *impl_;
  std::lock_guard lock(m.m);
  auto it = m.stations.find(station_id);
  if (!m.running || it == m.stations.end() || !it->second->proc.worker) {
    if (error) *error = "ta stacja nie ma działającego processingu";
    return false;
  }
  ProcessingRuntime& p = it->second->proc;
  p.retries = 0;  // a manual restart starts the automatic ones from the beginning
  p.retry_at_ns = 0;
  p.retrying = false;
  log_info(processing_who(it->second->cfg.name) + "ponowne uruchomienie (ręczne)");
  p.worker->restart();
  return true;
}

}  // namespace bam::encoder
