// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"

namespace bp = bam::platform;

namespace {
std::string unique_name(const char* base) { return std::string(base) + std::to_string(bp::current_pid()); }

int recv_with_timeout(bp::UdpSocket& s, std::vector<uint8_t>& buf, int timeout_ms) {
  const int64_t end = bp::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (bp::monotonic_ns() < end) {
    int n = s.recv(buf, nullptr);
    if (n >= 0) return n;
    bp::sleep_ns(1'000'000);
  }
  return -1;
}
}  // namespace

TEST_CASE("clocks") {
  const int64_t a = bp::monotonic_ns();
  bp::sleep_until_ns(a + 5'000'000);
  const int64_t b = bp::monotonic_ns();
  CHECK(b - a >= 5'000'000);
  CHECK(b - a < 25'000'000);
  CHECK(bp::realtime_ns() > 1'600'000'000LL * 1'000'000'000);
}

TEST_CASE("shared memory is shared") {
  const std::string name = unique_name("bamtest.shm.");
  bool existed = true;
  std::string err;
  auto a = bp::SharedMemory::create(name, 1 << 20, &existed, &err);
  REQUIRE_MESSAGE(a, err);
  CHECK_FALSE(existed);
  auto b = bp::SharedMemory::open(name, 1 << 20, &err);
  REQUIRE_MESSAGE(b, err);
  std::memcpy(a->data(), "hello", 6);
  CHECK(std::string(static_cast<const char*>(b->data())) == "hello");
  bp::SharedMemory::remove(name);
}

TEST_CASE("named event") {
  const std::string name = unique_name("bamtest.ev.");
  std::string err;
  auto ev = bp::NamedEvent::create(name, &err);
  REQUIRE_MESSAGE(ev, err);
  auto other = bp::NamedEvent::open(name, &err);
  REQUIRE_MESSAGE(other, err);
  CHECK_FALSE(other->wait(10));
  ev->signal();
  ev->signal();  // auto-reset: two signals do not queue up
  CHECK(other->wait(100));
  CHECK_FALSE(other->wait(10));
  bp::NamedEvent::remove(name);
}

TEST_CASE("process helpers") {
  CHECK(bp::process_alive(bp::current_pid()));
  CHECK_FALSE(bp::process_alive(0));
}

TEST_CASE("multicast sockets only receive their own groups") {
  const std::string iface = bp::default_interface_ip();
  if (iface.empty()) {
    MESSAGE("no network interface, skipping");
    return;
  }
  std::string err;
  auto a = bp::UdpSocket::open(15004, &err);
  REQUIRE_MESSAGE(a, err);
  auto b = bp::UdpSocket::open(15004, &err);
  REQUIRE_MESSAGE(b, err);
  REQUIRE_MESSAGE(a->join("239.69.200.1", iface, "", &err), err);
  REQUIRE_MESSAGE(b->join("239.69.200.2", iface, "", &err), err);
  auto tx = bp::UdpSocket::open(0, &err);
  REQUIRE(tx);
  REQUIRE(tx->set_multicast_out(iface, 1, true));
  const std::vector<uint8_t> msg = {1, 2, 3, 4};
  REQUIRE(tx->send_to(msg, "239.69.200.1", 15004));
  std::vector<uint8_t> buf(64);
  CHECK(recv_with_timeout(*a, buf, 500) == 4);
  CHECK(recv_with_timeout(*b, buf, 100) == -1);
  while (a->recv(buf, nullptr) >= 0) {
  }  // macOS loopback may deliver a duplicate copy

  std::vector<bp::UdpSocket*> socks = {&*a, &*b};
  std::vector<uint8_t> ready;
  REQUIRE(tx->send_to(msg, "239.69.200.2", 15004));
  CHECK(bp::wait_readable(socks, 500, ready) >= 1);
  CHECK(ready[1] == 1);
  CHECK(ready[0] == 0);
}

TEST_CASE("interfaces are listed") {
  auto list = bp::list_interfaces();
  CHECK_FALSE(list.empty());
}
