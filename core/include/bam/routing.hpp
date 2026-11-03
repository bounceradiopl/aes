// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace bam {

// Route endpoints, written as ids: "net-in:3", "asio-out:1", "win-app:chrome.exe", "win-out:{0.0.0.00000000}.{guid}".
enum class SourceKind { None, NetIn, AsioOut, WinApp, WinLoop, WinIn };
enum class DestKind { NetOut, AsioIn, WinOut };

struct RouteSource {
  SourceKind kind = SourceKind::None;
  int index = 0;    // 1..32 for net-in / asio-out
  std::string key;  // Windows endpoint id or lower-case executable name
};

struct RouteDest {
  DestKind kind = DestKind::NetOut;
  int index = 0;    // 1..32 for net-out / asio-in
  std::string key;  // Windows endpoint id
  int pair = 1;     // Windows device channel pair: 1 = channels 1-2
};

std::optional<RouteSource> parse_source(std::string_view id);
std::optional<RouteDest> parse_dest(std::string_view id);
std::string to_id(const RouteSource& s);
std::string to_id(const RouteDest& d);
bool is_windows(const RouteSource& s);
bool is_windows(const RouteDest& d);

}  // namespace bam
