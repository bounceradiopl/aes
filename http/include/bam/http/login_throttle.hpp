// SPDX-License-Identifier: GPL-3.0-or-later
// Panel login throttling: failed logins are counted per remote address and overall; an address
// with too many recent failures is refused for a while (HTTP 429) instead of a request thread
// sleeping. Loopback addresses (someone at the computer itself) are only subject to their own
// counter, so failures from the network can never lock the local administrator out. Shared by the
// BounceAES Master and Bounce Encoder panels.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace bam::http {

class LoginThrottle {
 public:
  struct Limits {
    size_t per_address = 5;              // failures within `window_ns` that block an address
    size_t overall = 20;                 // failures from all addresses within `window_ns` that block every remote address
    int64_t window_ns = 60'000'000'000;  // 60 s
    int64_t block_ns = 30'000'000'000;   // 30 s
    size_t max_addresses = 4096;         // addresses tracked at most (memory bound)
  };

  LoginThrottle() : LoginThrottle(Limits{}) {}
  explicit LoginThrottle(Limits limits) : limits_(limits) {}

  // false: refuse this login attempt without checking the password.
  bool allowed(const std::string& address, int64_t now_ns);
  void failed(const std::string& address, int64_t now_ns);
  void succeeded(const std::string& address);

 private:
  struct Entry {
    std::deque<int64_t> failures;
    int64_t blocked_until = 0;
  };
  void prune(int64_t now_ns);

  Limits limits_;
  std::mutex m_;
  std::map<std::string, Entry> entries_;
  std::deque<int64_t> overall_;
  int64_t overall_blocked_until_ = 0;
};

}  // namespace bam::http
