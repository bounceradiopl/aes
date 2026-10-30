// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bam/bridge_protocol.hpp"
#include "bam/clock.hpp"
#include "bam/engine.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/udp.hpp"
#include "bam/routing.hpp"

namespace bam {

inline constexpr uint32_t kWinSourceDelayFrames = 960;  // D_src of Windows sources: 20 ms bridge buffer
inline constexpr int64_t kBridgeStaleNs = 2'000'000'000;  // bridge heartbeat older than this: no user session

// One received AES67 subscription, written to net_in(2n-2, 2n-1).
struct RxEntry {
  int subscription = 0;  // 1..32
  std::string key;       // parameters that require a rebuild when changed
  std::string source_name;
  std::string error;
  std::string group;   // multicast address the stream is received on
  std::string source;  // sender address from a=source-filter, may be empty
  uint16_t port = 0;
  std::unique_ptr<RxStream> stream;
};

// A receive socket for up to 16 subscriptions on one UDP port. Datagrams are dispatched to the
// streams by their destination group (and sender, when filtered), because on Windows and Linux a
// socket can see every group any socket joined on that port.
struct RxPort {
  uint16_t port = 0;
  std::string key;  // interface + sorted memberships; a different key means a new socket
  platform::UdpSocket socket;
  std::vector<std::shared_ptr<RxEntry>> entries;
};

// Where a destination reads its audio: a ring pair on the shared timeline and its readiness delay.
struct ResolvedSource {
  ShmSource kind = ShmSource::None;
  int pair = 0;           // 0-based pair index within the kind's rings
  uint32_t delay = 0;     // D_src in frames
  std::string id;         // route source id, empty when unrouted
  bool available = false;
  std::string note;       // why it is unavailable
};

// One AoIP output, reading its routed source.
struct TxEntry {
  int output = 0;  // 1..32
  std::string key;
  std::string address;
  std::atomic<bool> keep_active{true};
  bool follows_asio = true;  // source is asio-out (or none): transmits only while an ASIO host runs, unless keep_active
  std::unique_ptr<TxStream> stream;
};

// Immutable snapshot of the streams; swapped as a whole when the configuration changes.
struct RuntimeSet {
  std::vector<std::shared_ptr<RxEntry>> rx;
  std::vector<std::shared_ptr<RxPort>> rx_ports;
  std::vector<std::shared_ptr<TxEntry>> tx;
  std::map<std::string, int> win_slots;  // Windows source id -> win_src pair 0..15
};

struct Engine::Impl {
  EngineOptions opts;

  mutable std::mutex config_mutex;
  Config config;
  uint64_t config_version = 1;

  std::optional<platform::SharedMemory> shm;
  std::optional<platform::NamedEvent> period_event;
  std::unique_ptr<ShmView> view;

  ClockSource clock;
  ClockIdentity self_identity;
  mutable std::mutex ptp_mutex;
  PtpStatus ptp_status;
  std::atomic<bool> ptp_ever_locked{false};
  std::atomic<bool> ptp_running{false};
  std::atomic<uint64_t> ptp_local_version{0};  // role / priorities changed: PtpPort::set_local

  mutable std::mutex runtime_mutex;
  std::shared_ptr<const RuntimeSet> runtime = std::make_shared<RuntimeSet>();
  std::atomic<uint64_t> runtime_version{0};

  std::mutex tx_socket_mutex;
  std::optional<platform::UdpSocket> tx_socket;
  std::array<uint32_t, kDeviceCount> tx_ssrc{};  // stable per output, so a route change keeps the RTP identity

  mutable std::mutex sap_mutex;
  SapDirectory directory;
  std::atomic<uint64_t> sap_dirty{1};

  mutable std::mutex iface_mutex;
  std::string interface_ip;
  std::string host_name;

  // Meters per source ring, read at "now - D_src" by the network thread.
  std::array<std::atomic<float>, kChannelCount> net_peak;
  std::array<std::atomic<float>, kChannelCount> asio_peak;
  std::array<std::atomic<float>, kWinChannels> win_peak;
  std::array<std::atomic<uint32_t>, kDeviceCount> net_delay;  // subscription D_src for the meters
  std::atomic<int64_t> last_client_active_ns{0};

  // audio bridge (user session process)
  uint64_t bridge_generation = 0;  // config_mutex
  std::string bridge_request;      // last request body without the generation; config_mutex
  mutable std::mutex bridge_mutex;
  mutable uint64_t bridge_hb_seen = 0;
  mutable int64_t bridge_hb_changed_ns = 0;

  std::atomic<bool> running{false};
  int64_t started_ns = 0;
  std::vector<std::thread> threads;

  explicit Impl(EngineOptions o);

  // engine.cpp
  std::string current_interface() const;
  std::string station_name(const Config& cfg) const;
  std::shared_ptr<const RuntimeSet> snapshot() const;
  ResolvedSource resolve(const Config& cfg, const RuntimeSet& rt, const std::string& source_id) const;
  void rebuild_runtime(const Config& cfg, bool force);
  void publish_shared_config(const Config& cfg, const RuntimeSet& rt);
  bool save_config(const Config& cfg, std::string* error) const;
  std::string build_output_sdp(const Config& cfg, int output) const;
  ApplyResult commit(Config next);  // validate, save, apply; config_mutex held by the caller
  float source_peak(const std::string& source_id, const RuntimeSet& rt, int side) const;
  void publish_bridge_request(const Config& cfg, const RuntimeSet& rt);
  bool bridge_alive() const;
  std::optional<BridgeReport> bridge_report() const;  // only while the bridge is alive

  // workers
  void ptp_loop();
  void rx_loop();
  void tx_loop();
  void period_loop();
  void sap_loop();
  void monitor_loop();
};

}  // namespace bam
