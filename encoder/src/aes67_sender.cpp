// SPDX-License-Identifier: GPL-3.0-or-later
#include "aes67_sender.hpp"

#include <span>

#include "bam/constants.hpp"
#include "bam/platform/time.hpp"

namespace bam::encoder::detail {

namespace {
constexpr int64_t kPeriodNs = 250'000;
constexpr int kSendAttempts = 4;  // the first and 3 retries
constexpr int64_t kRetryNs = 20'000;
std::atomic<int> g_fail_interfaces{0};
}  // namespace

namespace testing {
void fail_next_aes67_interfaces(int n) { g_fail_interfaces.store(n); }
}  // namespace testing

Aes67Sender::Aes67Sender(const ClockSource& clock) : clock_(clock) {
  std::string err;
  socket_ = platform::UdpSocket::open(0, &err);
  if (!socket_) error_ = "nie można otworzyć gniazda nadawczego AES67: " + err;
  thread_ = std::thread([this] { run(); });
}

Aes67Sender::~Aes67Sender() { stop(); }

void Aes67Sender::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void Aes67Sender::set_outputs(Outputs outputs_with_address) {
  {
    std::lock_guard lock(m_);
    outputs_.swap(outputs_with_address);
  }
  // the list let go of goes here, on the caller's thread
}

void Aes67Sender::set_interface(const std::string& iface_ip) {
  std::lock_guard lock(m_);
  socket_ok_ = false;
  if (!socket_) {  // it failed before: try again
    std::string err;
    socket_ = platform::UdpSocket::open(0, &err);
    if (!socket_) {
      error_ = "nie można otworzyć gniazda nadawczego AES67: " + err;
      return;
    }
  }
  if (iface_ip.empty()) {
    error_ = "nie znaleziono aktywnej karty sieciowej IPv4";
    return;
  }
  bool failed = false;
  for (int n = g_fail_interfaces.load(); n > 0 && !failed;)
    failed = g_fail_interfaces.compare_exchange_weak(n, n - 1);
  if (failed || !socket_->set_multicast_out(iface_ip, kMulticastTtl, true)) {
    error_ = "nie można nadawać multicastu przez kartę " + iface_ip;
    return;
  }
  socket_ok_ = true;
  error_.clear();
}

void Aes67Sender::set_allowed(bool send) { allowed_.store(send, std::memory_order_relaxed); }

std::string Aes67Sender::error() const {
  std::lock_guard lock(m_);
  return error_;
}

void Aes67Sender::run() {
  platform::set_realtime_priority("Pro Audio");
  int64_t next_wake = platform::monotonic_ns();
  while (running_.load(std::memory_order_relaxed)) {
    next_wake += kPeriodNs;
    const int64_t before = platform::monotonic_ns();
    if (before - next_wake > 5'000'000) next_wake = before;  // fell far behind: don't burst
    platform::sleep_until_ns(next_wake);
    if (!allowed_.load(std::memory_order_relaxed)) continue;

    std::lock_guard lock(m_);
    if (!socket_ok_ || outputs_.empty()) continue;
    uint64_t generation = 0;
    const MediaClock clock = clock_.get(&generation);  // the mapping and its generation together
    const uint64_t now = clock.sample_at(platform::monotonic_ns());
    for (const auto& entry : outputs_) {
      const auto* e = &entry;  // one pointer: the sink fits std::function's own storage
      e->second->pump(now, generation, [this, e](std::span<const uint8_t> p) {
        // A full socket buffer (non-blocking send) clears within microseconds: retry briefly.
        for (int attempt = 0; attempt < kSendAttempts; ++attempt) {
          if (attempt > 0) platform::sleep_ns(kRetryNs);
          if (socket_->send_to(p, e->first, kRtpPort)) return;
        }
        e->second->count_send_error();
      });
    }
  }
}

}  // namespace bam::encoder::detail
