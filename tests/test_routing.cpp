// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/routing.hpp"

TEST_CASE("route source ids round trip") {
  for (const char* id : {"net-in:1", "net-in:32", "asio-out:7", "win-app:chrome.exe", "win-loop:{0.0.0.00000000}.{abc}",
                         "win-in:{0.0.1.00000000}.{def}"}) {
    auto s = bam::parse_source(id);
    REQUIRE_MESSAGE(s, id);
    CHECK(bam::to_id(*s) == id);
  }
  CHECK(bam::parse_source("asio-out:7")->index == 7);
  CHECK(bam::is_windows(*bam::parse_source("win-in:x")));
  CHECK_FALSE(bam::is_windows(*bam::parse_source("net-in:2")));
}

TEST_CASE("route dest ids round trip") {
  for (const char* id : {"net-out:1", "asio-in:32", "win-out:{0.0.0.00000000}.{abc}"}) {
    auto d = bam::parse_dest(id);
    REQUIRE_MESSAGE(d, id);
    CHECK(bam::to_id(*d) == id);
  }
}

TEST_CASE("invalid route ids are rejected") {
  for (const char* id : {"net-in:0", "net-in:33", "net-in:x", "foo:1", "win-app:", "net-in", "", "asio-out:-1"})
    CHECK_MESSAGE(!bam::parse_source(id), id);
  for (const char* id : {"net-out:0", "asio-in:40", "win-out:", "net-in:1", "bar"}) CHECK_MESSAGE(!bam::parse_dest(id), id);
  CHECK_FALSE(bam::parse_source("net-out:1"));  // a destination is not a source
}

TEST_CASE("application keys are case-insensitive") {
  auto s = bam::parse_source("win-app:Chrome.EXE");
  REQUIRE(s);
  CHECK(s->key == "chrome.exe");
  CHECK(bam::to_id(*s) == "win-app:chrome.exe");
}

TEST_CASE("win-out channel pairs") {
  auto d = bam::parse_dest("win-out:{0.0.0.00000000}.{abc}#3");
  REQUIRE(d);
  CHECK(d->key == "{0.0.0.00000000}.{abc}");
  CHECK(d->pair == 3);
  CHECK(bam::to_id(*d) == "win-out:{0.0.0.00000000}.{abc}#3");
  auto first = bam::parse_dest("win-out:{0.0.0.00000000}.{abc}");
  REQUIRE(first);
  CHECK(first->pair == 1);
  CHECK(bam::to_id(*first) == "win-out:{0.0.0.00000000}.{abc}");
  CHECK_FALSE(bam::parse_dest("win-out:{x}#1"));   // pair 1 has no suffix
  CHECK_FALSE(bam::parse_dest("win-out:{x}#0"));
  CHECK_FALSE(bam::parse_dest("win-out:{x}#99"));
  CHECK_FALSE(bam::parse_dest("win-out:#2"));
}
