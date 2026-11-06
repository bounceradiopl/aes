// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bam {

class Engine;

struct PanelAsset {
  const char* path;  // "/index.html"
  const char* mime;
  const unsigned char* data;
  size_t size;
};
extern const PanelAsset kPanelAssets[];
extern const size_t kPanelAssetCount;

// REST API + SSE live feed + embedded web panel (Polish UI, Axia-style layout). Since 1.2.2 with
// the request hardening shared with Bounce Encoder (bam/http/guard.hpp): one request per
// connection, JSON-only bodies of at most 1 MB, the session checked after the body was read, the
// first password only through a loopback Host, failed logins throttled (HTTP 429).
class HttpServer {
 public:
  explicit HttpServer(Engine& engine);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  // Port 0 picks a free port (tests); port() tells which. false with a Polish *error when the
  // address cannot be bound.
  bool start(const std::string& bind, uint16_t port, std::string* error);
  void stop();
  uint16_t port() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

namespace testing {
// Test hook: shifts the clock every HttpServer uses for sessions and login throttling.
void set_http_clock_offset_ns(int64_t offset_ns);
// Test hook: runs inside a login after the password was verified, before the session is created
// (empty = none).
void set_login_verified_hook(std::function<void()> hook);
}  // namespace testing

}  // namespace bam
