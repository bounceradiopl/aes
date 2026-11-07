// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAES Master 1.2.2 panel hardening: the Bounce Encoder 1.0 attack tests, run against the
// BounceAES Master HTTP server. Every server here listens on 127.0.0.1 on a free port; the engine
// runs without PTP and SAP on the loopback interface.
#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "bam/auth.hpp"
#include "bam/config.hpp"
#include "bam/engine.hpp"
#include "bam/http/guard.hpp"
#include "bam/http_server.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"
#include "http_attack.hpp"
#include "test_util.hpp"

namespace bp = bam::platform;
using json = nlohmann::json;
using namespace http_attack;

namespace {

constexpr const char* kLoopback = "127.0.0.1";
constexpr const char* kPassword = "tajnehaslo1";

std::string unique_suffix() {
  static int n = 0;
  return std::to_string(bp::current_pid()) + ".x" + std::to_string(++n) + "." + std::to_string(bp::monotonic_ns() % 100000);
}

struct EngineOnly {
  std::string suffix = unique_suffix();
  bam::EngineOptions opts;
  std::unique_ptr<bam::Engine> engine;

  EngineOnly() {
    opts.config_path = "bam_harden_config_" + suffix + ".json";
    opts.shm_name = "bamtest.x." + suffix;
    opts.event_name = "bamtest.xe." + suffix;
    opts.enable_ptp = false;
    opts.enable_sap = false;
    auto cfg = bam::default_config();
    cfg.interface_ip = kLoopback;  // tests never send multicast onto a real network
    cfg.http_bind = kLoopback;
    std::ofstream(opts.config_path) << bam::config_to_json(cfg);
    engine = std::make_unique<bam::Engine>(opts);
    std::string err;
    REQUIRE_MESSAGE(engine->start(&err), err);
  }
  ~EngineOnly() {
    engine->stop();
    std::remove(opts.config_path.c_str());
    std::remove((opts.config_path + ".tmp").c_str());
    bp::SharedMemory::remove(opts.shm_name);
    bp::NamedEvent::remove(opts.event_name);
  }
};

struct Fixture : EngineOnly {
  std::unique_ptr<bam::HttpServer> server;
  std::unique_ptr<httplib::Client> client;
  httplib::Headers auth;
  uint16_t port = 0;

  Fixture() {
    std::string err;
    server = std::make_unique<bam::HttpServer>(*engine);
    REQUIRE_MESSAGE(server->start(kLoopback, 0, &err), err);
    port = server->port();
    REQUIRE(port != 0);
    client = std::make_unique<httplib::Client>(kLoopback, port);
    client->set_read_timeout(10, 0);
  }
  ~Fixture() { server->stop(); }
  void take_cookie(const httplib::Result& r) {
    REQUIRE(r);
    const std::string c = r->get_header_value("Set-Cookie");
    REQUIRE(!c.empty());
    auth = {{"Cookie", c.substr(0, c.find(';'))}};
  }
  void setup_and_login() {
    auto r = client->Post("/api/setup", json{{"password", kPassword}}.dump(), "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 200);
    take_cookie(r);
  }
  httplib::Result post(const std::string& path, const json& body) { return client->Post(path, auth, body.dump(), "application/json"); }
  int get_status(const std::string& path, const httplib::Headers& h) {
    auto r = client->Get(path, h);
    REQUIRE(r);
    return r->status;
  }
  std::string cookie() const { return auth.begin()->second; }
  bool password_set() { return json::parse(client->Get("/api/auth-state")->body)["password_set"].get<bool>(); }
};

struct ResetHooks {
  ~ResetHooks() {
    bam::testing::set_http_clock_offset_ns(0);
    bam::testing::set_login_verified_hook({});
  }
};

}  // namespace

TEST_CASE("BounceAES HTTP: a request hidden in a refused request's body is never executed") {
  Fixture f;
  const uint16_t port = f.port;
  // read-ahead boundaries, and next to them
  const std::vector<size_t> offsets = {4096, 4095, 4097, 8192, 2000};

  SUBCASE("(a) inside a text/plain body: 415, then the connection closes") {
    for (size_t offset : offsets) {
      CAPTURE(offset);
      const auto head = [](size_t n) { return head_with_length("POST", "/api/setup", "text/plain", n); };
      const RawExchange x = raw_exchange(port, hiding_request(head, offset, kLastRequest));
      CHECK(x.closed);
      CHECK(statuses_or_reset(x.statuses, 415));  // one request per connection: nothing after it is read
      CHECK_FALSE(f.password_set());
    }
  }
  SUBCASE("(b) behind a Content-Length over 1 MB: 413 and the server closes the connection") {
    for (size_t offset : offsets) {
      CAPTURE(offset);
      const auto head = [](size_t n) { return head_with_length("POST", "/api/setup", "application/json", n, "", "2000000"); };
      const RawExchange x = raw_exchange(port, hiding_request(head, offset, kLastRequest));
      CHECK(x.closed);
      CHECK(statuses_or_reset(x.statuses, 413));
      // the whole answer before the close; on Windows a reset may cut the body off after the status line
      if (!x.statuses.empty() && !kResetMayLoseAnswer) CHECK(x.text.find("za duża") != std::string::npos);
      CHECK_FALSE(f.password_set());
    }
  }
  SUBCASE("(c) inside a chunked body, or a body without Content-Length: 411 and the server closes the connection") {
    for (size_t offset : offsets) {
      CAPTURE(offset);
      const auto chunked = [](size_t n) {
        std::ostringstream hex;
        hex << std::hex << n;
        return "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n" +
               hex.str() + "\r\n";
      };
      RawExchange x = raw_exchange(port, hiding_request(chunked, offset, "\r\n0\r\n\r\n" + kLastRequest));
      CHECK(x.closed);
      CHECK(statuses_or_reset(x.statuses, 411));
      const auto no_length = [](size_t) {
        return std::string("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n\r\n");
      };
      x = raw_exchange(port, hiding_request(no_length, offset, kLastRequest));
      CHECK(x.closed);
      CHECK(statuses_or_reset(x.statuses, 411));
      CHECK_FALSE(f.password_set());
    }
  }
  SUBCASE("(d) inside an unauthenticated request to a protected endpoint: 401, then the connection closes") {
    for (const std::string path : {"/api/outputs", "/api/subscriptions", "/api/password", "/api/logout", "/api/stats/reset",
                                   "/api/config", "/api/routing", "/api/mixes"}) {
      for (size_t offset : offsets) {
        CAPTURE(path);
        CAPTURE(offset);
        const bool put = path == "/api/config" || path == "/api/routing" || path == "/api/mixes";
        const auto head = [&](size_t n) { return head_with_length(put ? "PUT" : "POST", path, "application/json", n); };
        const RawExchange x = raw_exchange(port, hiding_request(head, offset, kLastRequest));
        CHECK(x.closed);
        CHECK(statuses_or_reset(x.statuses, 401));
        CHECK_FALSE(f.password_set());
      }
    }
  }
}

TEST_CASE("BounceAES HTTP: one request per connection, whatever httplib makes of a request") {
  Fixture f;
  const uint16_t port = f.port;
  const std::vector<size_t> offsets = {4096, 8192, 12288};  // httplib's 4 KB read-ahead boundaries
  // The first response only, then the server closes: the hidden setup copies are never parsed.
  auto check_vector = [&](const std::string& name, const std::function<std::string(size_t)>& head, const std::string& tail = "") {
    for (size_t offset : offsets) {
      CAPTURE(name);
      CAPTURE(offset);
      const RawExchange x = raw_exchange(port, hiding_request(head, offset, tail + kLastRequest));
      CHECK(x.closed);
      CHECK_FALSE(f.password_set());
      // exactly one answer, a refusal; on Windows a reset may (rarely) cost the client even that one
      if (kResetMayLoseAnswer && x.statuses.empty()) continue;
      CHECK(x.statuses.size() == 1);
      if (!x.statuses.empty()) CHECK(x.statuses[0] >= 400);
    }
  };
  auto with = [](std::string method, std::string headers) {
    return [method, headers](size_t n) {
      std::string h = headers;
      const std::string marker = "{n}";
      for (size_t p; (p = h.find(marker)) != std::string::npos;) h.replace(p, marker.size(), std::to_string(n));
      return method + " /api/setup HTTP/1.1\r\nHost: 127.0.0.1\r\n" + h + "\r\n";
    };
  };
  auto chunk_head = [](std::string method, std::string headers) {
    return [method, headers](size_t n) {
      std::ostringstream hex;
      hex << std::hex << n;
      return method + " /api/setup HTTP/1.1\r\nHost: 127.0.0.1\r\n" + headers + "Transfer-Encoding: chunked\r\n\r\n" + hex.str() + "\r\n";
    };
  };
  const std::string chunk_end = "\r\n0\r\n\r\n";

  SUBCASE("(1) multipart/form-data without a boundary, and with a malformed part") {
    check_vector("multipart, no boundary", with("POST", "Content-Type: multipart/form-data\r\nContent-Length: {n}\r\n"));
    check_vector("multipart, bad part", with("POST", "Content-Type: multipart/form-data; boundary=xyz\r\nContent-Length: {n}\r\n"));
  }
  SUBCASE("(2) Content-Encoding gzip and br") {
    check_vector("gzip", with("POST", "Content-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: {n}\r\n"));
    check_vector("br", with("POST", "Content-Type: application/json\r\nContent-Encoding: br\r\nContent-Length: {n}\r\n"));
  }
  SUBCASE("(3) HEAD with a chunked body, a Content-Length over 1 MB, two Content-Lengths") {
    check_vector("HEAD chunked", chunk_head("HEAD", ""), chunk_end);
    check_vector("HEAD > 1 MB", with("HEAD", "Content-Length: 2000000\r\n"));
    check_vector("HEAD two lengths", with("HEAD", "Content-Length: {n}\r\nContent-Length: {n}\r\n"));
  }
  SUBCASE("(4) Range with a Content-Length over 1 MB and with a chunked body") {
    for (const std::string range : {"bytes=0-", "bytes=-5"}) {
      check_vector("Range " + range + " > 1 MB",
                   with("POST", "Content-Type: application/json\r\nRange: " + range + "\r\nContent-Length: 2000000\r\n"));
      check_vector("Range " + range + " chunked", chunk_head("POST", "Content-Type: application/json\r\nRange: " + range + "\r\n"), chunk_end);
    }
  }
  SUBCASE("(5) pipelining: two requests on one connection, only the first is answered") {
    const std::string get = "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    const RawExchange x = raw_exchange(port, get + get);
    CHECK(x.closed);
    CHECK(x.statuses == std::vector<int>{200});
  }
}

TEST_CASE("BounceAES HTTP: methods, encodings, ranges and body types outside the allowlist are refused") {
  Fixture f;
  const uint16_t port = f.port;
  CHECK(raw_status(port, "HEAD /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n") == 405);
  CHECK(raw_status(port, "OPTIONS /api/setup HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://example.com\r\n\r\n") == 405);
  CHECK(status_or_reset(
      raw_status(port, "PATCH /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"), 405));
  CHECK(status_or_reset(raw_status(port, "POST /api/setup HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                         "Content-Encoding: identity\r\nContent-Length: 2\r\n\r\n{}"),
                        415));
  CHECK(raw_status(port, "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=0-10\r\n\r\n") == 416);
  // the JSON media type exactly; a charset parameter is fine, anything else is not
  auto setup = [&](const std::string& type) {
    const std::string body = json{{"password", "x"}}.dump();  // too short: 400 once the type is accepted
    return raw_status(port, "POST /api/setup HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: " + type + "\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body);
  };
  CHECK(setup("application/json") == 400);
  CHECK(setup("Application/JSON; charset=UTF-8") == 400);
  CHECK(setup("application/json;charset=utf-8") == 400);
  CHECK(setup("application/jsonx") == 415);
  CHECK(setup("application/json; boundary=x") == 415);
  CHECK(status_or_reset(setup("multipart/form-data; boundary=x"), 415));  // refused on its headers (body unread)
  CHECK(setup("text/plain") == 415);
  CHECK_FALSE(f.password_set());
  // the panel files still answer
  CHECK(f.client->Get("/")->status == 200);
}

TEST_CASE("BounceAES HTTP: bodies without a length, chunked or too large are refused before they are read") {
  Fixture f;
  const uint16_t port = f.port;
  CHECK(status_or_reset(raw_status(port, chunked_request("POST", "/api/setup", json{{"password", kPassword}}.dump())), 411));
  CHECK_FALSE(f.password_set());
  CHECK(status_or_reset(raw_status(port, "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n\r\n"
                                         "{\"password\":\"zlehaslo123\"}"),
                        411));
  CHECK(status_or_reset(raw_status(port, "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                         "Transfer-Encoding: chunked\r\n\r\n300000\r\n" + std::string(65536, 'a')),
                        411));

  f.setup_and_login();
  CHECK(status_or_reset(raw_status(port, chunked_request("PUT", "/api/config", R"({"ptp_domain":7})", f.cookie())), 411));
  CHECK(f.engine->config().ptp_domain == 0);
  CHECK(raw_status(port, "PUT /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nCookie: " + f.cookie() +
                             "\r\nContent-Length: 2000000000\r\n\r\n",
                   2000) == 413);
  CHECK(f.get_status("/api/config", f.auth) == 200);  // the server still answers normally
}

TEST_CASE("BounceAES HTTP: a body must be JSON (no cross-origin simple requests)") {
  Fixture f;
  auto r = f.client->Post("/api/setup", json{{"password", kPassword}}.dump(), "text/plain");
  REQUIRE(r);
  CHECK(r->status == 415);
  CHECK(json::parse(r->body)["error"].get<std::string>().find("application/json") != std::string::npos);
  CHECK(f.client->Post("/api/setup", "password=tajnehaslo1", "application/x-www-form-urlencoded")->status == 415);
  CHECK_FALSE(f.password_set());
  CHECK(f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "text/plain")->status == 415);
  auto big = f.client->Post("/api/setup", std::string(200000, 'x'), "text/plain");
  REQUIRE(big);
  CHECK(big->status == 415);

  f.setup_and_login();
  CHECK(f.client->Put("/api/config", f.auth, R"({"ptp_domain":3})", "text/plain")->status == 415);
  CHECK(f.engine->config().ptp_domain == 0);
  CHECK(f.client->Put("/api/routing", f.auth, R"({"dst": "asio-in:1", "src": "net-in:1"})", "text/plain")->status == 415);
  CHECK(f.client->Put("/api/config", f.auth, R"({"ptp_domain":3})", "application/json; charset=utf-8")->status == 200);
  CHECK(f.engine->config().ptp_domain == 3);
}

TEST_CASE("BounceAES HTTP: the bundled panel's requests still work") {
  // panel/app.js: JSON bodies with "Content-Type: application/json"; logout and stats reset are
  // POSTs without a body (a browser sends "Content-Length: 0" and no type); the tray polls
  // GET /api/health.
  Fixture f;
  const uint16_t port = f.port;
  CHECK(raw_status(port, "GET /api/health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n") == 200);
  f.setup_and_login();
  CHECK(raw_status(port, "POST /api/stats/reset HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " + f.cookie() +
                             "\r\nContent-Length: 0\r\n\r\n") == 200);
  CHECK(f.client->Post("/api/stats/reset", f.auth)->status == 200);
  CHECK(f.client->Put("/api/routing", f.auth, R"({"dst": "asio-in:1", "src": null})", "application/json")->status == 200);
  CHECK(f.client->Delete("/api/subscriptions/40", f.auth)->status == 404);  // the handler ran
  CHECK(f.client->Get("/api/outputs/1/sdp", f.auth)->status == 200);
  CHECK(raw_status(port, "POST /api/logout HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " + f.cookie() + "\r\nContent-Length: 0\r\n\r\n") ==
        200);
  CHECK(f.get_status("/api/config", f.auth) == 401);
}

TEST_CASE("BounceAES HTTP: every API route but the public ones needs a session") {
  Fixture f;
  for (const char* path : {"/api/config", "/api/routing", "/api/streams", "/api/status", "/api/stats", "/api/logs", "/api/interfaces",
                           "/api/live", "/api/outputs/1/sdp", "/api/devices/1/sdp"}) {
    auto r = f.client->Get(path);
    REQUIRE(r);
    CHECK_MESSAGE(r->status == 401, path);
  }
  CHECK(f.client->Put("/api/config", "{}", "application/json")->status == 401);
  CHECK(f.client->Put("/api/routing", "{}", "application/json")->status == 401);
  CHECK(f.client->Put("/api/mixes", "{}", "application/json")->status == 401);
  CHECK(f.client->Post("/api/outputs", "{}", "application/json")->status == 401);
  CHECK(f.client->Post("/api/subscriptions", "{}", "application/json")->status == 401);
  CHECK(f.client->Post("/api/password", "{}", "application/json")->status == 401);
  CHECK(f.client->Post("/api/logout", "", "application/json")->status == 401);
  CHECK(f.client->Post("/api/stats/reset", "", "application/json")->status == 401);
  CHECK(f.client->Delete("/api/outputs/1")->status == 401);
  CHECK(f.client->Delete("/api/subscriptions/1")->status == 401);
  for (const char* path : {"/api/health", "/api/auth-state", "/", "/app.js"}) CHECK_MESSAGE(f.client->Get(path)->status == 200, path);
}

TEST_CASE("BounceAES HTTP: the first password only through a loopback Host (no DNS rebinding)") {
  const std::string body = json{{"password", kPassword}}.dump();
  {
    Fixture f;
    for (const char* host : {"evil.example", "evil.example:8067", "127.0.0.1.evil.example", "localhost.evil.example",
                             "192.168.1.10:8067", "[::2]:8067", ""}) {
      auto r = f.client->Post("/api/setup", {{"Host", host}}, body, "application/json");
      REQUIRE(r);
      CHECK_MESSAGE(r->status == 403, host);
      CHECK(json::parse(r->body)["error"].get<std::string>().find("127.0.0.1") != std::string::npos);
      // the panel shows the "open http://127.0.0.1:port" hint instead of the form
      CHECK(json::parse(f.client->Get("/api/auth-state", {{"Host", host}})->body)["local"] == false);
    }
    // HTTP/1.0 without any Host
    CHECK(raw_status(f.port, "POST /api/setup HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n" + body) == 403);
    CHECK_FALSE(f.password_set());
    CHECK(f.engine->config().password_hash.empty());
  }
  for (const std::string host : {"127.0.0.1:%", "localhost:%", "LocalHost:%", "127.0.0.1", "localhost", "[::1]:%", "[::1]"}) {
    Fixture f;
    std::string h = host;
    if (const size_t pct = h.find('%'); pct != std::string::npos) h.replace(pct, 1, std::to_string(f.port));
    CHECK(json::parse(f.client->Get("/api/auth-state", {{"Host", h}})->body)["local"] == true);
    auto r = f.client->Post("/api/setup", {{"Host", h}}, body, "application/json");
    REQUIRE(r);
    CHECK_MESSAGE(r->status == 200, h);
  }
}

TEST_CASE("BounceAES HTTP: only one first password, even when set concurrently") {
  Fixture f;
  std::atomic<int> ok{0}, conflict{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {  // within httplib's listen backlog of 5
    threads.emplace_back([&, i] {
      httplib::Client c(kLoopback, f.port);
      c.set_read_timeout(10, 0);
      auto r = c.Post("/api/setup", json{{"password", "haslo-nr-" + std::to_string(i)}}.dump(), "application/json");
      if (r && r->status == 200) ++ok;
      if (r && r->status == 409) ++conflict;
    });
  }
  for (auto& t : threads) t.join();
  CHECK(ok == 1);
  CHECK(conflict == 3);
}

TEST_CASE("BounceAES HTTP: failed logins are throttled without holding a request thread") {
  ResetHooks reset;
  Fixture f;
  f.setup_and_login();
  const int64_t t0 = bp::monotonic_ns();
  for (int i = 0; i < 5; ++i) CHECK(f.client->Post("/api/login", R"({"password":"zlehaslo123"})", "application/json")->status == 401);
  CHECK(bp::monotonic_ns() - t0 < 2'400'000'000);  // no sleeping per failure (5 x 500 ms would be 2.5 s)
  // the address is now refused for a while, even with the right password
  auto r = f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json");
  REQUIRE(r);
  CHECK(r->status == 429);
  CHECK(json::parse(r->body)["error"] == "zbyt wiele nieudanych prób, spróbuj za chwilę");
  CHECK(f.get_status("/api/config", f.auth) == 200);  // a session already open is not affected
  // 30 s later the address may try again
  bam::testing::set_http_clock_offset_ns(31'000'000'000);
  CHECK(f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json")->status == 200);
}

TEST_CASE("BounceAES HTTP: concurrent failed logins cannot outrun the throttle") {
  Fixture f;
  f.setup_and_login();
  check_login_burst(concurrent_wrong_logins(f.port, 24), 24);
  // the address is blocked now, even with the right password; the open session is not affected
  CHECK(f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json")->status == 429);
  CHECK(f.get_status("/api/config", f.auth) == 200);
}

TEST_CASE("BounceAES HTTP: an output or subscription number too large for an int is 404, not 500") {
  Fixture f;
  f.setup_and_login();
  const std::string huge = "99999999999999999999";
  for (const std::string& path : {"/api/outputs/" + huge + "/sdp", "/api/devices/" + huge + "/sdp"}) {
    auto r = f.client->Get(path, f.auth);
    REQUIRE(r);
    CHECK_MESSAGE(r->status == 404, path);
    CHECK(json::parse(r->body)["error"] == "brak wyjścia");
  }
  auto out = f.client->Delete("/api/outputs/" + huge, f.auth);
  REQUIRE(out);
  CHECK(out->status == 404);
  CHECK(json::parse(out->body)["error"] == "brak wyjścia");
  auto sub = f.client->Delete("/api/subscriptions/" + huge, f.auth);
  REQUIRE(sub);
  CHECK(sub->status == 404);
  CHECK(json::parse(sub->body)["error"] == "brak subskrypcji");
  CHECK(f.client->Get("/api/outputs/1/sdp", f.auth)->status == 200);  // a normal number still works
}

TEST_CASE("BounceAES HTTP: the live feed ends with its session; a password change ends the other sessions") {
  Fixture f;
  f.setup_and_login();

  // reads /api/live with `cookie` until the server ends it (true) or 5 s pass (false)
  auto watch = [&](const std::string& cookie, std::atomic<int>& events, std::atomic<bool>& ended) {
    httplib::Client c(kLoopback, f.port);
    c.set_read_timeout(10, 0);
    const int64_t deadline = bp::monotonic_ns() + 5'000'000'000;
    bool gave_up = false;
    c.Get("/api/live", httplib::Headers{{"Cookie", cookie}}, [&](const char*, size_t) {
      ++events;
      if (bp::monotonic_ns() > deadline) gave_up = true;
      return !gave_up;
    });
    ended = !gave_up;
  };

  {  // logout
    std::atomic<int> events{0};
    std::atomic<bool> ended{false};
    std::thread t([&] { watch(f.cookie(), events, ended); });
    JoinOnExit join{t};
    REQUIRE(wait_for([&] { return events > 0; }, 3000));
    CHECK(f.client->Post("/api/logout", f.auth)->status == 200);
    t.join();
    CHECK(ended);
  }

  // two sessions; the second changes the password
  auto first = f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json");
  REQUIRE(first->status == 200);
  const std::string other = first->get_header_value("Set-Cookie").substr(0, first->get_header_value("Set-Cookie").find(';'));
  auto second = f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json");
  REQUIRE(second->status == 200);
  f.take_cookie(second);

  std::atomic<int> events{0};
  std::atomic<bool> ended{false};
  std::thread t([&] { watch(other, events, ended); });
  JoinOnExit join{t};
  REQUIRE(wait_for([&] { return events > 0; }, 3000));
  auto change = f.post("/api/password", {{"current", kPassword}, {"password", "nowehaslo99"}});
  REQUIRE(change->status == 200);
  t.join();
  CHECK(ended);
  CHECK(f.get_status("/api/config", httplib::Headers{{"Cookie", other}}) == 401);
  // the session that changed the password goes on (with the cookie it was given)
  if (!change->get_header_value("Set-Cookie").empty()) f.take_cookie(change);
  CHECK(f.get_status("/api/config", f.auth) == 200);
  CHECK(f.client->Post("/api/login", json{{"password", "nowehaslo99"}}.dump(), "application/json")->status == 200);
}

TEST_CASE("BounceAES HTTP: an open live feed does not keep its session alive") {
  ResetHooks reset;
  Fixture f;
  f.setup_and_login();
  constexpr int64_t hour = 3600LL * 1'000'000'000;
  std::atomic<int> events{0};
  std::atomic<bool> ended{false};
  std::thread t([&] {
    httplib::Client c(kLoopback, f.port);
    c.set_read_timeout(10, 0);
    const int64_t deadline = bp::monotonic_ns() + 8'000'000'000;
    bool gave_up = false;
    c.Get("/api/live", f.auth, [&](const char*, size_t) {
      ++events;
      gave_up = bp::monotonic_ns() > deadline;
      return !gave_up;
    });
    ended = !gave_up;
  });
  JoinOnExit join{t};
  REQUIRE(wait_for([&] { return events > 0; }, 3000));
  // 11 h 59 min later the feed still runs; a sliding check would now have moved the expiry on
  bam::testing::set_http_clock_offset_ns(12 * hour - 60'000'000'000);
  const int seen = events;
  REQUIRE(wait_for([&] { return events > seen + 2; }, 3000));
  CHECK_FALSE(ended);
  // 12 h after the login (not after the last event) the session is over and the feed ends
  bam::testing::set_http_clock_offset_ns(12 * hour + 1'000'000'000);
  REQUIRE(wait_for([&] { return ended.load(); }, 3000));
  t.join();
  CHECK(ended);
  CHECK(f.get_status("/api/config", f.auth) == 401);
}

TEST_CASE("BounceAES HTTP: a login that checked the old password does not survive a password change") {
  ResetHooks reset;
  Fixture f;
  f.setup_and_login();  // the session that will change the password

  std::mutex m;
  std::condition_variable cv;
  bool in_login = false, changed = false;
  bam::testing::set_login_verified_hook([&] {
    std::unique_lock lock(m);
    in_login = true;
    cv.notify_all();
    cv.wait_for(lock, std::chrono::seconds(5), [&] { return changed; });
  });
  int login_status = 0;
  std::string login_cookie;
  std::thread t([&] {
    httplib::Client c(kLoopback, f.port);
    c.set_read_timeout(10, 0);
    auto r = c.Post("/api/login", json{{"password", kPassword}}.dump(), "application/json");
    if (r) {
      login_status = r->status;
      login_cookie = r->get_header_value("Set-Cookie");
    }
  });
  JoinOnExit join{t};
  {
    std::unique_lock lock(m);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return in_login; }));
  }
  auto change = f.post("/api/password", {{"current", kPassword}, {"password", "nowehaslo99"}});
  REQUIRE(change->status == 200);
  {
    std::lock_guard lock(m);
    changed = true;
  }
  cv.notify_all();
  t.join();
  bam::testing::set_login_verified_hook({});
  CHECK(login_status == 401);
  CHECK(login_cookie.empty());
  f.take_cookie(change);
  CHECK(f.get_status("/api/config", f.auth) == 200);  // the session that changed it goes on
}

TEST_CASE("BounceAES HTTP: a server stopped right after it started does not hang") {
  // httplib ignores a stop() that comes before its listen thread runs; start() waits for it
  EngineOnly e;
  std::string err;
  for (int i = 0; i < 50; ++i) {
    bam::HttpServer server(*e.engine);
    REQUIRE_MESSAGE(server.start(kLoopback, 0, &err), err);
    server.stop();  // returns: the listen thread has ended
  }
  {
    bam::HttpServer server(*e.engine);
    REQUIRE(server.start(kLoopback, 0, &err));
  }  // the destructor stops it as well
}

TEST_CASE("BounceAES HTTP: deeply nested, container-heavy or very wide JSON is refused before it is parsed, even without a session") {
  Fixture f;
  const auto error_of = [](const httplib::Result& r) { return json::parse(r->body)["error"].get<std::string>(); };
  // a 1 MB "[[[[…" used to crash the service (stack overflow destroying the parsed value)
  const std::string nested = std::string(500'000, '[') + std::string(500'000, ']');  // valid JSON, 1 MB
  const int64_t t0 = bp::monotonic_ns();
  auto r = f.client->Post("/api/setup", nested, "application/json");
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zagnieżdż") != std::string::npos, r->body);
  CHECK_FALSE(f.password_set());
  f.setup_and_login();
  // from here on, /api/login without a session
  const auto login = [&](const std::string& body) { return f.client->Post("/api/login", body, "application/json"); };
  r = login(nested);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zagnieżdż") != std::string::npos, r->body);
  std::string objects;
  for (int i = 0; i < 40; ++i) objects += "{\"a\":";
  objects += "1" + std::string(40, '}');
  r = login(objects);
  REQUIRE(r);
  CHECK(r->status == 400);
  // many small containers
  std::string flat = "[";
  for (int i = 0; i < 200'000; ++i) flat += i ? ",{}" : "{}";
  flat += "]";
  r = login(flat);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zbyt wiele") != std::string::npos, r->body);
  // a flat list of 1 MB of numbers
  std::string numbers = "[0";
  while (numbers.size() < 1'000'000) numbers += ",0";
  numbers += "]";
  r = login(numbers);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zbyt wiele wartości") != std::string::npos, r->body);
  // an object of 90 000 keys (ordered_json parses it in quadratic time: seconds of CPU)
  std::string keys = "{";
  for (int i = 0; i < 90'000; ++i) keys += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
  keys += "}";
  const int64_t t1 = bp::monotonic_ns();
  r = login(keys);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK(bp::monotonic_ns() - t1 < 1'000'000'000);
  // under the value cap, over the members of one object
  std::string wide = "{";
  for (int i = 0; i < 2'500; ++i) wide += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
  wide += "}";
  r = login(wide);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zbyt wiele pól") != std::string::npos, r->body);
  CHECK(bp::monotonic_ns() - t0 < 5'000'000'000);  // refused quickly: nothing was built
  // brackets, braces, commas and colons inside a string are not structure
  r = login(json{{"password", std::string(100'000, '{') + std::string(50'000, ',') + std::string(50'000, ':')}}.dump());
  REQUIRE_MESSAGE(r, httplib::to_string(r.error()));
  CHECK(r->status == 401);
  // 16 levels are fine, 17 are not
  const auto deep = [](int levels) {  // the object counts as one level
    return R"({"password":"zlehaslo123","x":)" + std::string(size_t(levels - 1), '[') + std::string(size_t(levels - 1), ']') + "}";
  };
  CHECK(login(deep(16))->status == 401);
  CHECK(login(deep(17))->status == 400);
  // the panel's own requests still parse: the whole configuration back as it came
  auto cfg = f.client->Get("/api/config", f.auth);
  REQUIRE(cfg);
  REQUIRE(cfg->status == 200);
  auto put = f.client->Put("/api/config", f.auth, cfg->body, "application/json");
  REQUIRE(put);
  CHECK_MESSAGE(put->status == 200, put->body);
  // a large configuration (32 mixes of 16 sources, 256 routes, more than there can be) is read and
  // judged on its content, never refused for its shape, and stays under a third of the value cap
  bam::Config big = bam::default_config();
  for (int m = 0; m < bam::kMaxMixes; ++m) {
    bam::MixConfig mix;
    for (int i = 0; i < bam::kMaxMixSources; ++i) mix.sources.push_back({"net-in:" + std::to_string(i + 1), -6.0, false});
    big.mixes["mix-destination:" + std::to_string(m + 1)] = mix;
  }
  for (int d = 0; d < 256; ++d) big.routes["destination:" + std::to_string(d + 1)] = "source:" + std::to_string(d + 1);
  const std::string text = bam::config_to_json(big);
  CHECK_MESSAGE(bam::http::json_shape_error(text).empty(), bam::http::json_shape_error(text));
  CAPTURE(json_value_separators(text));
  CHECK(json_value_separators(text) * 3 < bam::http::kMaxJsonValues);
  put = f.client->Put("/api/config", f.auth, text, "application/json");
  REQUIRE(put);
  CHECK_MESSAGE(put->body.find("w treści żądania") == std::string::npos, put->body.substr(0, 400));
}

TEST_CASE("BounceAES HTTP: a login with a 100 KB password is answered within a second") {
  Fixture f;
  f.setup_and_login();
  const int64_t t0 = bp::monotonic_ns();
  auto r = f.client->Post("/api/login", json{{"password", std::string(100'000, 'x')}}.dump(), "application/json");
  REQUIRE(r);
  CHECK(r->status == 401);
  CHECK(bp::monotonic_ns() - t0 < 1'000'000'000);
}
