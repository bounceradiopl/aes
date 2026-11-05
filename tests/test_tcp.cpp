// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"

namespace bp = bam::platform;

namespace {
// Reads exactly `n` bytes (or fails), looping recv() until the deadline.
bool recv_exact(bp::TcpStream& s, std::vector<uint8_t>& out, size_t n, int timeout_ms) {
  out.resize(n);
  size_t have = 0;
  const int64_t deadline = bp::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (have < n) {
    const int remaining_ms = static_cast<int>((deadline - bp::monotonic_ns()) / 1'000'000);
    if (remaining_ms <= 0) return false;
    int got = s.recv(std::span<uint8_t>(out.data() + have, n - have), remaining_ms);
    if (got == 0) return false;  // closed early
    if (got < 0) continue;       // spurious wakeup/timeout slice; retry within the overall deadline
    have += static_cast<size_t>(got);
  }
  return true;
}
}  // namespace

TEST_CASE("TCP: listener on 127.0.0.1 port 0, client connects") {
  std::string err;
  auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(listener, err);
  CHECK(listener->port() != 0);

  std::optional<bp::TcpStream> accepted;
  std::thread server([&] { accepted = listener->accept(2000); });

  auto client = bp::TcpStream::connect("127.0.0.1", listener->port(), 2000, &err);
  server.join();
  REQUIRE_MESSAGE(client, err);
  CHECK(client->valid());
  REQUIRE(accepted);
  CHECK(accepted->valid());
}

TEST_CASE("TCP: 1 MB sent both ways intact") {
  std::string err;
  auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(listener, err);

  std::vector<uint8_t> up(1 << 20);
  for (size_t i = 0; i < up.size(); ++i) up[i] = static_cast<uint8_t>(i * 7 + 3);
  std::vector<uint8_t> down(1 << 20);
  for (size_t i = 0; i < down.size(); ++i) down[i] = static_cast<uint8_t>(i * 13 + 1);

  std::vector<uint8_t> server_got;
  bool server_echo_ok = false;
  std::thread server([&] {
    auto s = listener->accept(2000);
    if (!s) return;
    server_echo_ok = recv_exact(*s, server_got, up.size(), 5000) && s->send_all(down, 5000);
  });

  auto client = bp::TcpStream::connect("127.0.0.1", listener->port(), 2000, &err);
  REQUIRE_MESSAGE(client, err);
  REQUIRE(client->send_all(up, 5000));
  std::vector<uint8_t> client_got;
  const bool client_ok = recv_exact(*client, client_got, down.size(), 5000);
  server.join();

  REQUIRE(server_echo_ok);
  REQUIRE(client_ok);
  CHECK(server_got == up);
  CHECK(client_got == down);
}

TEST_CASE("TCP: recv timeout returns -1") {
  std::string err;
  auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(listener, err);
  std::optional<bp::TcpStream> accepted;  // kept alive: an immediate close would read as EOF (0), not a timeout
  std::thread server([&] { accepted = listener->accept(2000); });
  auto client = bp::TcpStream::connect("127.0.0.1", listener->port(), 2000, &err);
  server.join();
  REQUIRE_MESSAGE(client, err);
  REQUIRE(accepted);

  std::vector<uint8_t> buf(16);
  CHECK(client->recv(buf, 50) == -1);
}

TEST_CASE("TCP: connect to a closed port fails with an error") {
  std::string err;
  uint16_t port;
  {
    auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
    REQUIRE_MESSAGE(listener, err);
    port = listener->port();
  }  // listener closed: nothing is listening on `port` any more

  auto client = bp::TcpStream::connect("127.0.0.1", port, 1000, &err);
  CHECK_FALSE(client);
  CHECK_FALSE(err.empty());
}

TEST_CASE("TCP: connect to an unresolvable name fails") {
  std::string err;
  // "invalid" is an RFC 6761 reserved TLD that never resolves; no real network traffic is sent.
  auto client = bp::TcpStream::connect("this-name-does-not-exist.invalid", 1234, 1000, &err);
  CHECK_FALSE(client);
  CHECK_FALSE(err.empty());
}
