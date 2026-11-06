// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/http/login_throttle.hpp"

namespace bam::http {

namespace {
bool is_loopback(const std::string& a) { return a == "127.0.0.1" || a == "::1" || a == "::ffff:127.0.0.1"; }
}  // namespace

bool LoginThrottle::allowed(const std::string& address, int64_t now_ns) {
  std::lock_guard lock(m_);
  if (auto it = entries_.find(address); it != entries_.end() && it->second.blocked_until > now_ns) return false;
  return is_loopback(address) || overall_blocked_until_ <= now_ns;
}

void LoginThrottle::failed(const std::string& address, int64_t now_ns) {
  std::lock_guard lock(m_);
  prune(now_ns);
  if (!is_loopback(address)) {
    overall_.push_back(now_ns);
    if (overall_.size() >= limits_.overall) {
      overall_blocked_until_ = now_ns + limits_.block_ns;
      overall_.clear();
    }
  }
  auto it = entries_.find(address);
  if (it == entries_.end()) {
    if (entries_.size() >= limits_.max_addresses) {
      // full: forget the address whose last failure is the oldest, unless every one is blocked
      auto victim = entries_.end();
      for (auto e = entries_.begin(); e != entries_.end(); ++e)
        if (e->second.blocked_until <= now_ns &&
            (victim == entries_.end() || e->second.failures.back() < victim->second.failures.back()))
          victim = e;
      if (victim == entries_.end()) return;  // counted overall only
      entries_.erase(victim);
    }
    it = entries_.emplace(address, Entry{}).first;
  }
  Entry& e = it->second;
  e.failures.push_back(now_ns);
  if (e.failures.size() >= limits_.per_address) {
    e.blocked_until = now_ns + limits_.block_ns;
    e.failures.clear();
  }
}

void LoginThrottle::succeeded(const std::string& address) {
  std::lock_guard lock(m_);
  entries_.erase(address);
}

void LoginThrottle::prune(int64_t now_ns) {
  auto expired = [&](int64_t t) { return now_ns - t >= limits_.window_ns; };
  while (!overall_.empty() && expired(overall_.front())) overall_.pop_front();
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto& f = it->second.failures;
    while (!f.empty() && expired(f.front())) f.pop_front();
    if (f.empty() && it->second.blocked_until <= now_ns) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace bam::http
