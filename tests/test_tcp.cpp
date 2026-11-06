// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdint>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

TEST_CASE("TCP: send_all survives a reset peer instead of raising SIGPIPE") {
  std::string err;
  auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(listener, err);

  // The server reads a single, modest chunk and then lets its stream go out of scope (close()).
  // It does *not* drain everything the client sends, so unread data is still queued when it
  // closes: the kernel resets the connection (RST) instead of a graceful FIN, exactly like a
  // crashed/killed Icecast server would mid-upload.
  bool server_accepted = false;
  std::thread server([&] {
    auto s = listener->accept(2000);
    server_accepted = static_cast<bool>(s);
    if (!s) return;
    std::vector<uint8_t> buf(65536);
    s->recv(buf, 2000);
  });

  auto client = bp::TcpStream::connect("127.0.0.1", listener->port(), 2000, &err);
  REQUIRE_MESSAGE(client, err);

  // Keep writing ~1 MB chunks right away, without waiting for the server thread: the whole point
  // is to have several MB in flight when the reset happens, matching a continuous Icecast upload.
  // Note: send_all()'s own poll-before-send gate already shields *this* pattern on loopback (its
  // internal poll() reliably observes POLLHUP before the next send(), since there is no real
  // network latency for the RST to race against) -- this is a non-regression/sanity net, not the
  // test that actually pins the SIGPIPE fix. See the next test case for that.
  std::vector<uint8_t> chunk(1 << 20);
  int failures = 0;
  const int64_t deadline = bp::monotonic_ns() + 10'000'000'000LL;
  while (bp::monotonic_ns() < deadline && failures < 5) {
    if (!client->send_all(chunk, 200)) ++failures;
  }
  server.join();
  CHECK(server_accepted);
  CHECK(failures > 0);
}

#ifndef _WIN32
// POSIX only: SIGPIPE on a broken-pipe write is a POSIX-only hazard (no equivalent on Windows).
// This test bypasses TcpStream::send_all()'s own poll-before-send guard (which, as the previous
// test shows, already shields the common case on loopback) and writes directly to the raw fd via
// native(), exactly like send_all() does internally once poll() has said "writable" -- the one
// thing SO_NOSIGPIPE actually has to protect against. A forced abortive close (SO_LINGER{1,0}) on
// the peer -- raw sockets, since TcpStream intentionally doesn't expose linger control -- gives a
// deterministic "connection already reset" state before any write is attempted.
TEST_CASE("TCP: a reset peer does not raise SIGPIPE when written to directly") {
  std::string err;
  auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(listener, err);
  auto client = bp::TcpStream::connect("127.0.0.1", listener->port(), 2000, &err);
  REQUIRE_MESSAGE(client, err);
  auto server = listener->accept(2000);
  REQUIRE(server);

  const int server_fd = static_cast<int>(server->native());
  const linger l{1, 0};
  setsockopt(server_fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
  server->close();  // abortive close: the client's connection is reset (RST), not gracefully closed
  bp::sleep_until_ns(bp::monotonic_ns() + 200'000'000);  // let the RST be processed locally

  const int client_fd = static_cast<int>(client->native());
  std::vector<uint8_t> chunk(4096, 'x');
  int failures = 0;
  for (int i = 0; i < 5; ++i) {
    const ssize_t n = ::send(client_fd, chunk.data(), chunk.size(), 0);
    if (n < 0) ++failures;
  }
  CHECK(failures > 0);  // the test process must still be alive to reach this line
}
#endif

TEST_CASE("TCP: send_some/recv_some report bytes, timeout (0) and a closed peer (-1)") {
  std::string err;
  auto listener = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(listener, err);
  auto client = bp::TcpStream::connect("127.0.0.1", listener->port(), 2000, &err);
  REQUIRE_MESSAGE(client, err);
  auto server = listener->accept(2000);
  REQUIRE(server);

  std::vector<uint8_t> buf(16);
  CHECK(client->recv_some(buf, 0) == 0);   // nothing there yet: just a check
  CHECK(client->recv_some(buf, 50) == 0);  // nothing within the timeout

  const std::vector<uint8_t> hello = {'h', 'e', 'l', 'l', 'o'};
  CHECK(server->send_some(hello, 1000) == 5);
  std::vector<uint8_t> got;
  const int64_t deadline = bp::monotonic_ns() + 2'000'000'000LL;
  while (got.size() < hello.size() && bp::monotonic_ns() < deadline) {
    const int n = client->recv_some(buf, 100);
    REQUIRE(n >= 0);
    got.insert(got.end(), buf.begin(), buf.begin() + n);
  }
  CHECK(got == hello);

  // A peer that never reads: the socket buffers fill up and send_some times out with 0.
  std::vector<uint8_t> chunk(1 << 16, 'x');
  int last = 1;
  size_t total = 0;
  for (int i = 0; i < 100000 && last > 0; ++i) {
    last = client->send_some(chunk, 20);
    if (last > 0) total += static_cast<size_t>(last);
  }
  CHECK(last == 0);
  CHECK(total > 0);

  server->close();
  int closed = 0;
  const int64_t deadline2 = bp::monotonic_ns() + 2'000'000'000LL;
  while (bp::monotonic_ns() < deadline2) {
    closed = client->recv_some(buf, 100);
    if (closed < 0) break;
  }
  CHECK(closed == -1);
}
