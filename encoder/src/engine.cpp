// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder engine: owns the network thread, the PTP slave, the stations and their stream
// workers, and rebuilds only what a new configuration changes (see engine.hpp).
#include "bam/encoder/engine.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

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

// Stops the workers side by side: each IcecastClient::stop() may take a few hundred ms.
void stop_workers(std::vector<std::shared_ptr<StreamWorker>> workers) {
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

// The running part of one enabled station, by input index and stream id.
struct StationRuntime {
  StationConfig cfg;
  std::array<std::shared_ptr<InputRx>, kStationInputs> rx;
  std::array<std::shared_ptr<DelayLine>, kStationInputs> delays;
  std::array<std::shared_ptr<const StationPlan::Input>, kStationInputs> plan_inputs;
  std::map<std::string, std::shared_ptr<StreamWorker>> streams;
  std::unique_ptr<Station> station;
};

struct EncoderEngine::Impl {
  EncoderEngineOptions opts;
  mutable std::mutex m;  // everything below; held by every public method
  EncoderConfig config;
  bool running = false;
  std::string iface;
  int64_t started_ns = 0;
  ClockSource clock;
  std::unique_ptr<PtpClient> ptp;
  std::unique_ptr<RxThread> rx;
  std::map<std::string, std::unique_ptr<StationRuntime>> stations;  // enabled stations by id

  explicit Impl(EncoderEngineOptions o) : opts(o) { set_free_run(); }

  void set_free_run() {
    MediaClock free_run;
    free_run.set_mapping(platform::monotonic_ns(), platform::realtime_ns() + kTaiOffsetNs + opts.free_run_offset_ns, 1.0);
    clock.set(free_run, false);
  }

  void apply(const EncoderConfig& next);
  void shutdown();
  StationStatus station_status(const StationConfig& sc) const;
};

void EncoderEngine::Impl::apply(const EncoderConfig& next) {
  const std::string next_iface = next.interface_ip.empty() ? platform::default_interface_ip() : next.interface_ip;
  if (next_iface.empty()) log_warning("Nie znaleziono aktywnej karty sieciowej IPv4");
  if (next_iface != iface) log_info("Bounce Encoder: karta sieciowa " + (next_iface.empty() ? std::string("(brak)") : next_iface));
  iface = next_iface;
  if (opts.enable_ptp && (!ptp || ptp->domain() != next.ptp_domain || ptp->iface() != iface)) {
    ptp.reset();
    ptp = std::make_unique<PtpClient>(clock, next.ptp_domain, iface);
  }

  std::map<std::string, std::unique_ptr<StationRuntime>> kept;
  std::vector<std::unique_ptr<StationRuntime>> gone;
  std::vector<std::shared_ptr<StreamWorker>> retired;
  for (const StationConfig& sc : next.stations) {
    if (!sc.enabled) continue;
    std::unique_ptr<StationRuntime> rt;
    if (auto it = stations.find(sc.id); it != stations.end()) {
      rt = std::move(it->second);
      stations.erase(it);
    } else {
      rt = std::make_unique<StationRuntime>();
    }
    auto plan = std::make_shared<StationPlan>();

    // inputs: a new socket only for a new SDP, latency or interface; delay lines (23 MB) are
    // made here, never on the station thread, and kept while the input stays enabled
    uint32_t latency = 0;
    for (int i = 0; i < kStationInputs; ++i) {
      const InputConfig* in = size_t(i) < sc.inputs.size() && sc.inputs[i].enabled ? &sc.inputs[i] : nullptr;
      if (!in) {
        rt->rx[i].reset();
        rt->delays[i].reset();
        rt->plan_inputs[i].reset();
        continue;
      }
      if (!rt->rx[i] || rt->rx[i]->key != detail::input_key(*in, iface)) {
        rt->rx[i] = detail::make_input_rx(*in, iface);
        if (!rt->rx[i]->error.empty())
          log_warning("Stacja \"" + sc.name + "\", wejście " + std::to_string(i + 1) + ": odbiór nieaktywny: " + rt->rx[i]->error);
      }
      if (!rt->delays[i]) rt->delays[i] = std::make_shared<DelayLine>();
      const double gain = db_to_gain(in->gain_db);
      auto& pi = rt->plan_inputs[i];
      if (!pi || pi->rx != rt->rx[i] || pi->delay != rt->delays[i] || pi->gain != gain || pi->delay_s != in->delay_s)
        pi = std::make_shared<const StationPlan::Input>(StationPlan::Input{rt->rx[i], rt->delays[i], gain, in->delay_s});
      plan->inputs[i] = pi;
      latency = std::max(latency, rt->rx[i]->latency_frames);
    }
    plan->latency_frames = latency ? latency : kDefaultLatencyFrames;

    // streams: an edited stream is rebuilt alone; a gain change is applied live
    std::map<std::string, std::shared_ptr<StreamWorker>> workers;
    for (const StreamConfig& s : sc.streams) {
      if (!s.enabled) continue;
      auto it = rt->streams.find(s.id);
      if (it != rt->streams.end() && it->second->key() == detail::stream_key(s, sc.name)) {
        if (it->second->gain_db() != s.gain_db) it->second->set_gain_db(s.gain_db);
        workers[s.id] = it->second;
        rt->streams.erase(it);
      } else {
        auto w = std::make_shared<StreamWorker>(s, sc.name);
        if (w->active()) {
          log_info("Stacja \"" + sc.name + "\": strumień " + s.mount + " uruchomiony");
        } else {
          log_warning("Stacja \"" + sc.name + "\": strumień " + s.mount + " nieaktywny: " + w->status().encoder_error);
        }
        workers[s.id] = std::move(w);
      }
      if (workers[s.id]->active()) plan->streams.push_back(workers[s.id]);
    }
    for (auto& [id, w] : rt->streams) retired.push_back(w);
    rt->streams = std::move(workers);
    rt->cfg = sc;

    if (rt->station) {
      rt->station->publish(std::move(plan));
    } else {
      rt->station = std::make_unique<Station>(clock, std::move(plan));
      log_info("Stacja \"" + sc.name + "\" uruchomiona");
    }
    kept[sc.id] = std::move(rt);
  }
  for (auto& [id, rt] : stations) gone.push_back(std::move(rt));  // removed or disabled
  stations = std::move(kept);

  auto set = std::make_shared<detail::RxSet>();
  for (const auto& [id, rt] : stations)
    for (const auto& in : rt->rx)
      if (in) set->push_back(in);
  if (rx) rx->set_inputs(std::move(set));

  // what is gone stops only now, after the stations were handed plans without it
  for (auto& rt : gone) {
    rt->station->stop();
    log_info("Stacja \"" + rt->cfg.name + "\" zatrzymana");
    for (auto& [id, w] : rt->streams) retired.push_back(w);
  }
  if (!retired.empty()) stop_workers(std::move(retired));
  config = next;
}

void EncoderEngine::Impl::shutdown() {
  std::vector<std::shared_ptr<StreamWorker>> workers;
  for (auto& [id, rt] : stations) {
    rt->station->stop();
    for (auto& [sid, w] : rt->streams) workers.push_back(w);
  }
  if (!workers.empty()) stop_workers(std::move(workers));
  stations.clear();
  rx.reset();
  ptp.reset();
}

EncoderEngine::EncoderEngine(EncoderEngineOptions opts) : impl_(std::make_unique<Impl>(opts)) {}

EncoderEngine::~EncoderEngine() { stop(); }

bool EncoderEngine::start(const EncoderConfig& config, std::string* error) {
  auto& m = *impl_;
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
  log_info("Bounce Encoder: silnik uruchomiony, stacji: " + std::to_string(m.stations.size()));
  return true;
}

void EncoderEngine::stop() {
  auto& m = *impl_;
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
  std::array<std::array<float, 2>, kStationInputs> input_peaks{};
  if (rt) {
    rt->station->collect();
    st.started_ns = rt->station->started_ns();
    const auto mix = rt->station->mix_peak();
    for (int s = 0; s < 2; ++s) st.peak_db[s] = detail::level_db(mix[s]);
    st.skipped_frames = rt->station->skipped_frames();
    input_peaks = rt->station->input_peaks();
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
    const InputRx* rx = rt && i < size_t(kStationInputs) ? rt->rx[i].get() : nullptr;
    if (rx) {
      if (is.name.empty()) is.name = rx->name;
      if (!rx->group.empty()) is.address = rx->group + ":" + std::to_string(rx->port);
      is.error = rx->error;
      if (rx->stream) {
        is.rx = rx->stream->stats();
        is.signal = rx->stream->has_signal(now);
      }
      for (int s = 0; s < 2; ++s) is.peak_db[s] = detail::level_db(input_peaks[i][s]);
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
    s.ptp_free_run = !m.ptp->ever_stepped();
  }
  if (m.running) s.uptime_s = (platform::monotonic_ns() - m.started_ns) / 1'000'000'000;
  for (const StationConfig& sc : m.config.stations) s.stations.push_back(m.station_status(sc));
  return s;
}

}  // namespace bam::encoder
