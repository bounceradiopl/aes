// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder engine: owns the network thread, the PTP slave, the stations and their stream
// workers, and rebuilds only what a new configuration changes (see engine.hpp).
#include "bam/encoder/engine.hpp"

#include <algorithm>
#include <condition_variable>
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

// Stops the workers side by side: each IcecastClient::stop() may take a few hundred ms.
void stop_workers(std::vector<std::shared_ptr<StreamWorker>> workers) {
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

}  // namespace

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

// The running part of one enabled station.
struct StationRuntime {
  StationConfig cfg;
  std::vector<std::shared_ptr<InputEntry>> inputs;  // per configured input; null when disabled
  std::map<std::string, std::shared_ptr<StreamWorker>> streams;  // by stream id
  std::unique_ptr<Station> station;
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

  std::mutex hk_m;
  std::condition_variable hk_cv;
  bool hk_stop = false;
  std::thread housekeeper;

  explicit Impl(EncoderEngineOptions o) : opts(o) { set_free_run(); }

  void set_free_run() {
    MediaClock free_run;
    free_run.set_mapping(platform::monotonic_ns(), platform::realtime_ns() + kTaiOffsetNs + opts.free_run_offset_ns, 1.0);
    clock.set(free_run, false);
  }

  void apply(const EncoderConfig& next);
  void update_inputs(StationRuntime& rt, const StationConfig& sc);
  void publish(StationRuntime& rt);
  void update_rx_set();
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
  for (const StreamConfig& s : rt.cfg.streams) {
    auto it = rt.streams.find(s.id);
    if (it != rt.streams.end() && it->second->active()) plan->streams.push_back(it->second);
  }
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
  if (rx) rx->set_inputs(std::move(set));
}

void EncoderEngine::Impl::apply(const EncoderConfig& next) {
  const std::string next_iface = next.interface_ip.empty() ? platform::default_interface_ip() : next.interface_ip;
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
  for (const StationConfig& sc : next.stations) {
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
    publish(*rt);
    kept[sc.id] = std::move(rt);
  }
  // stations removed or disabled (their streams were stopped above)
  for (auto& [id, rt] : stations) {
    rt->station->stop();
    log_info("Stacja \"" + rt->cfg.name + "\" zatrzymana");
  }
  stations = std::move(kept);
  update_rx_set();
  config = next;
}

// Every kHousekeepingNs, off the audio threads: releases what the stations let go of (old
// plans, faded-out inputs with their sockets and delay lines), lets removed inputs stop
// receiving once faded, and retries inputs whose socket could not be opened or joined.
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
  }
  if (rx_changed) update_rx_set();
}

void EncoderEngine::Impl::shutdown() {
  std::vector<std::shared_ptr<StreamWorker>> workers;
  for (auto& [id, rt] : stations) {
    rt->station->stop();
    for (auto& [sid, w] : rt->streams) workers.push_back(w);
  }
  stop_workers(std::move(workers));
  stations.clear();
  lingering.clear();
  rx.reset();
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
  m.iface.clear();
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
  return s;
}

}  // namespace bam::encoder
