// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bam/config.hpp"
#include "bam/ptp_slave.hpp"
#include "bam/rx_stream.hpp"
#include "bam/sap.hpp"
#include "bam/shm_layout.hpp"
#include "bam/tx_stream.hpp"

namespace bam {

struct OutputStatus {
  int index = 0;  // 1..32
  bool enabled = false;
  std::string name;
  std::string address;
  uint32_t ptime_us = 0;
  std::string source;  // routed source id, empty when unrouted
  bool tx_active = false;
  std::string conflict;
  TxStats tx;
  float peak_db[2] = {-120, -120};
};

struct SubscriptionStatus {
  int index = 0;  // 1..32
  bool enabled = false;
  std::string name;
  std::string address;
  bool signal = false;
  std::string error;
  uint32_t latency_us = 0;
  RxStats rx;
  float peak_db[2] = {-120, -120};
};

struct EngineStatus {
  PtpStatus ptp;
  PtpRole ptp_role = PtpRole::Auto;
  bool ptp_enabled = true;
  bool ptp_free_run = true;  // no PTP time yet: running on the local clock
  bool asio_connected = false;
  uint32_t asio_buffer_frames = 0;
  bool bridge_connected = false;  // Windows audio bridge running in a user session
  int64_t uptime_s = 0;
  std::string interface_ip;
  std::string station_name;
  std::string version;
  std::vector<OutputStatus> outputs;
  std::vector<SubscriptionStatus> subscriptions;
};

// One row or column of the routing matrix.
struct RouteNode {
  std::string id;
  std::string name;
  std::string group;  // "net", "asio", "win"
  bool available = true;
  std::string note;   // why it is unavailable, or extra info
  float peak_db[2] = {-120, -120};
  std::string virtual_device;  // family of a recognised virtual sound card ("VB-CABLE"), else empty
};

struct RoutingView {
  std::vector<RouteNode> sources;
  std::vector<RouteNode> dests;
  std::map<std::string, std::string> routes;  // dest id -> source id
};

struct ApplyResult {
  bool ok = false;
  std::vector<ConfigIssue> issues;
  std::vector<std::string> notices;
  int created = 0;  // subscription slot created by add_subscription (1..32)
};

struct EngineOptions {
  std::string config_path;
  std::string shm_name = kShmName;
  std::string event_name = kPeriodEventName;
  bool enable_ptp = true;
  bool enable_sap = true;
};

// The whole service: PTP port (slave or master), up to 32 subscriptions and 32 outputs,
// the routing matrix, the ASIO period clock, SAP and the configuration.
class Engine {
 public:
  explicit Engine(EngineOptions opts);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  bool start(std::string* error);
  void stop();

  Config config() const;
  ApplyResult apply_config(Config next);  // password_hash is kept from the current config
  bool set_password_hash(const std::string& hash);
  EngineStatus status() const;
  std::vector<DiscoveredStream> discovered_streams() const;
  std::string output_sdp(int output) const;  // output 1..32

  RoutingView routing() const;
  ApplyResult set_route(const std::string& dest, const std::optional<std::string>& source);
  ApplyResult add_subscription(const std::string& sdp_or_sap_id);
  ApplyResult remove_subscription(int index);

  void reset_stats();

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace bam
