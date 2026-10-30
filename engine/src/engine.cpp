// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <fstream>
#include <sstream>

#include "bam/auth.hpp"
#include "bam/log.hpp"
#include "bam/meters.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam {

namespace {
constexpr int64_t kTaiOffsetNs = 37LL * 1'000'000'000;  // PTP timescale is TAI

uint32_t us_to_frames(uint32_t us) { return static_cast<uint32_t>(std::lround(us * (kSampleRate / 1e6))); }

std::string device_label(int device) {
  char b[8];
  std::snprintf(b, sizeof(b), "%02d", device);
  return b;
}

std::string dir_of(const std::string& path) {
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string() : path.substr(0, pos);
}
}  // namespace

Engine::Impl::Impl(EngineOptions o) : opts(std::move(o)) {
  for (auto& p : in_peak) p = kSilenceDb;
  for (auto& p : out_peak) p = kSilenceDb;
  const std::string id = random_hex(8);
  for (int i = 0; i < 8; ++i) self_identity.bytes[i] = static_cast<uint8_t>(std::stoi(id.substr(i * 2, 2), nullptr, 16));
  self_identity.bytes[3] = 0xFF;  // EUI-64 style marker, like identities derived from a MAC
  self_identity.bytes[4] = 0xFE;
}

std::string Engine::Impl::current_interface() const {
  std::lock_guard lock(iface_mutex);
  return interface_ip;
}

std::shared_ptr<const RuntimeSet> Engine::Impl::snapshot() const {
  std::lock_guard lock(runtime_mutex);
  return runtime;
}

void Engine::Impl::rebuild_runtime(const Config& cfg, bool force) {
  const std::string iface = current_interface();
  const auto old = snapshot();
  auto next = std::make_shared<RuntimeSet>();
  const double to_app = gain_to_app(cfg);
  const double to_net = gain_to_network(cfg);

  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& d = cfg.devices[i];
    const int dev = i + 1;
    if (!d.enabled) continue;

    // ---- output ----
    {
      std::ostringstream key;
      key << d.tx_address << '|' << d.tx_ptime_us << '|' << to_net << '|' << iface;
      std::shared_ptr<TxEntry> reuse;
      for (const auto& e : old->tx)
        if (e->device == dev && e->key == key.str() && !force) reuse = e;
      if (reuse) {
        // keep_active can change without disturbing the stream
        auto copy = reuse;
        copy->keep_active = d.keep_active;
        next->tx.push_back(copy);
      } else {
        auto e = std::make_shared<TxEntry>();
        e->device = dev;
        e->key = key.str();
        e->address = d.tx_address;
        e->keep_active = d.keep_active;
        TxConfig tc;
        tc.frames_per_packet = ptime_us_to_frames(d.tx_ptime_us);
        tc.payload_type = 98;
        tc.ssrc = static_cast<uint32_t>(std::stoul(random_hex(4), nullptr, 16));
        tc.gain = to_net;
        e->stream = std::make_unique<TxStream>(tc, view->output(i * 2), view->output(i * 2 + 1));
        next->tx.push_back(e);
      }
    }

    // ---- input ----
    if (!d.rx_sdp.empty()) {
      std::ostringstream key;
      key << d.rx_sdp << '|' << d.rx_latency_us << '|' << to_app << '|' << iface;
      std::shared_ptr<RxEntry> reuse;
      for (const auto& e : old->rx)
        if (e->device == dev && e->key == key.str() && !force) reuse = e;
      if (reuse) {
        next->rx.push_back(reuse);
        continue;
      }
      auto e = std::make_shared<RxEntry>();
      e->device = dev;
      e->key = key.str();
      auto parsed = parse_sdp(d.rx_sdp);
      if (!parsed.stream) {
        e->error = parsed.error;
      } else {
        const auto& s = *parsed.stream;
        e->source_name = s.session_name;
        std::string err;
        auto sock = platform::UdpSocket::open(s.port, &err);
        if (!sock) {
          e->error = err;
        } else if (!sock->join(s.multicast_address, iface, s.source_filter, &err)) {
          e->error = err;
        } else {
          e->socket = std::move(*sock);
          e->stream = std::make_unique<RxStream>(s, view->input(i * 2), view->input(i * 2 + 1), us_to_frames(d.rx_latency_us), to_app);
        }
      }
      if (!e->error.empty()) log_warning("Urządzenie " + device_label(dev) + ": wejście nieaktywne: " + e->error);
      next->rx.push_back(e);
    }
  }

  {
    std::lock_guard lock(runtime_mutex);
    runtime = next;
  }
  runtime_version.fetch_add(1);
  sap_dirty.fetch_add(1);
}

void Engine::Impl::publish_shared_config(const Config& cfg) {
  auto& h = view->header();
  uint32_t mask = 0;
  uint32_t max_ptime_frames = 48;
  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& d = cfg.devices[i];
    view->set_device_name(i, d.name);
    h.rx_latency_frames[i].store(us_to_frames(d.rx_latency_us));
    if (d.enabled) {
      mask |= 1u << i;
      max_ptime_frames = std::max(max_ptime_frames, ptime_us_to_frames(d.tx_ptime_us));
    }
  }
  h.device_active_mask.store(mask);
  h.output_latency_frames.store(max_ptime_frames);
}

bool Engine::Impl::save_config(const Config& cfg, std::string* error) const {
  if (opts.config_path.empty()) return true;
  const std::string dir = dir_of(opts.config_path);
  if (!dir.empty()) platform::ensure_directory(dir);
  const std::string tmp = opts.config_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (error) *error = "nie można zapisać " + tmp;
      return false;
    }
    f << config_to_json(cfg);
    if (!f.good()) {
      if (error) *error = "błąd zapisu " + tmp;
      return false;
    }
  }
  if (!platform::replace_file(tmp, opts.config_path)) {
    if (error) *error = "nie można zastąpić " + opts.config_path;
    return false;
  }
  return true;
}

std::string Engine::Impl::build_output_sdp(const Config& cfg, int device) const {
  const auto& d = cfg.devices[device - 1];
  SdpStream s;
  s.session_name = d.name;
  s.origin_address = current_interface().empty() ? "0.0.0.0" : current_interface();
  s.multicast_address = d.tx_address;
  s.ttl = kMulticastTtl;
  s.port = kRtpPort;
  s.payload_type = 98;
  s.encoding = Encoding::L24;
  s.sample_rate = kSampleRate;
  s.channels = 2;
  s.frames_per_packet = ptime_us_to_frames(d.tx_ptime_us);
  s.ptime_ms = s.frames_per_packet * 1000.0 / kSampleRate;
  {
    std::lock_guard lock(ptp_mutex);
    if (ptp_status.has_master) {
      s.ptp_gmid = ptp_status.grandmaster.to_string();
      s.ptp_domain = ptp_status.domain;
    } else {
      s.ptp_domain = cfg.ptp_domain;
    }
  }
  // The SDP version only changes when this device's own stream changes, so receivers of the
  // other outputs never see a spurious session update.
  const uint16_t version = sap_hash(d.name + "|" + d.tx_address + "|" + std::to_string(d.tx_ptime_us) + "|" + s.ptp_gmid +
                                    "|" + s.origin_address);
  return generate_sdp(s, 0x42414D00u + static_cast<uint64_t>(device), version);
}

// ---------------------------------------------------------------------------------------------

Engine::Engine(EngineOptions opts) : impl_(std::make_unique<Impl>(std::move(opts))) {}

Engine::~Engine() { stop(); }

bool Engine::start(std::string* error) {
  auto& m = *impl_;
  if (m.running) return true;
  platform::net_init();

  // ---- configuration ----
  Config cfg = default_config();
  bool save_needed = false;
  if (!m.opts.config_path.empty()) {
    std::ifstream f(m.opts.config_path, std::ios::binary);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      std::string err;
      if (auto loaded = config_from_json(ss.str(), &err)) {
        cfg = *loaded;
      } else {
        log_error("Plik konfiguracji uszkodzony (" + err + "), zapisano kopię .bad i użyto ustawień domyślnych");
        platform::replace_file(m.opts.config_path, m.opts.config_path + ".bad");
        save_needed = true;
      }
    } else {
      log_info("Brak pliku konfiguracji, tworzę domyślny: " + m.opts.config_path);
      save_needed = true;
    }
  }
  for (const auto& n : normalize_config(cfg)) log_info(n);
  if (auto issues = validate_config(cfg); !issues.empty()) {
    for (const auto& i : issues) log_warning("Konfiguracja: " + i.message);
  }
  if (save_needed) {
    std::string err;
    if (!m.save_config(cfg, &err)) log_error(err);
  }
  m.config = cfg;

  {
    std::lock_guard lock(m.iface_mutex);
    m.interface_ip = cfg.interface_ip.empty() ? platform::default_interface_ip() : cfg.interface_ip;
  }
  if (m.current_interface().empty()) log_warning("Nie znaleziono aktywnej karty sieciowej IPv4");

  // ---- shared memory ----
  std::string err;
  bool existed = false;
  m.shm = platform::SharedMemory::create(m.opts.shm_name, ShmLayout::total_bytes(), &existed, &err);
  if (!m.shm) {
    if (error) *error = "pamięć współdzielona: " + err;
    return false;
  }
  ShmView::initialize(m.shm->data(), static_cast<uint64_t>(platform::realtime_ns()));
  m.view = std::make_unique<ShmView>(m.shm->data());
  m.period_event = platform::NamedEvent::create(m.opts.event_name, &err);
  if (!m.period_event) {
    if (error) *error = "zdarzenie okresu: " + err;
    return false;
  }

  // ---- clock: free-run on the system clock until PTP takes over ----
  MediaClock free_run;
  const int64_t now = platform::monotonic_ns();
  free_run.set_mapping(now, platform::realtime_ns() + kTaiOffsetNs, 1.0);
  m.clock.set(free_run, false);
  m.view->header().ptp_state.store(static_cast<uint32_t>(ShmPtpState::FreeRun));

  // ---- network ----
  auto tx = platform::UdpSocket::open(0, &err);
  if (!tx) {
    if (error) *error = "gniazdo nadawcze: " + err;
    return false;
  }
  tx->set_multicast_out(m.current_interface(), kMulticastTtl, true);
  m.tx_socket = std::move(*tx);

  m.publish_shared_config(cfg);
  m.rebuild_runtime(cfg, true);

  m.started_ns = now;
  m.running = true;
  if (m.opts.enable_ptp) m.threads.emplace_back([&m] { m.ptp_loop(); });
  m.threads.emplace_back([&m] { m.rx_loop(); });
  m.threads.emplace_back([&m] { m.tx_loop(); });
  m.threads.emplace_back([&m] { m.period_loop(); });
  if (m.opts.enable_sap) m.threads.emplace_back([&m] { m.sap_loop(); });
  m.threads.emplace_back([&m] { m.monitor_loop(); });
  log_info(std::string(kProductName) + " " + kVersion + " uruchomiony, karta sieciowa " +
           (m.current_interface().empty() ? "(brak)" : m.current_interface()));
  return true;
}

void Engine::stop() {
  auto& m = *impl_;
  if (!m.running.exchange(false)) return;
  for (auto& t : m.threads)
    if (t.joinable()) t.join();
  m.threads.clear();
  {
    std::lock_guard lock(m.runtime_mutex);
    m.runtime = std::make_shared<RuntimeSet>();
  }
  if (m.view) {
    m.view->header().heartbeat.store(0);
    m.view->header().ptp_state.store(static_cast<uint32_t>(ShmPtpState::Listening));
  }
  m.view.reset();
  m.period_event.reset();
  m.shm.reset();
  m.tx_socket.reset();
  log_info(std::string(kProductName) + " zatrzymany");
}

Config Engine::config() const {
  std::lock_guard lock(impl_->config_mutex);
  return impl_->config;
}

ApplyResult Engine::apply_config(Config next) {
  auto& m = *impl_;
  ApplyResult r;
  std::lock_guard lock(m.config_mutex);
  next.password_hash = m.config.password_hash;
  r.notices = normalize_config(next);
  r.issues = validate_config(next);
  if (!r.issues.empty()) return r;
  std::string err;
  if (!m.save_config(next, &err)) {
    r.issues.push_back({0, "config", err});
    return r;
  }
  const bool iface_changed = next.interface_ip != m.config.interface_ip;
  const bool http_changed = next.http_port != m.config.http_port || next.http_bind != m.config.http_bind;
  const bool ptp_changed = next.ptp_domain != m.config.ptp_domain;
  m.config = next;
  ++m.config_version;
  if (iface_changed) {
    std::lock_guard il(m.iface_mutex);
    m.interface_ip = next.interface_ip.empty() ? platform::default_interface_ip() : next.interface_ip;
  }
  if (m.running) {
    m.publish_shared_config(next);
    if (iface_changed) {
      std::lock_guard tl(m.tx_socket_mutex);
      if (m.tx_socket) m.tx_socket->set_multicast_out(m.current_interface(), kMulticastTtl, true);
    }
    m.rebuild_runtime(next, iface_changed);
    if (ptp_changed || iface_changed) m.ptp_running = false;  // ptp loop re-creates its sockets/slave
  }
  if (http_changed) r.notices.push_back("Zmiana adresu lub portu panelu zacznie działać po ponownym uruchomieniu usługi");
  log_info("Zastosowano nową konfigurację");
  r.ok = true;
  return r;
}

bool Engine::set_password_hash(const std::string& hash) {
  auto& m = *impl_;
  std::lock_guard lock(m.config_mutex);
  Config next = m.config;
  next.password_hash = hash;
  std::string err;
  if (!m.save_config(next, &err)) {
    log_error(err);
    return false;
  }
  m.config = next;
  return true;
}

EngineStatus Engine::status() const {
  const auto& m = *impl_;
  EngineStatus s;
  const Config cfg = config();
  const auto snap = m.snapshot();
  const int64_t now = platform::monotonic_ns();
  {
    std::lock_guard lock(m.ptp_mutex);
    s.ptp = m.ptp_status;
  }
  s.ptp_enabled = m.opts.enable_ptp;
  s.ptp_free_run = !m.ptp_ever_locked;
  s.interface_ip = m.current_interface();
  s.version = kVersion;
  s.uptime_s = m.running ? (now - m.started_ns) / 1'000'000'000 : 0;
  if (m.view) {
    auto& h = m.view->header();
    s.asio_connected = h.client_pid.load() != 0 && h.client_running.load() != 0;
    s.asio_buffer_frames = h.client_pid.load() != 0 ? h.buffer_frames.load() : 0;
  }
  const bool client_recent = now - m.last_client_active_ns.load() < 1'000'000'000;
  std::vector<DiscoveredStream> discovered;
  {
    std::lock_guard lock(m.sap_mutex);
    discovered = m.directory.list();
  }

  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& d = cfg.devices[i];
    DeviceStatus ds;
    ds.index = i + 1;
    ds.enabled = d.enabled;
    ds.name = d.name;
    ds.tx_address = d.tx_address;
    ds.rx_latency_us = d.rx_latency_us;
    ds.tx_ptime_us = d.tx_ptime_us;
    ds.rx_configured = d.enabled && !d.rx_sdp.empty();
    for (const auto& e : snap->tx) {
      if (e->device != ds.index) continue;
      ds.tx = e->stream->stats();
      ds.tx_active = e->keep_active || client_recent;
    }
    for (const auto& e : snap->rx) {
      if (e->device != ds.index) continue;
      ds.rx_error = e->error;
      ds.rx_source_name = e->source_name;
      if (e->stream) {
        ds.rx = e->stream->stats();
        ds.rx_signal = e->stream->has_signal(now);
      }
    }
    if (d.enabled) {
      for (const auto& st : discovered) {
        if (st.parsed.multicast_address == d.tx_address && st.origin != s.interface_ip) {
          ds.tx_conflict = "adres " + d.tx_address + " ogłasza też " + st.origin + " (" + st.name + ")";
        }
      }
    }
    for (int c = 0; c < 2; ++c) {
      ds.in_peak_db[c] = m.in_peak[i * 2 + c].load(std::memory_order_relaxed);
      ds.out_peak_db[c] = m.out_peak[i * 2 + c].load(std::memory_order_relaxed);
    }
    s.devices.push_back(std::move(ds));
  }
  return s;
}

std::vector<DiscoveredStream> Engine::discovered_streams() const {
  std::lock_guard lock(impl_->sap_mutex);
  return impl_->directory.list();
}

std::string Engine::output_sdp(int device) const {
  if (device < 1 || device > kDeviceCount) return {};
  return impl_->build_output_sdp(config(), device);
}

void Engine::reset_stats() {
  const auto snap = impl_->snapshot();
  for (const auto& e : snap->rx)
    if (e->stream) e->stream->reset_stats();
  for (const auto& e : snap->tx) e->stream->reset_stats();
  log_info("Liczniki statystyk wyzerowane");
}

}  // namespace bam
