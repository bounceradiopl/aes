// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder Icecast source client (spec §6) against a fake Icecast server on 127.0.0.1
// (TcpListener): request headers and Basic auth, 100-continue, body order, 401/403/unreachable
// messages, the SOURCE fallback, reconnecting on the retry schedule with a fresh stream, queue
// overflow and a prompt stop() with a stalled server. Only loopback is ever contacted.
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bam/encoder/icecast.hpp"
#include "bam/platform/tcp.hpp"

namespace be = bam::encoder;
namespace bp = bam::platform;
using Clock = std::chrono::steady_clock;

namespace {

int64_t ms_since(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}
int64_t ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

// Polls `pred` every 2 ms until it holds or `timeout_ms` passes.
bool wait_for(const std::function<bool()>& pred, int timeout_ms = 10000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

// Set while a failed test unwinds: the fake server threads give up instead of waiting.
std::atomic<bool> g_abort{false};

// Joins a fake server thread at the end of a test, also when a REQUIRE failed half-way.
struct Joiner {
  std::thread& t;
  ~Joiner() {
    g_abort = true;
    if (t.joinable()) t.join();
    g_abort = false;
  }
};

// Waits for a flag set by the test thread (or for a failed test to unwind).
void wait_flag(const std::atomic<bool>& flag) {
  while (!flag && !g_abort) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Restores the default retry schedule when a test ends (also on a failed REQUIRE).
struct ScheduleOverride {
  explicit ScheduleOverride(std::vector<int> s) { be::testing::set_icecast_retry_schedule_ms(std::move(s)); }
  ~ScheduleOverride() { be::testing::set_icecast_retry_schedule_ms({}); }
};

// One accepted connection of the fake server: reads the request head, then body bytes.
struct FakeConn {
  bp::TcpStream s;
  std::string pending;  // bytes received after the head

  // Reads up to and including the blank line that ends the request head.
  std::optional<std::string> read_head(int timeout_ms = 5000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string buf;
    std::vector<uint8_t> chunk(4096);
    while (Clock::now() < deadline && !g_abort) {
      const size_t end = buf.find("\r\n\r\n");
      if (end != std::string::npos) {
        pending = buf.substr(end + 4);
        return buf.substr(0, end + 4);
      }
      const int n = s.recv_some(chunk, 50);
      if (n < 0) return std::nullopt;
      buf.append(reinterpret_cast<const char*>(chunk.data()), size_t(n));
    }
    return std::nullopt;
  }

  bool reply(const std::string& text) {
    return s.send_all(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), 2000);
  }

  // Reads exactly n body bytes.
  std::optional<std::vector<uint8_t>> read_body(size_t n, int timeout_ms = 10000) {
    std::vector<uint8_t> out(pending.begin(), pending.end());
    pending.clear();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<uint8_t> chunk(65536);
    while (out.size() < n && Clock::now() < deadline && !g_abort) {
      const int r = s.recv_some(std::span(chunk.data(), std::min(chunk.size(), n - out.size())), 50);
      if (r < 0) return std::nullopt;
      out.insert(out.end(), chunk.begin(), chunk.begin() + r);
    }
    if (out.size() < n) return std::nullopt;
    if (out.size() > n) {
      pending.assign(out.begin() + std::ptrdiff_t(n), out.end());
      out.resize(n);
    }
    return out;
  }
};

struct FakeServer {
  bp::TcpListener listener;
  static FakeServer make() {
    std::string err;
    auto l = bp::TcpListener::listen("127.0.0.1", 0, &err);
    REQUIRE_MESSAGE(l, err);
    return FakeServer{std::move(*l)};
  }
  uint16_t port() const { return listener.port(); }
  std::optional<FakeConn> accept(int timeout_ms = 10000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline && !g_abort) {
      if (auto s = listener.accept(50)) return FakeConn{std::move(*s), {}};
    }
    return std::nullopt;
  }
};

be::IcecastTarget target(uint16_t port) {
  be::IcecastTarget t;
  t.host = "127.0.0.1";
  t.port = port;
  t.mount = "live";  // no leading slash: the client adds it
  t.user = "source";
  t.password = "hackme";
  t.station_name = "Radio Bounce";
  t.content_type = "audio/mpeg";
  return t;
}

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  uint32_t x = seed * 2654435761u + 1;
  for (auto& b : v) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(x >> 24);
  }
  return v;
}

bool has_line(const std::string& head, const std::string& line) {
  return head.find("\r\n" + line + "\r\n") != std::string::npos;
}

std::string first_line(const std::string& head) { return head.substr(0, head.find("\r\n")); }

constexpr const char* k100 = "HTTP/1.1 100 Continue\r\n\r\n";

}  // namespace

TEST_CASE("Icecast: retry schedule 1, 2, 5, 10 s then every 10 s") {
  CHECK(be::icecast_retry_schedule_ms() == std::vector<int>{1000, 2000, 5000, 10000});
}

TEST_CASE("Icecast: Basic auth base64 matches the RFC 4648 vectors") {
  CHECK(be::testing::base64("") == "");
  CHECK(be::testing::base64("f") == "Zg==");
  CHECK(be::testing::base64("fo") == "Zm8=");
  CHECK(be::testing::base64("foo") == "Zm9v");
  CHECK(be::testing::base64("foob") == "Zm9vYg==");
  CHECK(be::testing::base64("fooba") == "Zm9vYmE=");
  CHECK(be::testing::base64("foobar") == "Zm9vYmFy");
  CHECK(be::testing::base64("source:hackme") == "c291cmNlOmhhY2ttZQ==");
  CHECK(be::testing::base64("\xff\xfe\x00") == "//4=");
}

TEST_CASE("Icecast: PUT with Basic auth and headers, 100-continue, body bytes in order") {
  auto server = FakeServer::make();
  std::string head;
  std::atomic<bool> head_read{false}, go{false}, body_done{false}, release{false};
  std::optional<std::vector<uint8_t>> body;
  std::vector<uint8_t> expected;
  std::vector<std::vector<uint8_t>> pieces;
  for (uint32_t i = 0; i < 300; ++i) {
    pieces.push_back(pattern((i * 37) % 1000 + 1, i));
    expected.insert(expected.end(), pieces.back().begin(), pieces.back().end());
  }

  std::thread srv([&] {
    auto c = server.accept();
    if (!c) return;
    if (auto h = c->read_head()) head = *h;
    head_read = true;
    wait_flag(go);
    if (!c->reply(k100)) return;
    body = c->read_body(expected.size());
    body_done = true;
    wait_flag(release);  // keep the connection open while the test looks at the status
  });
  Joiner join_srv{srv};

  be::IcecastClient client(target(server.port()), 1 << 20);
  std::atomic<int> reconnect_calls{0};
  client.on_reconnect([&] { ++reconnect_calls; });
  REQUIRE(wait_for([&] { return head_read.load(); }));
  CHECK(client.status().state == be::IcecastState::Connecting);
  // the first half is queued while the server has not answered yet: kept for the first connection
  for (size_t i = 0; i < pieces.size() / 2; ++i) client.push(pieces[i]);
  go = true;
  REQUIRE(wait_for([&] { return client.status().state == be::IcecastState::Streaming; }));
  for (size_t i = pieces.size() / 2; i < pieces.size(); ++i) client.push(pieces[i]);
  REQUIRE(wait_for([&] { return body_done.load(); }));

  CHECK(first_line(head) == "PUT /live HTTP/1.1");
  CHECK(has_line(head, "Authorization: Basic c291cmNlOmhhY2ttZQ=="));
  CHECK(has_line(head, "Content-Type: audio/mpeg"));
  CHECK(has_line(head, "Ice-Name: Radio Bounce"));
  CHECK(has_line(head, "Ice-Public: 0"));
  CHECK(has_line(head, "Expect: 100-continue"));
  CHECK(has_line(head, "Host: 127.0.0.1:" + std::to_string(server.port())));
  CHECK(head.find("Content-Length") == std::string::npos);
  CHECK(head.find("hackme") == std::string::npos);  // the password only travels base64-encoded

  REQUIRE(body);
  CHECK(*body == expected);
  CHECK(wait_for([&] { return client.status().bytes_sent == expected.size(); }));
  CHECK(wait_for([&] { return client.status().kbps > 0; }, 5000));
  const auto st = client.status();
  CHECK(st.state == be::IcecastState::Streaming);
  CHECK(st.error.empty());
  CHECK(st.reconnects == 0);
  CHECK(st.dropped_bytes == 0);
  CHECK(reconnect_calls == 0);

  client.stop();
  CHECK(client.status().state == be::IcecastState::Idle);
  client.stop();  // idempotent
  release = true;
  srv.join();
}

TEST_CASE("Icecast: 401 and 403 are reported in Polish") {
  std::string response, message;
  SUBCASE("401") {
    response = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    message = "złe hasło lub użytkownik";
  }
  SUBCASE("403") {
    response = "HTTP/1.1 403 Forbidden\r\n\r\n";
    message = "mountpoint zajęty lub zabroniony";
  }
  auto server = FakeServer::make();
  std::thread srv([&] {
    auto c = server.accept();
    if (!c || !c->read_head()) return;
    c->reply(response);
  });
  Joiner join_srv{srv};
  be::IcecastClient client(target(server.port()), 1 << 16);
  CHECK(wait_for([&] { return client.status().state == be::IcecastState::Error; }));
  const auto st = client.status();
  CHECK(st.error == message);
  CHECK(st.error.find("hackme") == std::string::npos);
  srv.join();
  client.stop();
}

TEST_CASE("Icecast: closed port is reported as unreachable") {
  uint16_t port;
  {
    auto s = FakeServer::make();
    port = s.port();
  }  // nothing listens on `port` any more
  be::IcecastClient client(target(port), 1 << 16);
  CHECK(wait_for([&] { return client.status().state == be::IcecastState::Error; }));
  CHECK(client.status().error == "serwer nieosiągalny");
  client.stop();
}

TEST_CASE("Icecast: a refused PUT falls back to SOURCE, which is kept for reconnects") {
  std::string refusal;
  SUBCASE("405") { refusal = "HTTP/1.1 405 Method Not Allowed\r\n\r\n"; }
  SUBCASE("400") { refusal = "HTTP/1.0 400 Bad Request\r\n\r\n"; }
  ScheduleOverride fast({50});
  auto server = FakeServer::make();
  std::string head1, head2, head3;
  std::optional<std::vector<uint8_t>> body2, body3;
  const auto a = pattern(3000, 1), b = pattern(2000, 2);

  std::thread srv([&] {
    if (auto c = server.accept()) {
      if (auto h = c->read_head()) head1 = *h;
      c->reply(refusal);
    }
    if (auto c = server.accept()) {
      if (auto h = c->read_head()) head2 = *h;
      c->reply("HTTP/1.0 200 OK\r\n\r\n");
      body2 = c->read_body(a.size());
    }  // dropped: the client reconnects
    if (auto c = server.accept()) {
      if (auto h = c->read_head()) head3 = *h;
      c->reply("HTTP/1.0 200 OK\r\n\r\n");
      body3 = c->read_body(b.size());
    }
  });
  Joiner join_srv{srv};

  be::IcecastClient client(target(server.port()), 1 << 20);
  std::atomic<int> reconnect_calls{0};
  client.on_reconnect([&] { ++reconnect_calls; });
  client.push(a);
  REQUIRE(wait_for([&] { return reconnect_calls.load() == 1; }));
  client.push(b);
  srv.join();

  CHECK(first_line(head1) == "PUT /live HTTP/1.1");
  CHECK(first_line(head2) == "SOURCE /live ICE/1.0");
  CHECK(has_line(head2, "Authorization: Basic c291cmNlOmhhY2ttZQ=="));
  CHECK(has_line(head2, "Content-Type: audio/mpeg"));
  CHECK(has_line(head2, "Ice-Name: Radio Bounce"));
  CHECK(has_line(head2, "Ice-Public: 0"));
  CHECK(head2.find("Expect") == std::string::npos);
  REQUIRE(body2);
  CHECK(*body2 == a);
  CHECK(first_line(head3) == "SOURCE /live ICE/1.0");  // no second PUT round-trip after a drop
  REQUIRE(body3);
  CHECK(*body3 == b);
  CHECK(client.status().state == be::IcecastState::Streaming);
  client.stop();
}

TEST_CASE("Icecast: a dropped connection is retried on the schedule with a fresh stream") {
  // Distinct waits so each retry shows which schedule entry it used.
  ScheduleOverride sched({100, 1000, 300});
  auto server = FakeServer::make();
  const auto a = pattern(5000, 10), stale = pattern(777, 11), b = pattern(4000, 12), c = pattern(1500, 13);
  std::atomic<bool> conn1_done{false}, stale_pushed{false}, conn4_done{false};
  Clock::time_point t_close1, t2, t3, t4, t_close4, t5;
  std::optional<std::vector<uint8_t>> body1, body4, body5;

  std::thread srv([&] {
    if (auto k = server.accept()) {
      if (k->read_head() && k->reply(k100)) body1 = k->read_body(a.size());
      k->s.close();
      t_close1 = Clock::now();
    }
    conn1_done = true;
    if (auto k = server.accept()) {  // attempt 2: closed before any answer
      t2 = Clock::now();
      k->read_head();
    }
    if (auto k = server.accept()) {  // attempt 3: closed before any answer
      t3 = Clock::now();
      k->read_head();
    }
    if (auto k = server.accept()) {  // attempt 4: accepted
      t4 = Clock::now();
      if (k->read_head()) {
        wait_flag(stale_pushed);
        if (k->reply(k100)) body4 = k->read_body(b.size());
      }
      k->s.close();
      t_close4 = Clock::now();
    }
    conn4_done = true;
    if (auto k = server.accept()) {  // after a success the schedule starts over
      t5 = Clock::now();
      if (k->read_head() && k->reply(k100)) body5 = k->read_body(c.size());
    }
  });
  Joiner join_srv{srv};

  be::IcecastClient client(target(server.port()), 1 << 20);
  std::atomic<int> reconnect_calls{0};
  client.on_reconnect([&] { ++reconnect_calls; });
  client.push(a);
  REQUIRE(wait_for([&] { return conn1_done.load(); }));
  // once the client has noticed the drop, nothing is sent until the reconnect
  REQUIRE(wait_for([&] { return client.status().state != be::IcecastState::Streaming; }));
  client.push(stale);  // queued while disconnected: belongs to the old stream, must be discarded
  stale_pushed = true;
  REQUIRE(wait_for([&] { return reconnect_calls.load() == 1; }));
  CHECK(client.status().reconnects == 3);
  client.push(b);  // the producer's fresh stream after the reconnect
  REQUIRE(wait_for([&] { return conn4_done.load(); }));
  REQUIRE(wait_for([&] { return reconnect_calls.load() == 2; }));
  client.push(c);
  srv.join();

  REQUIRE(body1);
  CHECK(*body1 == a);
  REQUIRE(body4);
  CHECK(*body4 == b);  // starts with b: the stale piece never reached the new connection
  REQUIRE(body5);
  CHECK(*body5 == c);

  const auto i1 = ms_between(t_close1, t2), i2 = ms_between(t2, t3), i3 = ms_between(t3, t4),
             i4 = ms_between(t_close4, t5);
  MESSAGE("retry waits (ms): " << i1 << ", " << i2 << ", " << i3 << ", after success " << i4);
  CHECK(i1 >= 95);
  CHECK(i1 < 900);
  CHECK(i2 >= 995);
  CHECK(i3 >= 295);
  CHECK(i3 < 900);
  CHECK(i4 >= 95);
  CHECK(i4 < 900);

  const auto st = client.status();
  CHECK(st.state == be::IcecastState::Streaming);
  CHECK(st.reconnects == 4);
  CHECK(st.bytes_sent == a.size() + b.size() + c.size());
  client.stop();
}

TEST_CASE("Icecast: queue overflow drops the oldest pieces whole and counts them") {
  auto server = FakeServer::make();
  std::atomic<bool> go{false};
  std::optional<std::vector<uint8_t>> body;
  std::thread srv([&] {
    auto k = server.accept();
    if (!k || !k->read_head()) return;
    wait_flag(go);
    if (k->reply(k100)) body = k->read_body(900);
  });
  Joiner join_srv{srv};

  be::IcecastClient client(target(server.port()), 1000);
  std::vector<std::vector<uint8_t>> pieces;
  for (uint32_t i = 0; i < 10; ++i) {
    pieces.push_back(pattern(300, 100 + i));
    client.push(pieces.back());  // nothing leaves before the server answers
  }
  CHECK(client.status().dropped_bytes == 7 * 300);
  client.push(pattern(1500, 99));  // larger than the whole queue: dropped itself
  CHECK(client.status().dropped_bytes == 7 * 300 + 1500);
  go = true;
  srv.join();

  std::vector<uint8_t> expected;
  for (int i = 7; i < 10; ++i) expected.insert(expected.end(), pieces[i].begin(), pieces[i].end());
  REQUIRE(body);
  CHECK(*body == expected);
  CHECK(wait_for([&] { return client.status().bytes_sent == 900; }));
  client.stop();
}

TEST_CASE("Icecast: stop() returns promptly with a stalled server") {
  auto server = FakeServer::make();
  std::atomic<bool> release{false};
  std::vector<std::optional<bp::TcpStream>> backlog;
  std::thread srv;
  Joiner join_srv{srv};
  std::optional<be::IcecastClient> client;

  SUBCASE("server never answers the request") {
    srv = std::thread([&] {
      auto k = server.accept();
      if (k) k->read_head();
      wait_flag(release);
    });
    client.emplace(target(server.port()), 1 << 20);
    REQUIRE(wait_for([&] { return client->status().state == be::IcecastState::Connecting; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let it sit in the handshake
  }
  SUBCASE("server never reads the stream") {
    srv = std::thread([&] {
      auto k = server.accept();
      if (k && k->read_head()) k->reply(k100);
      wait_flag(release);
    });
    client.emplace(target(server.port()), 64 << 20);
    REQUIRE(wait_for([&] { return client->status().state == be::IcecastState::Streaming; }));
    // Far more than the loopback socket buffers hold; push() itself never waits on the network.
    const auto piece = pattern(1 << 16, 5);
    const auto t0 = Clock::now();
    for (int i = 0; i < 512; ++i) client->push(piece);
    CHECK(ms_since(t0) < 2000);
    // wait until sending is stuck: no progress over 200 ms with data still queued
    uint64_t last = UINT64_MAX;
    REQUIRE(wait_for(
        [&] {
          const uint64_t now = client->status().bytes_sent;
          const bool stuck = now == last && now > 0;
          last = now;
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          return stuck;
        },
        20000));
    CHECK(client->status().bytes_sent < uint64_t(512) << 16);
    CHECK(client->status().state == be::IcecastState::Streaming);
  }
  SUBCASE("connect is slow (name lookup / connect delayed by the test hook)") {
    be::testing::set_icecast_connect_delay_ms(3000);
    client.emplace(target(server.port()), 1 << 20);
    REQUIRE(wait_for([&] { return client->status().state == be::IcecastState::Connecting; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // inside the slow connect
    be::testing::set_icecast_connect_delay_ms(0);  // read by the helper already: affects later connects only
  }
  SUBCASE("listen backlog full: connect refused or never completes") {
    // Nobody accepts; fill the backlog. macOS then refuses further connects (the client waits
    // for its retry), other systems may leave the connect hanging or complete it unanswered.
    for (int i = 0; i < 256; ++i) {
      std::string err;
      auto s = bp::TcpStream::connect("127.0.0.1", server.port(), 50, &err);
      if (!s) break;
      backlog.push_back(std::move(s));
    }
    client.emplace(target(server.port()), 1 << 20);
    REQUIRE(wait_for([&] { return client->status().state == be::IcecastState::Connecting; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const auto before = client->status();
  const auto t0 = Clock::now();
  client->stop();
  const auto took = ms_since(t0);
  MESSAGE("stop() took " << took << " ms, state before " << int(before.state) << " \"" << before.error
                         << "\", " << backlog.size() << " backlog connections");
  CHECK(took < 1000);
  CHECK(client->status().state == be::IcecastState::Idle);
  client->push(pattern(10, 1));  // ignored after stop
  CHECK(client->status().state == be::IcecastState::Idle);
  release = true;
  if (srv.joinable()) srv.join();
}

#endif  // BAM_WITH_CODECS
