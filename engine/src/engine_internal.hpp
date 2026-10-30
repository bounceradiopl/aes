// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bam/clock.hpp"
#include "bam/engine.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/udp.hpp"

namespace bam {

struct RxEntry {
  int device = 0;   // 1..32
  std::string key;  // parameters that require a rebuild when changed
  std::string source_name;
  std::string error;
  std::string group;   // multicast address the stream is received on
  std::string source;  // sender address from a=source-filter, may be empty
  uint16_t port = 0;
  std::unique_ptr<RxStream> stream;
};

// One receive socket per UDP port, shared by every input stream on that port. Datagrams are
// dispatched to the streams by their destination group (and sender, when filtered), because on
// Windows and Linux a socket sees every group any socket joined on that port.
struct RxPort {
  uint16_t port = 0;
  std::string key;  // interface + sorted memberships; a different key means a new socket
  platform::UdpSocket socket;
  std::vector<std::shared_ptr<RxEntry>> entries;
};

struct TxEntry {
  int device = 0;
  std::string key;
  std::string address;
  std::atomic<bool> keep_active{true};
  std::unique_ptr<TxStream> stream;
};

// Immutable snapshot of the streams; swapped as a whole when the configuration changes.
struct RuntimeSet {
  std::vector<std::shared_ptr<RxEntry>> rx;
  std::vector<std::shared_ptr<RxPort>> rx_ports;
  std::vector<std::shared_ptr<TxEntry>> tx;
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

  mutable std::mutex runtime_mutex;
  std::shared_ptr<const RuntimeSet> runtime = std::make_shared<RuntimeSet>();
  std::atomic<uint64_t> runtime_version{0};

  std::mutex tx_socket_mutex;
  std::optional<platform::UdpSocket> tx_socket;

  mutable std::mutex sap_mutex;
  SapDirectory directory;
  std::atomic<uint64_t> sap_dirty{1};

  mutable std::mutex iface_mutex;
  std::string interface_ip;

  std::array<std::atomic<float>, kChannelCount> in_peak;
  std::array<std::atomic<float>, kChannelCount> out_peak;
  std::atomic<int64_t> last_client_active_ns{0};

  std::atomic<bool> running{false};
  int64_t started_ns = 0;
  std::vector<std::thread> threads;

  explicit Impl(EngineOptions o);

  // engine.cpp
  std::string current_interface() const;
  std::shared_ptr<const RuntimeSet> snapshot() const;
  void rebuild_runtime(const Config& cfg, bool force);
  void publish_shared_config(const Config& cfg);
  bool save_config(const Config& cfg, std::string* error) const;
  std::string build_output_sdp(const Config& cfg, int device) const;

  // workers
  void ptp_loop();
  void rx_loop();
  void tx_loop();
  void period_loop();
  void sap_loop();
  void monitor_loop();
};

}  // namespace bam
