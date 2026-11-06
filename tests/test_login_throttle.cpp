// SPDX-License-Identifier: GPL-3.0-or-later
// The login counter behind HTTP 429 in both panels (BounceAES Master, Bounce Encoder).
#include <doctest/doctest.h>

#include <string>

#include "bam/http/login_throttle.hpp"

TEST_CASE("panel login throttle: per address, overall ceiling, expiry") {
  using T = bam::http::LoginThrottle;
  constexpr int64_t s = 1'000'000'000;
  const std::string a = "192.168.1.10", b = "192.168.1.11", local = "127.0.0.1";

  SUBCASE("5 failures within 60 s block the address for 30 s") {
    T t;
    for (int i = 0; i < 5; ++i) {
      CHECK(t.allowed(a, i * s));
      t.failed(a, i * s);
    }
    CHECK_FALSE(t.allowed(a, 5 * s));
    CHECK(t.allowed(b, 5 * s));            // another address is not affected
    CHECK_FALSE(t.allowed(a, 4 * s + 29 * s));
    CHECK(t.allowed(a, 4 * s + 31 * s));   // the block is over
  }
  SUBCASE("failures older than the window do not count; a success clears the address") {
    T t;
    for (int i = 0; i < 4; ++i) t.failed(a, i * s);
    t.failed(a, 70 * s);  // the first four are older than 60 s by now
    CHECK(t.allowed(a, 70 * s));
    for (int i = 0; i < 4; ++i) t.failed(b, i * s);
    t.succeeded(b);
    t.failed(b, 5 * s);
    CHECK(t.allowed(b, 5 * s));
  }
  SUBCASE("too many failures overall block every remote address, never the local one") {
    T::Limits l;
    l.overall = 6;
    T t(l);
    for (int i = 0; i < 6; ++i) t.failed("10.0.0." + std::to_string(i), s);  // one or two per address
    CHECK_FALSE(t.allowed("10.0.0.99", 2 * s));
    CHECK(t.allowed(local, 2 * s));
    CHECK(t.allowed("10.0.0.99", 32 * s));
  }
  SUBCASE("the local address has its own counter") {
    T t;
    for (int i = 0; i < 5; ++i) t.failed(local, s);
    CHECK_FALSE(t.allowed(local, 2 * s));
  }
  SUBCASE("the number of tracked addresses is bounded") {
    T::Limits l;
    l.max_addresses = 8;
    l.overall = 1'000'000;
    T t(l);
    for (int i = 0; i < 100; ++i) t.failed("10.1.0." + std::to_string(i), s);
    for (int i = 0; i < 5; ++i) t.failed(a, s);
    CHECK_FALSE(t.allowed(a, 2 * s));  // a blocked address is still found
  }
}
