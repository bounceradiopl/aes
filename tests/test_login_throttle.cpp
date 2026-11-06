// SPDX-License-Identifier: GPL-3.0-or-later
// The login counter behind HTTP 429 in both panels (BounceAES Master, Bounce Encoder).
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "bam/http/login_throttle.hpp"

TEST_CASE("panel login throttle: per address, overall ceiling, expiry") {
  using T = bam::http::LoginThrottle;
  constexpr int64_t s = 1'000'000'000;
  const std::string a = "192.168.1.10", b = "192.168.1.11", local = "127.0.0.1";
  // an attempt that is let in and ends at once (its ticket released)
  const auto allowed = [](T& t, const std::string& address, int64_t now) { return static_cast<bool>(t.attempt(address, now)); };

  SUBCASE("5 failures within 60 s block the address for 30 s") {
    T t;
    for (int i = 0; i < 5; ++i) {
      CHECK(allowed(t, a, i * s));
      t.failed(a, i * s);
    }
    CHECK_FALSE(allowed(t, a, 5 * s));
    CHECK(allowed(t, b, 5 * s));            // another address is not affected
    CHECK_FALSE(allowed(t, a, 4 * s + 29 * s));
    CHECK(allowed(t, a, 4 * s + 31 * s));   // the block is over
  }
  SUBCASE("failures older than the window do not count; a success clears the address") {
    T t;
    for (int i = 0; i < 4; ++i) t.failed(a, i * s);
    t.failed(a, 70 * s);  // the first four are older than 60 s by now
    CHECK(allowed(t, a, 70 * s));
    for (int i = 0; i < 4; ++i) t.failed(b, i * s);
    t.succeeded(b);
    t.failed(b, 5 * s);
    CHECK(allowed(t, b, 5 * s));
  }
  SUBCASE("too many failures overall block every remote address, never the local one") {
    T::Limits l;
    l.overall = 6;
    T t(l);
    for (int i = 0; i < 6; ++i) t.failed("10.0.0." + std::to_string(i), s);  // one or two per address
    CHECK_FALSE(allowed(t, "10.0.0.99", 2 * s));
    CHECK(allowed(t, local, 2 * s));
    CHECK(allowed(t, "10.0.0.99", 32 * s));
  }
  SUBCASE("the local address has its own counter") {
    T t;
    for (int i = 0; i < 5; ++i) t.failed(local, s);
    CHECK_FALSE(allowed(t, local, 2 * s));
  }
  SUBCASE("the number of tracked addresses is bounded") {
    T::Limits l;
    l.max_addresses = 8;
    l.overall = 1'000'000;
    T t(l);
    for (int i = 0; i < 100; ++i) t.failed("10.1.0." + std::to_string(i), s);
    for (int i = 0; i < 5; ++i) t.failed(a, s);
    CHECK_FALSE(allowed(t, a, 2 * s));  // a blocked address is still found
  }
  SUBCASE("attempts still checking their password count like failures (no burst past the limit)") {
    T t;
    std::vector<T::Ticket> running;
    for (int i = 0; i < 5; ++i) {
      running.push_back(t.attempt(a, s));
      CHECK(static_cast<bool>(running.back()));
    }
    CHECK_FALSE(allowed(t, a, s));  // 5 in progress: a sixth one must wait
    CHECK(allowed(t, b, s));        // another address is not affected
    running.pop_back();             // one ends without an outcome (an error path)
    CHECK(allowed(t, a, s));
    for (auto& r : running) r.failed(2 * s);
    CHECK(allowed(t, a, 3 * s));  // 4 failures: one more may try
    t.failed(a, 3 * s);
    CHECK_FALSE(allowed(t, a, 4 * s));
    CHECK(allowed(t, a, 34 * s));  // the block is over
  }
  SUBCASE("a success while other attempts of the address run keeps their reservation") {
    T t;
    auto first = t.attempt(a, s);
    auto second = t.attempt(a, s);
    REQUIRE(static_cast<bool>(first));
    REQUIRE(static_cast<bool>(second));
    t.failed(a, s);
    t.failed(a, s);
    first.succeeded();  // forgets the failures, not the attempt still running
    std::vector<T::Ticket> more;
    for (int i = 0; i < 4; ++i) more.push_back(t.attempt(a, s));
    for (auto& m : more) CHECK(static_cast<bool>(m));
    CHECK_FALSE(allowed(t, a, s));  // 1 + 4 in progress
  }
  SUBCASE("remote attempts in progress count against the overall ceiling, the local ones do not") {
    T::Limits l;
    l.overall = 6;
    T t(l);
    std::vector<T::Ticket> running;
    for (int i = 0; i < 6; ++i) running.push_back(t.attempt("10.0.0." + std::to_string(i), s));
    for (auto& r : running) CHECK(static_cast<bool>(r));
    CHECK_FALSE(allowed(t, "10.0.0.99", s));
    CHECK(allowed(t, local, s));
    running.clear();  // all ended without an outcome
    CHECK(allowed(t, "10.0.0.99", s));
  }
  SUBCASE("a moved ticket is released once") {
    T t;
    {
      auto x = t.attempt(a, s);
      T::Ticket y = std::move(x);
      CHECK_FALSE(static_cast<bool>(x));
      CHECK(static_cast<bool>(y));
      y.failed(s);
      y.failed(s);  // a second outcome is ignored
    }
    for (int i = 0; i < 4; ++i) CHECK(allowed(t, a, s));  // 1 failure: 4 more may run at once
    std::vector<T::Ticket> running;
    for (int i = 0; i < 4; ++i) running.push_back(t.attempt(a, s));
    CHECK_FALSE(allowed(t, a, s));
  }
}
