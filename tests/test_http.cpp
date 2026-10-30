// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>
#include <httplib.h>

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

#include "bam/engine.hpp"
#include "bam/http_server.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"

namespace bp = bam::platform;
using json = nlohmann::json;

namespace {
struct Fixture {
  std::string suffix = std::to_string(bp::current_pid()) + ".h" + std::to_string(bp::monotonic_ns() % 100000);
  bam::EngineOptions opts;
  std::unique_ptr<bam::Engine> engine;
  std::unique_ptr<bam::HttpServer> server;
  uint16_t port = static_cast<uint16_t>(20000 + bp::current_pid() % 20000);
  httplib::Client client{"127.0.0.1", port};
  std::string cookie;

  Fixture() {
    opts.config_path = "bam_http_config_" + suffix + ".json";
    opts.shm_name = "bamtest.h." + suffix;
    opts.event_name = "bamtest.hp." + suffix;
    opts.enable_ptp = false;
    opts.enable_sap = false;
    auto cfg = bam::default_config();
    cfg.interface_ip = "127.0.0.1";  // tests never send multicast onto a real network
    std::ofstream(opts.config_path) << bam::config_to_json(cfg);
    engine = std::make_unique<bam::Engine>(opts);
    std::string err;
    REQUIRE_MESSAGE(engine->start(&err), err);
    server = std::make_unique<bam::HttpServer>(*engine);
    REQUIRE_MESSAGE(server->start("127.0.0.1", port, &err), err);
    client.set_read_timeout(5, 0);
  }
  ~Fixture() {
    server->stop();
    engine->stop();
    std::remove(opts.config_path.c_str());
    bp::SharedMemory::remove(opts.shm_name);
    bp::NamedEvent::remove(opts.event_name);
  }
  httplib::Headers auth() const { return {{"Cookie", cookie}}; }
  void login(const std::string& setcookie) { cookie = setcookie.substr(0, setcookie.find(';')); }
};
}  // namespace

TEST_CASE("HTTP API: auth flow, config, SDP, panel, live feed") {
  Fixture f;

  auto health = f.client.Get("/api/health");
  REQUIRE(health);
  CHECK(health->status == 200);
  CHECK(json::parse(health->body)["service"] == "ok");

  auto state = f.client.Get("/api/auth-state");
  REQUIRE(state);
  CHECK(json::parse(state->body)["password_set"] == false);
  CHECK(json::parse(state->body)["local"] == true);

  CHECK(f.client.Get("/api/config")->status == 401);

  auto short_pw = f.client.Post("/api/setup", R"({"password":"abc"})", "application/json");
  CHECK(short_pw->status == 400);
  auto setup = f.client.Post("/api/setup", R"({"password":"tajnehaslo1"})", "application/json");
  REQUIRE(setup);
  CHECK(setup->status == 200);
  CHECK(setup->get_header_value("Set-Cookie").find("HttpOnly") != std::string::npos);
  CHECK(f.client.Post("/api/setup", R"({"password":"innehaslo22"})", "application/json")->status == 409);

  CHECK(f.client.Post("/api/login", R"({"password":"zle-haslo"})", "application/json")->status == 401);
  auto login = f.client.Post("/api/login", R"({"password":"tajnehaslo1"})", "application/json");
  REQUIRE(login);
  REQUIRE(login->status == 200);
  f.login(login->get_header_value("Set-Cookie"));

  auto cfg = f.client.Get("/api/config", f.auth());
  REQUIRE(cfg);
  REQUIRE(cfg->status == 200);
  auto cj = json::parse(cfg->body);
  CHECK(cj["devices"].size() == 32);
  CHECK_FALSE(cj.contains("password_hash"));

  auto bad = f.client.Put("/api/devices/2", f.auth(), R"({"tx_ptime_us": 999})", "application/json");
  REQUIRE(bad);
  CHECK(bad->status == 400);
  auto bj = json::parse(bad->body);
  CHECK(bj["ok"] == false);
  CHECK(bj["issues"][0]["device"] == 2);

  auto good = f.client.Put("/api/devices/3", f.auth(), R"({"enabled": true, "name": "Kraków"})", "application/json");
  REQUIRE(good);
  CHECK(good->status == 200);
  CHECK(f.engine->config().devices[2].name == "Kraków");
  CHECK(f.engine->config().devices[2].enabled);
  CHECK(f.engine->config().devices[0].enabled);  // untouched by the partial update

  auto partial = f.client.Put("/api/config", f.auth(), R"({"ptp_domain": 4, "devices": [{}, {"name": "Poznań"}]})", "application/json");
  REQUIRE(partial);
  CHECK(partial->status == 200);
  CHECK(f.engine->config().ptp_domain == 4);
  CHECK(f.engine->config().devices[1].name == "Poznań");
  CHECK(f.engine->config().devices[2].name == "Kraków");
  CHECK_FALSE(f.engine->config().password_hash.empty());

  auto sdp = f.client.Get("/api/devices/1/sdp", f.auth());
  REQUIRE(sdp);
  CHECK(sdp->body.find("a=rtpmap:98 L24/48000/2") != std::string::npos);

  auto status = f.client.Get("/api/status", f.auth());
  REQUIRE(status);
  CHECK(json::parse(status->body)["devices"].size() == 32);
  CHECK(f.client.Get("/api/streams", f.auth())->status == 200);
  CHECK(f.client.Get("/api/logs", f.auth())->status == 200);
  CHECK(f.client.Get("/api/interfaces", f.auth())->status == 200);
  CHECK(f.client.Post("/api/stats/reset", f.auth(), "", "application/json")->status == 200);

  auto index = f.client.Get("/");
  REQUIRE(index);
  CHECK(index->status == 200);
  CHECK(index->body.find("BounceAES Master") != std::string::npos);
  CHECK(f.client.Get("/app.js")->status == 200);
  CHECK(f.client.Get("/style.css")->get_header_value("Content-Type").find("text/css") == 0);
  CHECK(f.client.Get("/nope.txt")->status == 404);

  // live feed: first SSE event arrives quickly
  std::string received;
  const int64_t t0 = bp::monotonic_ns();
  f.client.Get("/api/live", f.auth(), [&](const char* data, size_t len) {
    received.append(data, len);
    return received.find("\n\n") == std::string::npos;  // stop after the first event
  });
  CHECK(received.rfind("event: live", 0) == 0);
  CHECK(bp::monotonic_ns() - t0 < 500'000'000);

  auto logout = f.client.Post("/api/logout", f.auth(), "", "application/json");
  CHECK(logout->status == 200);
  CHECK(f.client.Get("/api/config", f.auth())->status == 401);
}
