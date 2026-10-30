// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "bam/config.hpp"
#include "bam/ptp_slave.hpp"
#include "bam/rx_stream.hpp"
#include "bam/sap.hpp"
#include "bam/shm_layout.hpp"
#include "bam/tx_stream.hpp"

namespace bam {

struct DeviceStatus {
  int index = 0;  // 1..32
  bool enabled = false;
  std::string name;
  std::string tx_address;
  bool tx_active = false;
  bool rx_configured = false;
  bool rx_signal = false;
  std::string rx_source_name;
  std::string rx_error;
  std::string tx_conflict;
  RxStats rx;
  TxStats tx;
  float in_peak_db[2] = {-120, -120};
  float out_peak_db[2] = {-120, -120};
  uint32_t rx_latency_us = 0;
  uint32_t tx_ptime_us = 0;
};

struct EngineStatus {
  PtpStatus ptp;
  bool ptp_enabled = true;
  bool ptp_free_run = true;  // no PTP time yet: running on the local clock
  bool asio_connected = false;
  uint32_t asio_buffer_frames = 0;
  int64_t uptime_s = 0;
  std::string interface_ip;
  std::string version;
  std::vector<DeviceStatus> devices;
};

struct ApplyResult {
  bool ok = false;
  std::vector<ConfigIssue> issues;
  std::vector<std::string> notices;
};

struct EngineOptions {
  std::string config_path;
  std::string shm_name = kShmName;
  std::string event_name = kPeriodEventName;
  bool enable_ptp = true;
  bool enable_sap = true;
};

// The whole service: PTP slave, 32 RX + 32 TX AES67 streams, ASIO period clock, SAP, config.
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
  std::string output_sdp(int device) const;  // device 1..32
  void reset_stats();

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace bam
