// SPDX-License-Identifier: GPL-3.0-or-later
// Raw HTTP helpers for the panel attack tests (Bounce Encoder and BounceAES Master): requests sent
// byte for byte to a server on 127.0.0.1, and what comes back on that one connection.
#pragma once
#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"

namespace http_attack {

inline bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
  const int64_t end = bam::platform::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (bam::platform::monotonic_ns() < end) {
    if (pred()) return true;
    bam::platform::sleep_ns(20'000'000);
  }
  return pred();
}

// Sends `request` as it is and returns the status of the response that arrives within
// `timeout_ms` (0 = none).
inline int raw_status(uint16_t port, const std::string& request, int timeout_ms = 3000) {
  std::string err;
  auto s = bam::platform::TcpStream::connect("127.0.0.1", port, 2000, &err);
  REQUIRE_MESSAGE(s, err);
  REQUIRE(s->send_all({reinterpret_cast<const uint8_t*>(request.data()), request.size()}, 2000));
  std::string got;
  uint8_t buf[4096];
  const int64_t end = bam::platform::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (got.find("\r\n\r\n") == std::string::npos && bam::platform::monotonic_ns() < end) {
    const int n = s->recv(buf, 100);
    if (n == 0) break;
    if (n > 0) got.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
  }
  if (got.rfind("HTTP/1.1 ", 0) != 0) return 0;
  return std::stoi(got.substr(9, 3));
}

// Everything the server answers to `request` on one connection, until it closes the connection or
// `timeout_ms` pass.
struct RawExchange {
  std::vector<int> statuses;  // every response status line, in order
  bool closed = false;        // the server closed the connection
  std::string text;
};
inline RawExchange raw_exchange(uint16_t port, const std::string& request, int timeout_ms = 3000) {
  RawExchange x;
  std::string err;
  auto s = bam::platform::TcpStream::connect("127.0.0.1", port, 2000, &err);
  REQUIRE_MESSAGE(s, err);
  size_t sent = 0;
  uint8_t buf[8192];
  const int64_t end = bam::platform::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (bam::platform::monotonic_ns() < end) {
    if (sent < request.size()) {
      const int n = s->send_some({reinterpret_cast<const uint8_t*>(request.data()) + sent, request.size() - sent}, 10);
      if (n > 0) sent += static_cast<size_t>(n);
    }
    const int n = s->recv_some(buf, 10);
    if (n < 0) {
      x.closed = true;
      break;
    }
    x.text.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
  }
  // a response follows the previous body directly; no body here contains "HTTP/1.1 "
  for (size_t pos = 0; (pos = x.text.find("HTTP/1.1 ", pos)) != std::string::npos; pos += 9)
    if (pos + 12 <= x.text.size()) x.statuses.push_back(std::stoi(x.text.substr(pos + 9, 3)));
  return x;
}

// A request answered while bytes it sent are still unread (a body refused on its headers, or a
// request after the first one): the server closes after the answer (one request per connection),
// and Windows answers unread bytes at close with a reset that can destroy the answer before the
// client reads it. There, "no answer" counts as refused too; the security checks around these
// calls (nothing executed, the connection closed) are the same on every platform.
#ifdef _WIN32
inline constexpr bool kResetMayLoseAnswer = true;
#else
inline constexpr bool kResetMayLoseAnswer = false;
#endif
inline bool status_or_reset(int status, int expected) { return status == expected || (kResetMayLoseAnswer && status == 0); }
inline bool statuses_or_reset(const std::vector<int>& statuses, int expected) {
  return statuses == std::vector<int>{expected} || (kResetMayLoseAnswer && statuses.empty());
}

// The reviewer's attack: a complete first-password request hidden in another request's body.
inline std::string smuggled_setup(const std::string& password) {
  const std::string body = nlohmann::json{{"password", password}}.dump();
  return "POST /api/setup HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body + "\r\n";
}
// A whole request whose body hides 60 copies of the first-password request, the first copy
// starting exactly `offset` bytes into the connection: httplib reads ahead in 4 KB blocks and
// drops the read-ahead with the request, so a refused request's leftover body is parsed from a
// 4 KB boundary on. `head(body_size)` gives everything before the body; `tail` follows it.
// The same with 60 copies of any request (`smuggled(i)` gives copy i), e.g. an authenticated one
// when a password is set already.
inline std::string hiding_request_with(const std::function<std::string(size_t)>& head, size_t offset,
                                       const std::function<std::string(int)>& smuggled, const std::string& tail = "") {
  std::string copies;
  for (int i = 0; i < 60; ++i) copies += smuggled(i);
  const std::string first = smuggled(0).substr(0, 24);
  size_t body_size = copies.size() + offset;
  for (int i = 0; i < 4; ++i) {  // the head's length field depends on the body size
    const size_t h = head(body_size).size();
    REQUIRE(offset >= h + 2);
    body_size = (offset - h - 2) + 2 + copies.size();
  }
  const std::string hd = head(body_size);
  const std::string body = std::string(offset - hd.size() - 2, 'x') + "\r\n" + copies;
  REQUIRE(body.size() == body_size);
  REQUIRE((hd + body).compare(offset, first.size(), first) == 0);
  return hd + body + tail;
}
inline std::string hiding_request(const std::function<std::string(size_t)>& head, size_t offset, const std::string& tail = "") {
  return hiding_request_with(head, offset, [](int i) { return smuggled_setup("przejete" + std::to_string(i)); }, tail);
}
inline std::string head_with_length(const std::string& method, const std::string& path, const std::string& type, size_t body_size,
                                    const std::string& cookie = "", const std::string& length = "") {
  return method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: " + type + "\r\n" +
         (cookie.empty() ? std::string() : "Cookie: " + cookie + "\r\n") + "Content-Length: " +
         (length.empty() ? std::to_string(body_size) : length) + "\r\n\r\n";
}
// A last request on the same connection: with one request per connection it is never answered
// (if it were, the server would close after it anyway).
inline const std::string kLastRequest = "GET /api/auth-state HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";

inline std::string chunked_request(const std::string& method, const std::string& path, const std::string& body,
                                   const std::string& cookie = "") {
  std::ostringstream hex;
  hex << std::hex << body.size();
  return method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n" +
         (cookie.empty() ? std::string() : "Cookie: " + cookie + "\r\n") + "Transfer-Encoding: chunked\r\n\r\n" + hex.str() +
         "\r\n" + body + "\r\n0\r\n\r\n";
}

// N wrong logins from this computer at once: the throttle must count the ones still checking
// their password, not only the finished ones. A burst of N connections overflows httplib's listen
// backlog of 5, and the kernel may refuse or reset the extra ones before the server sees them:
// such a login is sent again (a little later) until it is answered.
struct LoginBurst {
  int unauthorized = 0, throttled = 0, lost = 0, other = 0, resent = 0;
};
inline LoginBurst concurrent_wrong_logins(uint16_t port, int n) {
  LoginBurst b;
  std::mutex m;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&] {
      while (!go) std::this_thread::yield();
      httplib::Result r;
      for (int tries = 0; tries < 50; ++tries) {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(10, 0);
        c.set_read_timeout(20, 0);
        r = c.Post("/api/login", R"({"password":"zlehaslo123"})", "application/json");
        if (r) break;
        {
          std::lock_guard lock(m);
          ++b.resent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      std::lock_guard lock(m);
      if (!r) ++b.lost;
      else if (r->status == 401) ++b.unauthorized;
      else if (r->status == 429) ++b.throttled;
      else ++b.other;
    });
  }
  go = true;
  for (auto& t : threads) t.join();
  return b;
}
// At most 5 logins got to check the password (5 failures block the address), whatever reached
// the server at the same time; every other one was refused with 429.
inline void check_login_burst(const LoginBurst& b, int n) {
  CAPTURE(b.unauthorized);
  CAPTURE(b.throttled);
  CAPTURE(b.lost);
  CAPTURE(b.resent);
  CHECK(b.unauthorized <= 5);
  CHECK(b.other == 0);
  CHECK(b.lost == 0);
  CHECK(b.unauthorized + b.throttled == n);
}

// Joins a test thread however the test leaves its scope (a failed REQUIRE included).
struct JoinOnExit {
  std::thread& t;
  ~JoinOnExit() {
    if (t.joinable()) t.join();
  }
};

}  // namespace http_attack
