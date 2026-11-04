// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>
#include <httplib.h>

#include <cstdio>
#include <fstream>
#include <set>
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
  CHECK(cj["version"] == 2);
  CHECK(cj["outputs"].size() == 32);
  CHECK(cj["subscriptions"].size() == 32);
  CHECK(cj["asio_names"].size() == 32);
  CHECK_FALSE(cj.contains("password_hash"));

  auto bad = f.client.Put("/api/config", f.auth(), R"({"outputs": [{}, {"ptime_us": 999}]})", "application/json");
  REQUIRE(bad);
  CHECK(bad->status == 400);
  auto bj = json::parse(bad->body);
  CHECK(bj["ok"] == false);
  CHECK(bj["issues"][0]["device"] == 2);

  auto partial = f.client.Put("/api/config", f.auth(),
                              R"({"ptp_domain": 4, "ptp_role": "preferred", "outputs": [{}, {"name": "Poznań"}, {"enabled": true, "name": "Kraków"}]})",
                              "application/json");
  REQUIRE(partial);
  CHECK(partial->status == 200);
  CHECK(f.engine->config().ptp_domain == 4);
  CHECK(f.engine->config().ptp_role == bam::PtpRole::Preferred);
  CHECK(f.engine->config().outputs[1].name == "Poznań");
  CHECK(f.engine->config().outputs[2].name == "Kraków");
  CHECK(f.engine->config().outputs[2].enabled);
  CHECK(f.engine->config().outputs[0].enabled);  // untouched by the partial update
  CHECK_FALSE(f.engine->config().password_hash.empty());

  auto sdp = f.client.Get("/api/outputs/1/sdp", f.auth());
  REQUIRE(sdp);
  CHECK(sdp->body.find("a=rtpmap:98 L24/48000/2") != std::string::npos);
  CHECK(f.client.Get("/api/outputs/33/sdp", f.auth())->status == 404);

  // ---- subscriptions and routing ----
  auto add = f.client.Post("/api/subscriptions", f.auth(), json{{"sdp", sdp->body}}.dump(), "application/json");
  REQUIRE(add);
  CHECK(add->status == 200);
  CHECK(json::parse(add->body)["created"] == 1);
  CHECK(f.engine->config().subscriptions[0].enabled);
  CHECK(f.client.Post("/api/subscriptions", f.auth(), R"({"sdp": "nonsense"})", "application/json")->status == 400);
  CHECK(f.client.Post("/api/subscriptions", f.auth(), R"({"sap_id": "1.2.3.4/ffff"})", "application/json")->status == 400);

  auto routing = f.client.Get("/api/routing", f.auth());
  REQUIRE(routing);
  REQUIRE(routing->status == 200);
  auto rj = json::parse(routing->body);
  auto ids = [](const json& list) {
    std::set<std::string> out;
    for (const auto& n : list) out.insert(n["id"].get<std::string>());
    return out;
  };
  CHECK(ids(rj["sources"]).count("net-in:1"));
  CHECK(ids(rj["sources"]).count("asio-out:1"));
  CHECK(ids(rj["dests"]).count("net-out:1"));
  CHECK(ids(rj["dests"]).count("asio-in:1"));
  CHECK(rj["routes"]["net-out:1"] == "asio-out:1");

  auto put = f.client.Put("/api/routing", f.auth(), R"({"dst": "asio-in:1", "src": "net-in:1"})", "application/json");
  REQUIRE(put);
  CHECK(put->status == 200);
  CHECK(json::parse(put->body)["routing"]["routes"]["asio-in:1"] == "net-in:1");
  CHECK(f.engine->config().routes.at("asio-in:1") == "net-in:1");
  CHECK(f.client.Put("/api/routing", f.auth(), R"({"dst": "nope:1", "src": "net-in:1"})", "application/json")->status == 400);
  CHECK(f.client.Put("/api/routing", f.auth(), R"({"dst": "asio-in:1", "src": "nope"})", "application/json")->status == 400);
  CHECK(f.client.Put("/api/routing", f.auth(), R"({"src": "net-in:1"})", "application/json")->status == 400);
  auto unroute = f.client.Put("/api/routing", f.auth(), R"({"dst": "asio-in:1", "src": null})", "application/json");
  CHECK(unroute->status == 200);
  CHECK(f.engine->config().routes.count("asio-in:1") == 0);

  // ---- mixes ----
  auto mix = f.client.Put("/api/mixes", f.auth(),
                          json{{"dst", "asio-in:5"}, {"enabled", true},
                               {"sources", json::array({{{"source", "asio-out:1"}, {"gain_db", -6}, {"mute", false}},
                                                        {{"source", "asio-out:2"}, {"gain_db", 0}, {"mute", true}}})}}
                              .dump(),
                          "application/json");
  REQUIRE(mix);
  CHECK(mix->status == 200);
  auto mixj = json::parse(mix->body);
  CHECK(mixj["routing"]["mixes"]["asio-in:5"].size() == 2);
  CHECK(mixj["routing"]["mixes"]["asio-in:5"][0]["gain_db"] == -6);
  CHECK(f.engine->config().mixes.count("asio-in:5"));
  CHECK(f.client.Put("/api/mixes", f.auth(), R"({"dst": "asio-in:5", "enabled": true, "sources": [{"source": "x", "gain_db": 0}]})",
                     "application/json")->status == 400);
  auto off = f.client.Put("/api/mixes", f.auth(), R"({"dst": "asio-in:5", "enabled": false, "keep": "asio-out:1"})", "application/json");
  REQUIRE(off);
  CHECK(off->status == 200);
  CHECK(f.engine->config().routes.at("asio-in:5") == "asio-out:1");
  CHECK(json::parse(f.client.Get("/api/status", f.auth())->body).contains("mixer_late"));

  f.client.Put("/api/routing", f.auth(), R"({"dst": "asio-in:2", "src": "net-in:1"})", "application/json");
  auto del = f.client.Delete("/api/subscriptions/1", f.auth());
  REQUIRE(del);
  CHECK(del->status == 200);
  CHECK_FALSE(f.engine->config().subscriptions[0].enabled);
  CHECK(f.engine->config().routes.count("asio-in:2") == 0);
  CHECK(f.client.Delete("/api/subscriptions/40", f.auth())->status == 404);

  // ---- outputs for receivers ----
  auto made = f.client.Post("/api/outputs", f.auth(),
                            json{{"name", "Orban Warszawa"}, {"source", "asio-out:2"}, {"ptime_us", 1000}}.dump(), "application/json");
  REQUIRE(made);
  CHECK(made->status == 200);
  auto mj = json::parse(made->body);
  const int made_n = mj["created"].get<int>();
  CHECK(made_n >= 2);
  CHECK(mj["sdp"].get<std::string>().find("Orban Warszawa") != std::string::npos);
  CHECK(mj["routing"]["routes"]["net-out:" + std::to_string(made_n)] == "asio-out:2");
  CHECK(f.engine->config().outputs[made_n - 1].enabled);
  CHECK(f.client.Post("/api/outputs", f.auth(), R"({"name": "x", "address": "10.0.0.1"})", "application/json")->status == 400);
  CHECK(f.client.Post("/api/outputs", f.auth(), "[]", "application/json")->status == 400);
  auto unmade = f.client.Delete("/api/outputs/" + std::to_string(made_n), f.auth());
  REQUIRE(unmade);
  CHECK(unmade->status == 200);
  CHECK_FALSE(f.engine->config().outputs[made_n - 1].enabled);
  CHECK(json::parse(unmade->body)["routing"]["routes"].count("net-out:" + std::to_string(made_n)) == 0);
  CHECK(f.client.Delete("/api/outputs/40", f.auth())->status == 404);
  CHECK(f.client.Post("/api/outputs", R"({"name": "x"})", "application/json")->status == 401);

  auto status = f.client.Get("/api/status", f.auth());
  REQUIRE(status);
  auto sj = json::parse(status->body);
  CHECK(sj["outputs"].size() == 32);
  CHECK(sj["subscriptions"].size() == 32);
  CHECK(sj["ptp"]["role"] == "preferred");
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

TEST_CASE("HTTP API: a stale \"mixes\" in PUT /api/config leaves the live mixes alone; PUT /api/mixes checks types") {
  Fixture f;
  auto setup = f.client.Post("/api/setup", R"({"password":"tajnehaslo1"})", "application/json");
  REQUIRE(setup);
  REQUIRE(setup->status == 200);
  f.login(setup->get_header_value("Set-Cookie"));

  auto mix = f.client.Put("/api/mixes", f.auth(),
                          json{{"dst", "asio-in:5"}, {"enabled", true},
                               {"sources", json::array({{{"source", "asio-out:1"}, {"gain_db", -6}, {"mute", false}},
                                                        {{"source", "asio-out:2"}, {"gain_db", 0}, {"mute", true}}})}}
                              .dump(),
                          "application/json");
  REQUIRE(mix);
  REQUIRE(mix->status == 200);

  // a panel draft taken before the mute: other gains, nothing muted, plus a real change (a name)
  const json stale = {{"outputs", json::array({json::object(), {{"name", "Nowa nazwa"}}})},
                      {"mixes", {{"asio-in:5", {{"sources", json::array({{{"source", "asio-out:1"}, {"gain_db", 0}, {"mute", false}},
                                                                         {{"source", "asio-out:2"}, {"gain_db", -12}, {"mute", false}}})}}},
                                 {"asio-in:6", {{"sources", json::array({{{"source", "asio-out:3"}, {"gain_db", 0}, {"mute", false}}})}}}}}};
  auto put = f.client.Put("/api/config", f.auth(), stale.dump(), "application/json");
  REQUIRE(put);
  CHECK(put->status == 200);
  const auto cfg = f.engine->config();
  CHECK(cfg.outputs[1].name == "Nowa nazwa");
  CHECK(cfg.mixes.size() == 1);
  REQUIRE(cfg.mixes.count("asio-in:5"));
  const auto& m = cfg.mixes.at("asio-in:5").sources;
  REQUIRE(m.size() == 2);
  CHECK(m[0].gain_db == doctest::Approx(-6));
  CHECK_FALSE(m[0].mute);
  CHECK(m[1].gain_db == doctest::Approx(0));
  CHECK(m[1].mute);

  // gain_db / mute of the wrong type are rejected, not silently replaced by defaults
  auto bad_gain = f.client.Put("/api/mixes", f.auth(),
                               R"({"dst": "asio-in:5", "enabled": true, "sources": [{"source": "asio-out:1", "gain_db": "-6"}]})",
                               "application/json");
  REQUIRE(bad_gain);
  CHECK(bad_gain->status == 400);
  CHECK(json::parse(bad_gain->body)["error"].get<std::string>().find("gain_db") != std::string::npos);
  auto bad_mute = f.client.Put("/api/mixes", f.auth(),
                               R"({"dst": "asio-in:5", "enabled": true, "sources": [{"source": "asio-out:1", "mute": 1}]})",
                               "application/json");
  REQUIRE(bad_mute);
  CHECK(bad_mute->status == 400);
  CHECK(json::parse(bad_mute->body)["error"].get<std::string>().find("mute") != std::string::npos);
  CHECK(f.engine->config().mixes.at("asio-in:5").sources.size() == 2);  // unchanged by the rejected requests
}
