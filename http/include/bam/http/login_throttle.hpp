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

  // One login attempt that may check its password. Until it ends (failed(), succeeded() or its
  // destruction, whatever path the request takes) it counts against the limits like a failure:
  // a password check takes a while (PBKDF2), and attempts sent at the same time must not all get
  // in before the first one has failed. Move-only.
  class Ticket {
   public:
    Ticket() = default;  // refused
    Ticket(Ticket&& other) noexcept;
    Ticket& operator=(Ticket&& other) noexcept;
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    ~Ticket();  // neither failed nor succeeded: the attempt just ends

    explicit operator bool() const { return owner_ != nullptr; }  // false: refuse (429) without checking the password
    void failed(int64_t now_ns);  // the password was wrong
    void succeeded();             // the password was right: the address's failures are forgotten

   private:
    friend class LoginThrottle;
    Ticket(LoginThrottle* owner, std::string address, bool tracked) : owner_(owner), address_(std::move(address)), tracked_(tracked) {}
    void finish(int outcome, int64_t now_ns);

    LoginThrottle* owner_ = nullptr;
    std::string address_;
    bool tracked_ = false;  // counted in the address's entry (false only when the address table is full)
  };

  LoginThrottle() : LoginThrottle(Limits{}) {}
  explicit LoginThrottle(Limits limits) : limits_(limits) {}
  LoginThrottle(const LoginThrottle&) = delete;
  LoginThrottle& operator=(const LoginThrottle&) = delete;

  // A login attempt from `address`: refused (false) while the address, or (for a remote address)
  // every remote address, is blocked, or when its recent failures plus the attempts still in
  // progress reach a limit. The throttle must outlive its tickets.
  Ticket attempt(const std::string& address, int64_t now_ns);

  // A failure or success recorded without a ticket (what Ticket::failed/succeeded do as well).
  void failed(const std::string& address, int64_t now_ns);
  void succeeded(const std::string& address);

 private:
  struct Entry {
    std::deque<int64_t> failures;
    int64_t blocked_until = 0;
    size_t pending = 0;  // tickets of this address still in progress
  };
  void prune(int64_t now_ns);
  std::map<std::string, Entry>::iterator track(const std::string& address, int64_t now_ns);  // end(): table full
  void record_failure(const std::string& address, int64_t now_ns);
  void record_success(const std::string& address);
  void release(const std::string& address, bool tracked);

  Limits limits_;
  std::mutex m_;
  std::map<std::string, Entry> entries_;
  std::deque<int64_t> overall_;
  size_t overall_pending_ = 0;  // remote tickets still in progress
  int64_t overall_blocked_until_ = 0;
};

}  // namespace bam::http
