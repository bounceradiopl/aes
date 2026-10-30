// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/routing.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "bam/constants.hpp"

namespace bam {

namespace {
std::optional<int> parse_index(std::string_view s) {
  int v = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v < 1 || v > kDeviceCount) return std::nullopt;
  return v;
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::pair<std::string_view, std::string_view> split(std::string_view id) {
  const auto colon = id.find(':');
  if (colon == std::string_view::npos) return {id, {}};
  return {id.substr(0, colon), id.substr(colon + 1)};
}
}  // namespace

std::optional<RouteSource> parse_source(std::string_view id) {
  auto [prefix, rest] = split(id);
  RouteSource s;
  if (prefix == "net-in" || prefix == "asio-out") {
    auto idx = parse_index(rest);
    if (!idx) return std::nullopt;
    s.kind = prefix == "net-in" ? SourceKind::NetIn : SourceKind::AsioOut;
    s.index = *idx;
    return s;
  }
  if (rest.empty()) return std::nullopt;
  if (prefix == "win-app") {
    s.kind = SourceKind::WinApp;
    s.key = lower(rest);  // executables match case-insensitively
  } else if (prefix == "win-loop") {
    s.kind = SourceKind::WinLoop;
    s.key = std::string(rest);
  } else if (prefix == "win-in") {
    s.kind = SourceKind::WinIn;
    s.key = std::string(rest);
  } else {
    return std::nullopt;
  }
  return s;
}

std::optional<RouteDest> parse_dest(std::string_view id) {
  auto [prefix, rest] = split(id);
  RouteDest d;
  if (prefix == "net-out" || prefix == "asio-in") {
    auto idx = parse_index(rest);
    if (!idx) return std::nullopt;
    d.kind = prefix == "net-out" ? DestKind::NetOut : DestKind::AsioIn;
    d.index = *idx;
    return d;
  }
  if (prefix == "win-out" && !rest.empty()) {
    d.kind = DestKind::WinOut;
    d.key = std::string(rest);
    return d;
  }
  return std::nullopt;
}

std::string to_id(const RouteSource& s) {
  switch (s.kind) {
    case SourceKind::NetIn:
      return "net-in:" + std::to_string(s.index);
    case SourceKind::AsioOut:
      return "asio-out:" + std::to_string(s.index);
    case SourceKind::WinApp:
      return "win-app:" + s.key;
    case SourceKind::WinLoop:
      return "win-loop:" + s.key;
    case SourceKind::WinIn:
      return "win-in:" + s.key;
    case SourceKind::None:
      break;
  }
  return {};
}

std::string to_id(const RouteDest& d) {
  switch (d.kind) {
    case DestKind::NetOut:
      return "net-out:" + std::to_string(d.index);
    case DestKind::AsioIn:
      return "asio-in:" + std::to_string(d.index);
    case DestKind::WinOut:
      return "win-out:" + d.key;
  }
  return {};
}

bool is_windows(const RouteSource& s) {
  return s.kind == SourceKind::WinApp || s.kind == SourceKind::WinLoop || s.kind == SourceKind::WinIn;
}

bool is_windows(const RouteDest& d) { return d.kind == DestKind::WinOut; }

}  // namespace bam
