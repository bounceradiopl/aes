// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/http/login_throttle.hpp"

#include <utility>

namespace bam::http {

namespace {
bool is_loopback(const std::string& a) { return a == "127.0.0.1" || a == "::1" || a == "::ffff:127.0.0.1"; }
enum Outcome { kNone, kFailed, kSucceeded };
}  // namespace

// ---- Ticket ----

LoginThrottle::Ticket::Ticket(Ticket&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), address_(std::move(other.address_)), tracked_(other.tracked_) {}

LoginThrottle::Ticket& LoginThrottle::Ticket::operator=(Ticket&& other) noexcept {
  if (this != &other) {
    finish(kNone, 0);
    owner_ = std::exchange(other.owner_, nullptr);
    address_ = std::move(other.address_);
    tracked_ = other.tracked_;
  }
  return *this;
}

LoginThrottle::Ticket::~Ticket() { finish(kNone, 0); }

void LoginThrottle::Ticket::failed(int64_t now_ns) { finish(kFailed, now_ns); }

void LoginThrottle::Ticket::succeeded() { finish(kSucceeded, 0); }

void LoginThrottle::Ticket::finish(int outcome, int64_t now_ns) {
  LoginThrottle* owner = std::exchange(owner_, nullptr);
  if (!owner) return;
  std::lock_guard lock(owner->m_);
  owner->release(address_, tracked_);
  if (outcome == kFailed) owner->record_failure(address_, now_ns);
  if (outcome == kSucceeded) owner->record_success(address_);
}

// ---- LoginThrottle ----

LoginThrottle::Ticket LoginThrottle::attempt(const std::string& address, int64_t now_ns) {
  std::lock_guard lock(m_);
  prune(now_ns);
  const bool local = is_loopback(address);
  auto it = entries_.find(address);
  if (it != entries_.end() &&
      (it->second.blocked_until > now_ns || it->second.failures.size() + it->second.pending >= limits_.per_address))
    return {};
  if (!local && (overall_blocked_until_ > now_ns || overall_.size() + overall_pending_ >= limits_.overall)) return {};
  if (it == entries_.end()) it = track(address, now_ns);
  const bool tracked = it != entries_.end();
  if (tracked) ++it->second.pending;
  if (!local) ++overall_pending_;
  return Ticket(this, address, tracked);
}

void LoginThrottle::failed(const std::string& address, int64_t now_ns) {
  std::lock_guard lock(m_);
  record_failure(address, now_ns);
}

void LoginThrottle::succeeded(const std::string& address) {
  std::lock_guard lock(m_);
  record_success(address);
}

std::map<std::string, LoginThrottle::Entry>::iterator LoginThrottle::track(const std::string& address, int64_t now_ns) {
  if (entries_.size() >= limits_.max_addresses) {
    // full: forget the address whose last failure is the oldest, unless every one is blocked or
    // has an attempt in progress
    auto victim = entries_.end();
    for (auto e = entries_.begin(); e != entries_.end(); ++e)
      if (e->second.blocked_until <= now_ns && e->second.pending == 0 && !e->second.failures.empty() &&
          (victim == entries_.end() || e->second.failures.back() < victim->second.failures.back()))
        victim = e;
    if (victim == entries_.end()) return entries_.end();  // counted overall only
    entries_.erase(victim);
  }
  return entries_.emplace(address, Entry{}).first;
}

void LoginThrottle::record_failure(const std::string& address, int64_t now_ns) {
  prune(now_ns);
  if (!is_loopback(address)) {
    overall_.push_back(now_ns);
    if (overall_.size() >= limits_.overall) {
      overall_blocked_until_ = now_ns + limits_.block_ns;
      overall_.clear();
    }
  }
  auto it = entries_.find(address);
  if (it == entries_.end()) it = track(address, now_ns);
  if (it == entries_.end()) return;
  Entry& e = it->second;
  e.failures.push_back(now_ns);
  if (e.failures.size() >= limits_.per_address) {
    e.blocked_until = now_ns + limits_.block_ns;
    e.failures.clear();
  }
}

void LoginThrottle::record_success(const std::string& address) {
  auto it = entries_.find(address);
  if (it == entries_.end()) return;
  if (it->second.pending > 0) {  // other attempts of this address are still in progress
    it->second.failures.clear();
    it->second.blocked_until = 0;
  } else {
    entries_.erase(it);
  }
}

void LoginThrottle::release(const std::string& address, bool tracked) {
  if (tracked) {
    auto it = entries_.find(address);  // an entry with pending tickets is never forgotten
    if (it != entries_.end() && it->second.pending > 0) --it->second.pending;
  }
  if (!is_loopback(address) && overall_pending_ > 0) --overall_pending_;
}

void LoginThrottle::prune(int64_t now_ns) {
  auto expired = [&](int64_t t) { return now_ns - t >= limits_.window_ns; };
  while (!overall_.empty() && expired(overall_.front())) overall_.pop_front();
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto& f = it->second.failures;
    while (!f.empty() && expired(f.front())) f.pop_front();
    if (f.empty() && it->second.blocked_until <= now_ns && it->second.pending == 0) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace bam::http
