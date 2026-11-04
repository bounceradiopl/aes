// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bam/constants.hpp"

namespace bam {

inline constexpr int kConfigVersion = 2;
inline constexpr int kMaxWindowsRoutes = 16;  // Windows sources and Windows destinations, each
inline constexpr int kMaxMixes = 32;
inline constexpr int kMaxMixSources = 16;
inline constexpr double kMixGainMinDb = -60.0;
inline constexpr double kMixGainMaxDb = 12.0;

// One member of a mix (a destination fed by several sources).
struct MixSource {
  std::string source;  // route source id
  double gain_db = 0;
  bool mute = false;
};

struct MixConfig {
  std::vector<MixSource> sources;
};

// A stereo AoIP stream this computer transmits.
struct OutputConfig {
  bool enabled = false;
  std::string name;
  std::string address;
  uint32_t ptime_us = 1000;  // 1000, 1333 or 4000
  bool keep_active = true;   // Axia "KEEP CHANNEL ACTIVE"
};

// A stereo AoIP stream this computer receives.
struct SubscriptionConfig {
  bool enabled = false;
  std::string name;
  std::string sdp;
  uint32_t latency_us = 5000;
};

enum class PtpRole { Auto, Preferred, Slave };

struct Config {
  std::string station_name;  // shown in SDP session names; empty = computer name
  std::string interface_ip;  // empty = first non-loopback IPv4 interface
  int ptp_domain = 0;
  PtpRole ptp_role = PtpRole::Auto;
  int ptp_priority1 = 250;
  int ptp_priority2 = 128;
  int pc_nominal_level_dbfs = -20;  // Axia "PC Audio Nominal Level"; -20 = 0 dB
  std::string http_bind = "0.0.0.0";
  uint16_t http_port = kDefaultHttpPort;
  std::string password_hash;
  std::array<OutputConfig, kDeviceCount> outputs;
  std::array<SubscriptionConfig, kDeviceCount> subscriptions;
  std::array<std::string, kDeviceCount> asio_names;  // pair names shown to ASIO hosts
  std::map<std::string, std::string> routes;         // destination id -> source id (see routing.hpp)
  std::map<std::string, MixConfig> mixes;            // destination id -> mix; such a destination is not in routes
};

struct ConfigIssue {
  int device = 0;  // 0 = global setting, 1..32 = output / subscription / ASIO pair
  std::string field;
  std::string message;  // Polish, user facing
};

Config default_config();
std::string config_to_json(const Config& c);
// Accepts version 2 and migrates the 1.0 format (no "version" key, "devices" array).
std::optional<Config> config_from_json(std::string_view text, std::string* error, bool* migrated = nullptr);
std::vector<ConfigIssue> validate_config(const Config& c);
// Raises subscription latencies below "source ptime + 1 ms". Returns Polish notices describing each change.
std::vector<std::string> normalize_config(Config& c);

const char* ptp_role_name(PtpRole r);
std::optional<PtpRole> parse_ptp_role(std::string_view s);
uint32_t ptime_us_to_frames(uint32_t us);
double gain_to_app(const Config& c);
double gain_to_network(const Config& c);

}  // namespace bam
