// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bam/constants.hpp"

namespace bam {

// One "BounceAES NN" device: a stereo output stream and a stereo input stream (Axia: one sound card).
struct DeviceConfig {
  bool enabled = false;
  std::string name;
  std::string tx_address;
  uint32_t tx_ptime_us = 1000;  // 1000, 1333 or 4000
  bool keep_active = true;      // Axia "KEEP CHANNEL ACTIVE"
  std::string rx_sdp;           // empty = no input source
  uint32_t rx_latency_us = 5000;
};

struct Config {
  std::string interface_ip;  // empty = first non-loopback IPv4 interface
  int ptp_domain = 0;
  int pc_nominal_level_dbfs = -20;  // Axia "PC Audio Nominal Level"; -20 = 0 dB
  std::string http_bind = "0.0.0.0";
  uint16_t http_port = kDefaultHttpPort;
  std::string password_hash;  // empty = panel password not set yet
  std::array<DeviceConfig, kDeviceCount> devices;
};

struct ConfigIssue {
  int device = 0;  // 0 = global setting, 1..32 = device
  std::string field;
  std::string message;  // Polish, user facing
};

Config default_config();
std::string config_to_json(const Config& c);
std::optional<Config> config_from_json(std::string_view text, std::string* error);
std::vector<ConfigIssue> validate_config(const Config& c);
// Raises rx latencies below "source ptime + 1 ms". Returns Polish notices describing each change.
std::vector<std::string> normalize_config(Config& c);

uint32_t ptime_us_to_frames(uint32_t us);
double gain_to_app(const Config& c);
double gain_to_network(const Config& c);

}  // namespace bam
