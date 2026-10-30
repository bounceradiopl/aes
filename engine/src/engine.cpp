// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "bam/auth.hpp"
#include "bam/log.hpp"
#include "bam/meters.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "bam/virtual_device.hpp"
#include "engine_internal.hpp"

namespace bam {

namespace {
constexpr int64_t kTaiOffsetNs = 37LL * 1'000'000'000;  // PTP timescale is TAI
constexpr size_t kMaxNameBytes = 40;

uint32_t us_to_frames(uint32_t us) { return static_cast<uint32_t>(std::lround(us * (kSampleRate / 1e6))); }

std::string two_digits(int n) {
  char b[8];
  std::snprintf(b, sizeof(b), "%02d", n);
  return b;
}

std::string dir_of(const std::string& path) {
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::string utf8_prefix(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t n = max_bytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

std::string output_label(const Config& cfg, int output) {
  const auto& name = cfg.outputs[output - 1].name;
  return name.empty() ? "BounceAES " + two_digits(output) : name;
}

std::string asio_label(const Config& cfg, int pair) {
  const auto& name = cfg.asio_names[pair - 1];
  return name.empty() ? "BounceAES " + two_digits(pair) : name;
}

// Windows sources used by any route get a win_src pair; pairs already assigned stay where they are.
std::map<std::string, int> assign_windows_slots(const Config& cfg, const std::map<std::string, int>& previous) {
  std::set<std::string> used;
  for (const auto& [dst, src] : cfg.routes) {
    auto s = parse_source(src);
    if (s && is_windows(*s)) used.insert(src);
  }
  std::map<std::string, int> slots;
  std::set<int> taken;
  for (const auto& id : used) {
    auto it = previous.find(id);
    if (it != previous.end()) {
      slots[id] = it->second;
      taken.insert(it->second);
    }
  }
  int next = 0;
  for (const auto& id : used) {
    if (slots.count(id)) continue;
    while (taken.count(next)) ++next;
    if (next >= kWinPairs) break;  // validate_config limits Windows sources to 16
    slots[id] = next;
    taken.insert(next);
  }
  return slots;
}
}  // namespace

Engine::Impl::Impl(EngineOptions o) : opts(std::move(o)) {
  for (auto& p : net_peak) p = kSilenceDb;
  for (auto& p : asio_peak) p = kSilenceDb;
  for (auto& p : win_peak) p = kSilenceDb;
  for (auto& d : net_delay) d = 0;
  const std::string id = random_hex(8);
  for (int i = 0; i < 8; ++i) self_identity.bytes[i] = static_cast<uint8_t>(std::stoi(id.substr(i * 2, 2), nullptr, 16));
  self_identity.bytes[3] = 0xFF;  // EUI-64 style marker, like identities derived from a MAC
  self_identity.bytes[4] = 0xFE;
  for (auto& s : tx_ssrc) s = static_cast<uint32_t>(std::stoul(random_hex(4), nullptr, 16));
}

std::string Engine::Impl::current_interface() const {
  std::lock_guard lock(iface_mutex);
  return interface_ip;
}

std::string Engine::Impl::station_name(const Config& cfg) const {
  if (!cfg.station_name.empty()) return cfg.station_name;
  std::lock_guard lock(iface_mutex);
  return host_name;
}

std::shared_ptr<const RuntimeSet> Engine::Impl::snapshot() const {
  std::lock_guard lock(runtime_mutex);
  return runtime;
}

ResolvedSource Engine::Impl::resolve(const Config& cfg, const RuntimeSet& rt, const std::string& source_id) const {
  ResolvedSource r;
  r.id = source_id;
  if (source_id.empty()) return r;
  auto s = parse_source(source_id);
  if (!s) {
    r.note = "nieprawidłowe źródło";
    return r;
  }
  switch (s->kind) {
    case SourceKind::NetIn: {
      const auto& sub = cfg.subscriptions[s->index - 1];
      r.kind = ShmSource::Net;
      r.pair = s->index - 1;
      r.delay = us_to_frames(sub.latency_us);
      if (!sub.enabled) {
        r.note = "subskrypcja wyłączona";
        break;
      }
      r.available = true;
      for (const auto& e : rt.rx) {
        if (e->subscription != s->index) continue;
        if (!e->error.empty()) {
          r.available = false;
          r.note = e->error;
        }
      }
      break;
    }
    case SourceKind::AsioOut:
      r.kind = ShmSource::Asio;
      r.pair = s->index - 1;
      r.available = true;
      break;
    case SourceKind::WinApp:
    case SourceKind::WinLoop:
    case SourceKind::WinIn: {
      auto it = rt.win_slots.find(source_id);
      if (it == rt.win_slots.end()) {
        r.note = "brak wolnego kanału Windows";
        break;
      }
      r.kind = ShmSource::Win;
      r.pair = it->second;
      r.delay = kWinSourceDelayFrames;
      r.available = true;  // the bridge report refines this (no user session, device missing)
      break;
    }
    case SourceKind::None:
      break;
  }
  return r;
}

void Engine::Impl::rebuild_runtime(const Config& cfg, bool force) {
  const std::string iface = current_interface();
  const auto old = snapshot();
  auto next = std::make_shared<RuntimeSet>();
  const double to_app = gain_to_app(cfg);
  const double to_net = gain_to_network(cfg);
  next->win_slots = assign_windows_slots(cfg, old->win_slots);

  // ---- subscriptions: net_in(2n-2, 2n-1) ----
  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& sub = cfg.subscriptions[i];
    const int n = i + 1;
    net_delay[i].store(us_to_frames(sub.latency_us), std::memory_order_relaxed);
    if (!sub.enabled || sub.sdp.empty()) continue;
    std::ostringstream key;
    key << sub.sdp << '|' << sub.latency_us << '|' << to_app << '|' << iface;
    std::shared_ptr<RxEntry> reuse;
    for (const auto& e : old->rx)
      if (e->subscription == n && e->key == key.str() && !force) reuse = e;
    if (reuse) {
      next->rx.push_back(reuse);
      continue;
    }
    auto e = std::make_shared<RxEntry>();
    e->subscription = n;
    e->key = key.str();
    auto parsed = parse_sdp(sub.sdp);
    if (!parsed.stream) {
      e->error = parsed.error;
    } else {
      const auto& s = *parsed.stream;
      e->source_name = s.session_name;
      e->group = s.multicast_address;
      e->source = s.source_filter;
      e->port = s.port;
      e->stream = std::make_unique<RxStream>(s, view->net_in(i * 2), view->net_in(i * 2 + 1), us_to_frames(sub.latency_us), to_app);
    }
    next->rx.push_back(e);
  }

  // ---- receive sockets: per port, at most kGroupsPerSocket groups each ----
  // Linux limits a socket to 20 memberships (igmp_max_memberships), older Windows stacks too.
  constexpr size_t kGroupsPerSocket = 16;
  std::map<uint16_t, std::vector<std::shared_ptr<RxEntry>>> by_port;
  for (const auto& e : next->rx)
    if (e->stream) by_port[e->port].push_back(e);
  for (auto& [port, entries] : by_port) {
    std::vector<std::pair<std::string, std::string>> memberships;
    {
      std::set<std::pair<std::string, std::string>> unique;
      for (const auto& e : entries) unique.insert({e->group, e->source});
      memberships.assign(unique.begin(), unique.end());
    }
    for (size_t first = 0; first < memberships.size(); first += kGroupsPerSocket) {
      const size_t last = std::min(memberships.size(), first + kGroupsPerSocket);
      const std::set<std::pair<std::string, std::string>> shard(memberships.begin() + first, memberships.begin() + last);
      std::ostringstream key;
      key << iface;
      for (const auto& [g, src] : shard) key << '|' << g << '@' << src;
      std::shared_ptr<RxPort> rp;
      for (const auto& old_port : old->rx_ports)
        if (old_port->port == port && old_port->key == key.str() && !force) rp = old_port;
      if (!rp) {
        rp = std::make_shared<RxPort>();
        rp->port = port;
        rp->key = key.str();
        std::string err;
        auto sock = platform::UdpSocket::open(port, &err);
        if (!sock) {
          for (const auto& e : entries)
            if (shard.count({e->group, e->source})) e->error = err;
          continue;
        }
        rp->socket = std::move(*sock);
        for (const auto& [g, src] : shard) {
          if (rp->socket.join(g, iface, src, &err)) continue;
          for (const auto& e : entries)
            if (e->group == g && e->source == src) e->error = err;
        }
      }
      // A shard only dispatches to its own streams; on stacks that deliver every group joined on
      // the port to every socket, other shards' packets are ignored here.
      rp->entries.clear();
      for (const auto& e : entries)
        if (e->error.empty() && shard.count({e->group, e->source})) rp->entries.push_back(e);
      next->rx_ports.push_back(rp);
    }
  }
  for (const auto& e : next->rx)
    if (!e->error.empty() && e->stream) e->stream.reset();
  for (const auto& e : next->rx)
    if (!e->error.empty()) log_warning("Subskrypcja " + two_digits(e->subscription) + ": odbiór nieaktywny: " + e->error);

  // ---- outputs: each reads its routed source D_src behind ----
  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& o = cfg.outputs[i];
    const int n = i + 1;
    if (!o.enabled) continue;
    auto route = cfg.routes.find("net-out:" + std::to_string(n));
    const ResolvedSource src = resolve(cfg, *next, route == cfg.routes.end() ? std::string() : route->second);
    std::ostringstream key;
    key << o.address << '|' << o.ptime_us << '|' << to_net << '|' << iface << '|' << static_cast<int>(src.kind) << ':'
        << src.pair << ':' << src.delay;
    const bool follows_asio = src.kind == ShmSource::None || src.kind == ShmSource::Asio;
    std::shared_ptr<TxEntry> reuse;
    for (const auto& e : old->tx)
      if (e->output == n && e->key == key.str() && !force) reuse = e;
    if (reuse) {
      // keep_active can change without disturbing the stream
      reuse->keep_active = o.keep_active;
      next->tx.push_back(reuse);
      continue;
    }
    auto e = std::make_shared<TxEntry>();
    e->output = n;
    e->key = key.str();
    e->address = o.address;
    e->keep_active = o.keep_active;
    e->follows_asio = follows_asio;
    TxConfig tc;
    tc.frames_per_packet = ptime_us_to_frames(o.ptime_us);
    tc.payload_type = 98;
    tc.ssrc = tx_ssrc[i];
    tc.gain = to_net;
    tc.source_delay = src.delay;
    e->stream = std::make_unique<TxStream>(tc, view->source_ring(src.kind, src.pair * 2), view->source_ring(src.kind, src.pair * 2 + 1));
    next->tx.push_back(e);
  }

  publish_shared_config(cfg, *next);
  publish_bridge_request(cfg, *next);
  {
    std::lock_guard lock(runtime_mutex);
    runtime = next;
  }
  runtime_version.fetch_add(1);
  sap_dirty.fetch_add(1);
}

void Engine::Impl::publish_shared_config(const Config& cfg, const RuntimeSet& rt) {
  auto& h = view->header();
  uint32_t mask = 0;
  uint32_t max_ptime_frames = 48;
  std::set<int> played;  // ASIO output pairs used as a source
  for (const auto& [dst, src] : cfg.routes) {
    auto s = parse_source(src);
    if (s && s->kind == SourceKind::AsioOut) played.insert(s->index - 1);
  }
  for (int i = 0; i < kDeviceCount; ++i) {
    view->set_device_name(i, asio_label(cfg, i + 1));
    auto route = cfg.routes.find("asio-in:" + std::to_string(i + 1));
    const ResolvedSource src = resolve(cfg, rt, route == cfg.routes.end() ? std::string() : route->second);
    view->set_asio_route(i, src.kind, src.pair, src.delay);
    if (src.kind != ShmSource::None || played.count(i)) mask |= 1u << i;
    if (cfg.outputs[i].enabled) max_ptime_frames = std::max(max_ptime_frames, ptime_us_to_frames(cfg.outputs[i].ptime_us));
  }
  h.device_active_mask.store(mask);
  h.output_latency_frames.store(max_ptime_frames);
  h.win_src_delay_frames.store(kWinSourceDelayFrames);
}

void Engine::Impl::publish_bridge_request(const Config& cfg, const RuntimeSet& rt) {
  BridgeRequest req;
  for (const auto& [id, slot] : rt.win_slots) req.sources.push_back({slot, id});
  std::sort(req.sources.begin(), req.sources.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
  for (const auto& [dst, src] : cfg.routes) {
    auto d = parse_dest(dst);
    if (!d || d->kind != DestKind::WinOut) continue;
    const ResolvedSource r = resolve(cfg, rt, src);
    req.dests.push_back({dst, r.kind, r.pair, r.delay});
  }
  const std::string body = to_json(req);  // generation 0: compared without it
  if (body == bridge_request) return;
  bridge_request = body;
  req.generation = ++bridge_generation;
  view->write_request(to_json(req));
}

bool Engine::Impl::bridge_alive() const {
  if (!view) return false;
  auto& h = view->header();
  const int64_t now = platform::monotonic_ns();
  const uint64_t hb = h.bridge_heartbeat.load(std::memory_order_acquire);
  std::lock_guard lock(bridge_mutex);
  if (hb != bridge_hb_seen) {
    bridge_hb_seen = hb;
    bridge_hb_changed_ns = now;
  }
  return h.bridge_pid.load() != 0 && hb != 0 && now - bridge_hb_changed_ns < kBridgeStaleNs;
}

std::optional<BridgeReport> Engine::Impl::bridge_report() const {
  if (!bridge_alive()) return std::nullopt;
  return report_from_json(view->read_report(nullptr));
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

std::string Engine::Impl::build_output_sdp(const Config& cfg, int output) const {
  const auto& o = cfg.outputs[output - 1];
  const std::string station = station_name(cfg);
  SdpStream s;
  s.session_name = station.empty() ? output_label(cfg, output) : station + " / " + output_label(cfg, output);
  s.origin_address = current_interface().empty() ? "0.0.0.0" : current_interface();
  s.multicast_address = o.address;
  s.ttl = kMulticastTtl;
  s.port = kRtpPort;
  s.payload_type = 98;
  s.encoding = Encoding::L24;
  s.sample_rate = kSampleRate;
  s.channels = 2;
  s.frames_per_packet = ptime_us_to_frames(o.ptime_us);
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
  // The SDP version only changes when this output's own stream changes, so receivers of the
  // other outputs never see a spurious session update.
  const uint16_t version = sap_hash(s.session_name + "|" + o.address + "|" + std::to_string(o.ptime_us) + "|" + s.ptp_gmid +
                                    "|" + s.origin_address);
  return generate_sdp(s, 0x42414D00u + static_cast<uint64_t>(output), version);
}

ApplyResult Engine::Impl::commit(Config next) {
  ApplyResult r;
  next.password_hash = config.password_hash;
  r.notices = normalize_config(next);
  r.issues = validate_config(next);
  if (!r.issues.empty()) return r;
  std::string err;
  if (!save_config(next, &err)) {
    r.issues.push_back({0, "config", err});
    return r;
  }
  const bool iface_changed = next.interface_ip != config.interface_ip;
  const bool http_changed = next.http_port != config.http_port || next.http_bind != config.http_bind;
  const bool ptp_changed = next.ptp_domain != config.ptp_domain;
  const bool ptp_local_changed = next.ptp_role != config.ptp_role || next.ptp_priority1 != config.ptp_priority1 ||
                                 next.ptp_priority2 != config.ptp_priority2;
  config = next;
  ++config_version;
  if (iface_changed) {
    std::lock_guard il(iface_mutex);
    interface_ip = next.interface_ip.empty() ? platform::default_interface_ip() : next.interface_ip;
  }
  if (running) {
    if (iface_changed) {
      std::lock_guard tl(tx_socket_mutex);
      if (tx_socket) tx_socket->set_multicast_out(current_interface(), kMulticastTtl, true);
    }
    rebuild_runtime(next, iface_changed);
    if (ptp_changed || iface_changed) ptp_running = false;  // ptp loop re-creates its sockets and port
    if (ptp_local_changed) ptp_local_version.fetch_add(1);
  }
  if (http_changed) r.notices.push_back("Zmiana adresu lub portu panelu zacznie działać po ponownym uruchomieniu usługi");
  r.ok = true;
  return r;
}

float Engine::Impl::source_peak(const std::string& source_id, const RuntimeSet& rt, int side) const {
  auto s = parse_source(source_id);
  if (!s) return kSilenceDb;
  switch (s->kind) {
    case SourceKind::NetIn:
      return net_peak[(s->index - 1) * 2 + side].load(std::memory_order_relaxed);
    case SourceKind::AsioOut:
      return asio_peak[(s->index - 1) * 2 + side].load(std::memory_order_relaxed);
    case SourceKind::WinApp:
    case SourceKind::WinLoop:
    case SourceKind::WinIn: {
      auto it = rt.win_slots.find(source_id);
      return it == rt.win_slots.end() ? kSilenceDb : win_peak[it->second * 2 + side].load(std::memory_order_relaxed);
    }
    case SourceKind::None:
      break;
  }
  return kSilenceDb;
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
      f.close();
      std::string err;
      bool migrated = false;
      if (auto loaded = config_from_json(ss.str(), &err, &migrated)) {
        cfg = *loaded;
        if (migrated) {
          const std::string backup = m.opts.config_path + ".v1.bak";
          if (!platform::copy_file(m.opts.config_path, backup)) log_warning("Nie można zapisać kopii " + backup);
          log_info("Konfiguracja 1.0 przeniesiona do formatu 1.1 (kopia: " + backup + ")");
          save_needed = true;
        }
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
    m.host_name = utf8_prefix(platform::computer_name(), kMaxNameBytes);
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

  m.rebuild_runtime(cfg, true);

  m.started_ns = now;
  m.running = true;
  if (m.opts.enable_ptp) m.threads.emplace_back([&m] { m.ptp_loop(); });
  m.threads.emplace_back([&m] { m.rx_loop(); });
  m.threads.emplace_back([&m] { m.tx_loop(); });
  m.threads.emplace_back([&m] { m.period_loop(); });
  if (m.opts.enable_sap) m.threads.emplace_back([&m] { m.sap_loop(); });
  m.threads.emplace_back([&m] { m.monitor_loop(); });
  log_info(std::string(kProductName) + " " + kVersion + " uruchomiony, stacja \"" + m.station_name(cfg) + "\", karta sieciowa " +
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
  std::lock_guard lock(m.config_mutex);
  ApplyResult r = m.commit(std::move(next));
  if (r.ok) log_info("Zastosowano nową konfigurację");
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

ApplyResult Engine::set_route(const std::string& dest, const std::optional<std::string>& source) {
  auto& m = *impl_;
  ApplyResult r;
  const auto parsed_dest = parse_dest(dest);
  if (!parsed_dest) {
    r.issues.push_back({0, "dst", "nieprawidłowy cel routingu: " + dest});
    return r;
  }
  const std::string dest_id = to_id(*parsed_dest);
  if (source && !source->empty() && !parse_source(*source)) {
    r.issues.push_back({0, "src", "nieprawidłowe źródło routingu: " + *source});
    return r;
  }
  std::lock_guard lock(m.config_mutex);
  Config next = m.config;
  if (source && !source->empty()) {
    next.routes[dest_id] = *source;
  } else {
    next.routes.erase(dest_id);
  }
  r = m.commit(std::move(next));
  if (r.ok) {
    auto it = m.config.routes.find(dest_id);
    log_info("Routing: " + dest_id + " ← " + (it != m.config.routes.end() ? it->second : std::string("(brak)")));
  }
  return r;
}

ApplyResult Engine::add_subscription(const std::string& sdp_or_sap_id) {
  auto& m = *impl_;
  ApplyResult r;
  std::string sdp = sdp_or_sap_id;
  if (sdp.rfind("v=", 0) != 0) {
    std::lock_guard lock(m.sap_mutex);
    sdp.clear();
    for (const auto& st : m.directory.list())
      if (st.id == sdp_or_sap_id) sdp = st.sdp;
    if (sdp.empty()) {
      r.issues.push_back({0, "sap_id", "nie znaleziono strumienia " + sdp_or_sap_id + " w SAP"});
      return r;
    }
  }
  auto parsed = parse_sdp(sdp);
  if (!parsed.stream) {
    r.issues.push_back({0, "sdp", parsed.error});
    return r;
  }

  std::lock_guard lock(m.config_mutex);
  Config next = m.config;
  int slot = 0;
  for (int i = 0; i < kDeviceCount && !slot; ++i)
    if (!next.subscriptions[i].enabled && next.subscriptions[i].sdp.empty()) slot = i + 1;
  for (int i = 0; i < kDeviceCount && !slot; ++i)
    if (!next.subscriptions[i].enabled) slot = i + 1;
  if (!slot) {
    r.issues.push_back({0, "subscriptions", "wszystkie 32 subskrypcje są zajęte"});
    return r;
  }
  auto& sub = next.subscriptions[slot - 1];
  sub = SubscriptionConfig{};
  sub.enabled = true;
  sub.sdp = sdp;
  sub.name = utf8_prefix(parsed.stream->session_name, kMaxNameBytes);
  r = m.commit(std::move(next));
  if (r.ok) {
    r.created = slot;
    log_info("Dodano subskrypcję " + two_digits(slot) + ": " + parsed.stream->session_name);
  }
  return r;
}

ApplyResult Engine::remove_subscription(int index) {
  auto& m = *impl_;
  ApplyResult r;
  if (index < 1 || index > kDeviceCount) {
    r.issues.push_back({0, "subscriptions", "brak subskrypcji " + std::to_string(index)});
    return r;
  }
  std::lock_guard lock(m.config_mutex);
  Config next = m.config;
  next.subscriptions[index - 1] = SubscriptionConfig{};
  const std::string id = "net-in:" + std::to_string(index);
  for (auto it = next.routes.begin(); it != next.routes.end();) {
    if (it->second == id) {
      it = next.routes.erase(it);
    } else {
      ++it;
    }
  }
  r = m.commit(std::move(next));
  if (r.ok) log_info("Usunięto subskrypcję " + two_digits(index));
  return r;
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
  s.ptp_role = cfg.ptp_role;
  s.ptp_enabled = m.opts.enable_ptp;
  s.ptp_free_run = !m.ptp_ever_locked;
  s.interface_ip = m.current_interface();
  s.station_name = m.station_name(cfg);
  s.version = kVersion;
  s.uptime_s = m.running ? (now - m.started_ns) / 1'000'000'000 : 0;
  if (m.view) {
    auto& h = m.view->header();
    s.asio_connected = h.client_pid.load() != 0 && h.client_running.load() != 0;
    s.asio_buffer_frames = h.client_pid.load() != 0 ? h.buffer_frames.load() : 0;
  }
  s.bridge_connected = m.bridge_alive();
  const bool client_recent = now - m.last_client_active_ns.load() < 1'000'000'000;
  std::vector<DiscoveredStream> discovered;
  {
    std::lock_guard lock(m.sap_mutex);
    discovered = m.directory.list();
  }

  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& o = cfg.outputs[i];
    OutputStatus os;
    os.index = i + 1;
    os.enabled = o.enabled;
    os.name = output_label(cfg, i + 1);
    os.address = o.address;
    os.ptime_us = o.ptime_us;
    if (auto it = cfg.routes.find("net-out:" + std::to_string(i + 1)); it != cfg.routes.end()) os.source = it->second;
    for (const auto& e : snap->tx) {
      if (e->output != os.index) continue;
      os.tx = e->stream->stats();
      os.tx_active = e->keep_active || client_recent || !e->follows_asio;
    }
    if (o.enabled) {
      for (const auto& st : discovered) {
        if (st.parsed.multicast_address == o.address && st.origin != s.interface_ip)
          os.conflict = "adres " + o.address + " ogłasza też " + st.origin + " (" + st.name + ")";
      }
    }
    for (int c = 0; c < 2; ++c) os.peak_db[c] = os.source.empty() ? kSilenceDb : m.source_peak(os.source, *snap, c);
    s.outputs.push_back(std::move(os));
  }

  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& sub = cfg.subscriptions[i];
    SubscriptionStatus ss;
    ss.index = i + 1;
    ss.enabled = sub.enabled;
    ss.name = sub.name;
    ss.latency_us = sub.latency_us;
    for (const auto& e : snap->rx) {
      if (e->subscription != ss.index) continue;
      ss.error = e->error;
      ss.address = e->group;
      if (ss.name.empty()) ss.name = e->source_name;
      if (e->stream) {
        ss.rx = e->stream->stats();
        ss.signal = e->stream->has_signal(now);
      }
    }
    for (int c = 0; c < 2; ++c) ss.peak_db[c] = m.net_peak[i * 2 + c].load(std::memory_order_relaxed);
    s.subscriptions.push_back(std::move(ss));
  }
  return s;
}

RoutingView Engine::routing() const {
  const auto& m = *impl_;
  RoutingView v;
  const Config cfg = config();
  const auto snap = m.snapshot();
  v.routes = cfg.routes;

  auto peaks = [&](RouteNode& n, const std::string& source) {
    for (int c = 0; c < 2; ++c) n.peak_db[c] = source.empty() ? kSilenceDb : m.source_peak(source, *snap, c);
  };

  // ---- sources ----
  for (int i = 0; i < kDeviceCount; ++i) {
    const auto& sub = cfg.subscriptions[i];
    if (!sub.enabled) continue;
    const std::string id = "net-in:" + std::to_string(i + 1);
    const ResolvedSource r = m.resolve(cfg, *snap, id);
    RouteNode n{id, sub.name.empty() ? "Subskrypcja " + two_digits(i + 1) : sub.name, "net", r.available, r.note, {}, {}};
    peaks(n, id);
    v.sources.push_back(std::move(n));
  }
  for (int i = 0; i < kDeviceCount; ++i) {
    const std::string id = "asio-out:" + std::to_string(i + 1);
    RouteNode n{id, asio_label(cfg, i + 1), "asio", true, {}, {}, {}};
    peaks(n, id);
    v.sources.push_back(std::move(n));
  }
  // Windows: what the bridge reports in this user session, plus anything routed that it does not know
  const auto report = m.bridge_report();
  const bool bridge = report.has_value();
  const std::string no_bridge = "Audio Windows: brak sesji użytkownika";
  std::map<std::string, const BridgeSlotState*> slot_state;
  if (report)
    for (const auto& st : report->slots) slot_state[st.id] = &st;
  std::vector<RouteNode> win_sources, win_dests;
  std::set<std::string> listed;
  auto win_node = [&](const std::string& id, const std::string& name) {
    RouteNode n{id, name, "win", bridge, bridge ? std::string() : no_bridge, {}, {}};
    if (auto it = slot_state.find(id); it != slot_state.end() && !it->second->ok) {
      n.available = false;
      n.note = it->second->reason;
    }
    return n;
  };
  if (report) {
    for (const auto& a : report->apps) {
      const std::string id = "win-app:" + a.exe;
      if (!listed.insert(id).second) continue;
      win_sources.push_back(win_node(id, a.name.empty() ? a.exe : a.name));
    }
    // Virtual sound cards: applications play into the render end, so its loopback is the source
    // and the render end the destination; the capture end of a loop cable carries the same audio
    // later and is listed only while something is routed from it.
    std::set<std::string> cable_render_ends;
    for (const auto& e : report->endpoints)
      if (auto vd = classify_virtual_device(e.name); vd && e.render && !vd->pair_key.empty())
        cable_render_ends.insert(vd->pair_key);
    std::set<std::string> routed_sources;
    for (const auto& [dst, src] : cfg.routes) routed_sources.insert(src);
    for (const auto& e : report->endpoints) {
      const auto vd = classify_virtual_device(e.name);
      const std::string label = vd && !vd->cable.empty() ? vd->cable : e.name;
      auto tagged = [&](RouteNode n) {
        if (vd) n.virtual_device = vd->family;
        return n;
      };
      if (e.render) {
        win_sources.push_back(tagged(win_node("win-loop:" + e.id, vd ? label + ": z aplikacji" : "Odsłuch: " + e.name)));
        listed.insert("win-loop:" + e.id);
        win_dests.push_back(tagged(win_node("win-out:" + e.id, vd && !vd->cable.empty() ? label + ": do aplikacji" : e.name)));
        listed.insert("win-out:" + e.id);
      } else {
        const std::string id = "win-in:" + e.id;
        if (vd && cable_render_ends.count(vd->pair_key) && !routed_sources.count(id)) continue;
        win_sources.push_back(tagged(win_node(id, vd && !vd->cable.empty() ? label + ": wyjście kabla" : e.name)));
        listed.insert(id);
      }
    }
  }
  for (const auto& [dst, src] : cfg.routes) {
    if (auto sp = parse_source(src); sp && is_windows(*sp) && listed.insert(src).second) {
      RouteNode n = win_node(src, sp->key);
      if (bridge && n.available) {
        n.available = false;
        n.note = sp->kind == SourceKind::WinApp ? "aplikacja nie działa" : "urządzenie niedostępne";
      }
      win_sources.push_back(std::move(n));
    }
    if (auto dp = parse_dest(dst); dp && is_windows(*dp) && listed.insert(dst).second) {
      RouteNode n = win_node(dst, dp->key);
      if (bridge) {
        n.available = false;
        n.note = "urządzenie niedostępne";
      }
      win_dests.push_back(std::move(n));
    }
  }
  // virtual sound cards first: they are what a studio routes most
  auto virtual_first = [](std::vector<RouteNode>& list) {
    std::stable_partition(list.begin(), list.end(), [](const RouteNode& n) { return !n.virtual_device.empty(); });
  };
  virtual_first(win_sources);
  virtual_first(win_dests);
  for (auto& n : win_sources) {
    peaks(n, n.id);
    v.sources.push_back(std::move(n));
  }

  // ---- destinations ----
  for (int i = 0; i < kDeviceCount; ++i) {
    if (!cfg.outputs[i].enabled) continue;
    const std::string id = "net-out:" + std::to_string(i + 1);
    RouteNode n{id, output_label(cfg, i + 1), "net", true, cfg.outputs[i].address, {}, {}};
    auto it = cfg.routes.find(id);
    peaks(n, it == cfg.routes.end() ? std::string() : it->second);
    v.dests.push_back(std::move(n));
  }
  for (int i = 0; i < kDeviceCount; ++i) {
    const std::string id = "asio-in:" + std::to_string(i + 1);
    RouteNode n{id, asio_label(cfg, i + 1), "asio", true, {}, {}, {}};
    auto it = cfg.routes.find(id);
    peaks(n, it == cfg.routes.end() ? std::string() : it->second);
    v.dests.push_back(std::move(n));
  }
  for (auto& n : win_dests) {
    auto it = cfg.routes.find(n.id);
    peaks(n, it == cfg.routes.end() ? std::string() : it->second);
    v.dests.push_back(std::move(n));
  }
  return v;
}

std::vector<DiscoveredStream> Engine::discovered_streams() const {
  std::lock_guard lock(impl_->sap_mutex);
  return impl_->directory.list();
}

std::string Engine::output_sdp(int output) const {
  if (output < 1 || output > kDeviceCount) return {};
  return impl_->build_output_sdp(config(), output);
}

void Engine::reset_stats() {
  const auto snap = impl_->snapshot();
  for (const auto& e : snap->rx)
    if (e->stream) e->stream->reset_stats();
  for (const auto& e : snap->tx) e->stream->reset_stats();
  log_info("Liczniki statystyk wyzerowane");
}

}  // namespace bam
