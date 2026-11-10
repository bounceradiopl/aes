// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder HTTP API (spec §7). Every server here listens on 127.0.0.1 on a free port; the
// engine runs without PTP on the loopback interface, and the only stream server is 127.0.0.1.
#ifdef BAM_WITH_CODECS
#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "bam/auth.hpp"
#include "bam/encoder/data_dir.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/encoder/http_server.hpp"
#include "bam/encoder/icecast.hpp"  // testing::base64
#include "bam/encoder/paths.hpp"
#include "bam/http/guard.hpp"
#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"
#include "bam/sdp.hpp"
#include "http_attack.hpp"
#include "test_util.hpp"
#ifdef BAM_FAKE_STEREOTOOL_PATH
#include "../encoder/src/stereotool_api.hpp"  // the parameter indices (the fake's values)
#include "fake_stereotool/fake_control.hpp"
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

namespace be = bam::encoder;
namespace bp = bam::platform;
using json = nlohmann::json;
using namespace http_attack;

namespace {

constexpr const char* kLoopback = "127.0.0.1";
constexpr const char* kPassword = "tajnehaslo1";

std::string unique_path(const char* tag) {
  static int n = 0;
  return std::string("bam_enc_") + tag + "_" + std::to_string(bp::current_pid()) + "_" + std::to_string(++n) + ".json";
}

bool exists(const std::string& path) { return std::ifstream(path).good(); }

std::string loopback_sdp(const std::string& name, const std::string& group, uint16_t port) {
  bam::SdpStream s;
  s.session_name = name;
  s.origin_address = kLoopback;
  s.multicast_address = group;
  s.ttl = 1;
  s.port = port;
  s.payload_type = 98;
  s.encoding = bam::Encoding::L24;
  s.sample_rate = 48000;
  s.channels = 2;
  s.ptime_ms = 1.0;
  s.frames_per_packet = 48;
  return bam::generate_sdp(s, 1, 1);
}

// A station as the panel sends it. Its stream goes to 127.0.0.1:1, where nothing listens.
json station_json(const std::string& name, bool enabled, const std::string& password, const std::string& mount = "/radio.mp3") {
  return {{"name", name},
          {"enabled", enabled},
          {"inputs", json::array({{{"enabled", true},
                                   {"sdp", loopback_sdp("Wejscie", "239.69.230.1", 15230)},
                                   {"gain_db", 0},
                                   {"delay_s", 0},
                                   {"latency_us", 20000}}})},
          {"streams", json::array({{{"enabled", true},
                                    {"codec", "mp3"},
                                    {"bitrate_kbps", 128},
                                    {"sample_rate", 44100},
                                    {"host", kLoopback},
                                    {"port", 1},
                                    {"mount", mount},
                                    {"user", "source"},
                                    {"password", password}}})}};
}

be::EncoderEngineOptions http_engine_options(const std::string& stereotool_dir, const std::string& crash_marker = "") {
  be::EncoderEngineOptions o;
  o.enable_ptp = false;
  o.aes67_require_ptp = false;  // tests send at once, on loopback only
  o.enable_sap = false;
  o.stereotool_dir = stereotool_dir;  // empty: no presets folder
  o.crash_marker_path = crash_marker;  // empty: no crash-loop guard
  return o;
}

struct Fixture {
  std::string config_path = unique_path("http");
  be::EncoderEngine engine;
  std::unique_ptr<be::EncoderHttpServer> server;
  std::unique_ptr<httplib::Client> client;
  httplib::Headers auth;

  explicit Fixture(const std::string& stereotool_dir = "", const std::string& crash_marker = "")
      : engine(http_engine_options(stereotool_dir, crash_marker)) {
    be::EncoderConfig cfg = be::default_encoder_config();
    cfg.interface_ip = kLoopback;  // tests never join groups on a real network
    cfg.http_bind = kLoopback;
    std::string err;
    REQUIRE(be::save_config_file(config_path, cfg, &err));
    REQUIRE_MESSAGE(engine.start(cfg, &err), err);
    server = std::make_unique<be::EncoderHttpServer>(engine, config_path);
    REQUIRE_MESSAGE(server->start(kLoopback, 0, &err), err);
    REQUIRE(server->port() != 0);
    client = std::make_unique<httplib::Client>(kLoopback, server->port());
    client->set_read_timeout(10, 0);
  }
  ~Fixture() {
    server->stop();
    engine.stop();
    std::remove(config_path.c_str());
    std::remove((config_path + ".tmp").c_str());
  }
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
  json get(const std::string& path, int expect = 200) {
    auto r = client->Get(path, auth);
    REQUIRE(r);
    CHECK_MESSAGE(r->status == expect, path << ": " << r->body);
    return json::parse(r->body);
  }
  httplib::Result put(const std::string& path, const json& body) { return client->Put(path, auth, body.dump(), "application/json"); }
  httplib::Result post(const std::string& path, const json& body) { return client->Post(path, auth, body.dump(), "application/json"); }
  json file() { return json::parse(read_file(config_path)); }
  std::string cookie() const { return auth.begin()->second; }
};

}  // namespace

TEST_CASE("encoder HTTP: login, sessions and 401 without a session") {
  Fixture f;

  auto health = f.client->Get("/api/health");
  REQUIRE(health);
  CHECK(health->status == 200);
  const json h = json::parse(health->body);
  CHECK(h["service"] == "ok");
  CHECK(h["product"] == "Bounce Encoder");
  CHECK(h["version"] == "1.1.0");

  const json state = json::parse(f.client->Get("/api/auth-state")->body);
  CHECK(state["password_set"] == false);
  CHECK(state["logged_in"] == false);
  CHECK(state["local"] == true);

  // everything else under /api/ needs a session
  for (const char* path : {"/api/config", "/api/status", "/api/codecs", "/api/logs", "/api/interfaces"}) {
    auto r = f.client->Get(path);
    REQUIRE(r);
    CHECK_MESSAGE(r->status == 401, path);
    CHECK(json::parse(r->body)["error"] == "wymagane logowanie");
  }
  CHECK(f.client->Put("/api/config", "{}", "application/json")->status == 401);
  CHECK(f.client->Post("/api/stations", "{}", "application/json")->status == 401);
  CHECK(f.client->Delete("/api/stations/s1")->status == 401);
  CHECK(f.client->Put("/api/stations/s1", "{}", "application/json")->status == 401);
  CHECK(f.client->Post("/api/password", R"({"current":"a","password":"bbbbbbbb"})", "application/json")->status == 401);
  CHECK(f.client->Post("/api/logout", "", "application/json")->status == 401);
  CHECK(f.client->Get("/api/live")->status == 401);
  CHECK(f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json")->status == 409);  // no password yet

  // first password: at least 8 characters, only once
  CHECK(f.client->Post("/api/setup", R"({"password":"krotkie"})", "application/json")->status == 400);
  auto setup = f.client->Post("/api/setup", json{{"password", kPassword}}.dump(), "application/json");
  REQUIRE(setup);
  CHECK(setup->status == 200);
  const std::string cookie = setup->get_header_value("Set-Cookie");
  CHECK(cookie.find("HttpOnly") != std::string::npos);
  CHECK(cookie.find("SameSite=Strict") != std::string::npos);
  // not the BounceAES Master cookie: both panels run on one computer (cookies ignore the port)
  CHECK(cookie.rfind("bounce_encoder_session=", 0) == 0);
  CHECK(f.client->Post("/api/setup", R"({"password":"innehaslo22"})", "application/json")->status == 409);
  f.take_cookie(setup);
  CHECK(f.get("/api/auth-state")["logged_in"] == true);
  f.get("/api/config");

  // the hash is in the file and nowhere in the API
  CHECK(bam::verify_password(kPassword, f.file()["password_hash"].get<std::string>()));
  CHECK_FALSE(f.get("/api/config").contains("password_hash"));

  // logout ends the session
  CHECK(f.client->Post("/api/logout", f.auth, "", "application/json")->status == 200);
  CHECK(f.client->Get("/api/config", f.auth)->status == 401);
  CHECK(f.client->Get("/api/config", httplib::Headers{{"Cookie", "bounce_encoder_session=zmyslony"}})->status == 401);

  // login
  auto bad = f.client->Post("/api/login", R"({"password":"zlehaslo123"})", "application/json");
  CHECK(bad->status == 401);
  CHECK(json::parse(bad->body)["error"] == "nieprawidłowe hasło");
  auto good = f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json");
  REQUIRE(good->status == 200);
  f.take_cookie(good);
  f.get("/api/status");

  // password change needs the current one
  CHECK(f.post("/api/password", {{"current", "zlehaslo123"}, {"password", "nowehaslo99"}})->status == 403);
  CHECK(f.post("/api/password", {{"current", kPassword}, {"password", "nowe"}})->status == 400);
  CHECK(f.post("/api/password", {{"current", kPassword}, {"password", "nowehaslo99"}})->status == 200);
  CHECK(f.client->Post("/api/login", json{{"password", "nowehaslo99"}}.dump(), "application/json")->status == 200);
  CHECK(bam::verify_password("nowehaslo99", f.file()["password_hash"].get<std::string>()));

  // the embedded panel is public
  auto index = f.client->Get("/");
  REQUIRE(index);
  CHECK(index->status == 200);
  CHECK(index->get_header_value("Content-Type").find("text/html") == 0);
  CHECK(index->body.find("Bounce Encoder") != std::string::npos);
  CHECK(f.client->Get("/brak.js")->status == 404);
}

// The panel (task 9) is a set of embedded files: each is served with its type and the panel CSP,
// everything index.html loads is embedded, and nothing in it runs inline (the CSP forbids that).
TEST_CASE("encoder HTTP: the embedded panel files") {
  Fixture f;
  auto expected_type = [](const std::string& path) -> std::string {
    const std::string ext = path.substr(path.rfind('.'));
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    return "";
  };
  std::set<std::string> embedded;
  for (size_t i = 0; i < be::kEncoderPanelAssetCount; ++i) {
    const be::EncoderPanelAsset& a = be::kEncoderPanelAssets[i];
    CAPTURE(a.path);
    embedded.insert(a.path);
    CHECK(std::string(a.mime) == expected_type(a.path));  // no file of an unknown type in encoder/panel/
    auto r = f.client->Get(a.path);
    REQUIRE(r);
    CHECK(r->status == 200);
    CHECK(r->get_header_value("Content-Type") == a.mime);
    CHECK(r->get_header_value("X-Content-Type-Options") == "nosniff");
    CHECK(r->get_header_value("Content-Security-Policy").find("default-src 'self'") == 0);
    CHECK(r->body.size() == a.size);
    CHECK(std::memcmp(r->body.data(), a.data, a.size) == 0);
  }
  for (const char* need : {"/index.html", "/app.js", "/logic.js", "/style.css", "/logo.svg", "/favicon.png", "/apple-touch-icon.png"})
    CHECK_MESSAGE(embedded.count(need) == 1, need);

  // the logo the login screen and the top bar show
  CHECK(json::parse(f.client->Get("/api/auth-state")->body)["logo"] == "logo.svg");

  // every file index.html refers to is embedded; scripts only from files, the helpers first
  auto index = f.client->Get("/");
  REQUIRE(index);
  const std::string html = index->body;
  CHECK(html.find("<title>Bounce Encoder</title>") != std::string::npos);
  CHECK(html.find("Encoder</span>") != std::string::npos);  // the brand caption
  const std::regex ref(R"re((?:src|href)="([^"#:]+)")re");
  int refs = 0;
  for (auto it = std::sregex_iterator(html.begin(), html.end(), ref); it != std::sregex_iterator(); ++it, ++refs)
    CHECK_MESSAGE(embedded.count("/" + (*it)[1].str()) == 1, (*it)[1].str());
  CHECK(refs >= 5);
  const std::regex script(R"(<script\b([^>]*)>)");
  std::vector<std::string> scripts;
  for (auto it = std::sregex_iterator(html.begin(), html.end(), script); it != std::sregex_iterator(); ++it) scripts.push_back((*it)[1].str());
  REQUIRE(scripts.size() == 2);
  CHECK(scripts[0] == R"( src="logic.js")");
  CHECK(scripts[1] == R"( src="app.js")");
  CHECK_FALSE(std::regex_search(html, std::regex(R"(\son[a-z]+\s*=)")));  // no inline event handlers
  CHECK(html.find("javascript:") == std::string::npos);
}

TEST_CASE("encoder HTTP: config get and put, passwords never leave the server") {
  Fixture f;
  f.setup_and_login();

  json cfg = f.get("/api/config");
  CHECK(cfg["version"] == 1);
  CHECK(cfg["stations"].empty());
  CHECK_FALSE(cfg.contains("password_hash"));

  // a station with a stream password; the station stays disabled (nothing to receive here)
  cfg["stations"] = json::array({station_json("Radio Test", false, "zrodlo123")});
  auto r = f.put("/api/config", cfg);
  REQUIRE(r);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  json out = json::parse(r->body);
  CHECK(out["ok"] == true);
  const json& st = out["config"]["stations"][0];
  CHECK(st["id"] == "s1");
  CHECK(st["inputs"][0]["name"] == "Wejscie");  // normalize_config named it after the SDP
  CHECK(st["streams"][0]["password"] == "");
  CHECK(st["streams"][0]["password_set"] == true);
  CHECK(r->body.find("zrodlo123") == std::string::npos);
  CHECK(f.get("/api/config").dump().find("zrodlo123") == std::string::npos);
  CHECK(f.get("/api/config")["stations"][0]["streams"][0]["password_set"] == true);
  // saved to the file, with the password
  CHECK(f.file()["stations"][0]["streams"][0]["password"] == "zrodlo123");
  CHECK(f.engine.config().stations.at(0).streams.at(0).password == "zrodlo123");

  // the config as the panel got it (password "") goes back unchanged: the password is kept
  cfg = f.get("/api/config");
  cfg["stations"][0]["name"] = "Radio Nowe";
  r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(f.file()["stations"][0]["name"] == "Radio Nowe");
  CHECK(f.file()["stations"][0]["streams"][0]["password"] == "zrodlo123");

  // ruling E7: another server without a new password is refused, and the message says why
  json moved = f.get("/api/config");
  moved["stations"][0]["streams"][0]["host"] = "localhost";
  r = f.put("/api/config", moved);
  REQUIRE(r);
  CHECK(r->status == 400);
  out = json::parse(r->body);
  CHECK(out["ok"] == false);
  REQUIRE(!out["issues"].empty());
  CHECK(out["issues"][0]["field"] == "stations[0].streams[0].password");
  CHECK(out["issues"][0]["message"].get<std::string>().find("hasło") != std::string::npos);
  CHECK(out["issues"][0]["message"].get<std::string>().find("po zmianie serwera") != std::string::npos);
  CHECK(out["error"] == out["issues"][0]["message"]);
  CHECK(f.file()["stations"][0]["streams"][0]["host"] == kLoopback);  // nothing changed
  CHECK(f.engine.config().stations.at(0).streams.at(0).host == kLoopback);

  // an invalid combination: 400 with the Polish issue, nothing changes
  json bad = f.get("/api/config");
  bad["stations"][0]["streams"][0]["bitrate_kbps"] = 999;
  r = f.put("/api/config", bad);
  CHECK(r->status == 400);
  out = json::parse(r->body);
  CHECK(out["issues"][0]["field"] == "stations[0].streams[0]");  // the encoder settings as a whole
  CHECK(out["issues"][0]["message"].get<std::string>().find("bitrate") != std::string::npos);
  CHECK(f.file()["stations"][0]["streams"][0]["bitrate_kbps"] == 128);

  // a body that is not a configuration
  r = f.client->Put("/api/config", f.auth, "{nie json", "application/json");
  CHECK(r->status == 400);
  CHECK(json::parse(r->body)["error"] == "nieprawidłowy JSON w treści żądania");
  r = f.put("/api/config", json::array());
  CHECK(r->status == 400);
  r = f.put("/api/config", {{"stations", "nie lista"}});
  CHECK(r->status == 400);
  CHECK(json::parse(r->body)["error"].get<std::string>().find("stations") != std::string::npos);
  r = f.put("/api/config", {{"version", 2}});
  CHECK(r->status == 400);

  // top-level keys that are left out keep their value; a password_hash in the body is ignored
  r = f.put("/api/config", {{"ptp_domain", 5}, {"password_hash", "podrobiony"}});
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(f.file()["ptp_domain"] == 5);
  CHECK(f.file()["stations"].size() == 1);
  CHECK(f.file()["stations"][0]["streams"][0]["password"] == "zrodlo123");
  CHECK(bam::verify_password(kPassword, f.file()["password_hash"].get<std::string>()));

  // a panel address change takes effect after a restart: said in the notices
  r = f.put("/api/config", {{"http_port", 8071}});
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["notices"].dump().find("ponownym uruchomieniu") != std::string::npos);
  CHECK(f.file()["http_port"] == 8071);

  // a body over 1 MB is refused on its headers and never read: the server answers 413 at once, so a
  // client still sending may see the connection reset instead of the answer
  const std::string huge(be::kMaxApiBodyBytes + 1, ' ');
  r = f.client->Put("/api/config", f.auth, huge, "application/json");
  if (r) {
    CHECK(r->status == 413);
    CHECK(json::parse(r->body)["error"].get<std::string>().find("za duża") != std::string::npos);
  }
  CHECK(raw_status(f.server->port(), "PUT /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nCookie: " +
                                         f.cookie() + "\r\nContent-Length: " + std::to_string(be::kMaxApiBodyBytes + 1) + "\r\n\r\n") == 413);
  CHECK(f.file()["http_port"] == 8071);
  f.get("/api/config");  // still answering
}

TEST_CASE("encoder HTTP: stations are added, edited and deleted") {
  Fixture f;
  f.setup_and_login();

  auto r = f.post("/api/stations", station_json("Pierwsza", false, "haslo-a", "/a.mp3"));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  json out = json::parse(r->body);
  CHECK(out["ok"] == true);
  CHECK(out["id"] == "s1");
  CHECK(out["config"]["stations"].size() == 1);

  // an id in the body is not taken over: a new station always gets a free one
  json second = station_json("Druga", false, "haslo-b", "/b.mp3");
  second["id"] = "s1";
  r = f.post("/api/stations", second);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["id"] == "s2");
  CHECK(f.file()["stations"].size() == 2);
  CHECK(json::parse(r->body).dump().find("haslo-b") == std::string::npos);

  // a new station without a stream password is refused (an enabled stream needs one)
  r = f.post("/api/stations", station_json("Trzecia", false, "", "/c.mp3"));
  CHECK(r->status == 400);
  CHECK(json::parse(r->body)["issues"][0]["field"] == "stations[2].streams[0].password");
  CHECK(f.file()["stations"].size() == 2);
  CHECK(f.post("/api/stations", json::array())->status == 400);

  // editing one station: its passwords are kept, the id from the path wins
  json edit = f.get("/api/config")["stations"][1];
  edit["name"] = "Druga po zmianie";
  edit["id"] = "inne";
  r = f.put("/api/stations/s2", edit);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(f.file()["stations"][1]["id"] == "s2");
  CHECK(f.file()["stations"][1]["name"] == "Druga po zmianie");
  CHECK(f.file()["stations"][1]["streams"][0]["password"] == "haslo-b");
  CHECK(f.put("/api/stations/s9", edit)->status == 404);

  // deleting the first station keeps the second one's id and password
  r = f.client->Delete("/api/stations/s1", f.auth);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  const json file = f.file();
  REQUIRE(file["stations"].size() == 1);
  CHECK(file["stations"][0]["id"] == "s2");
  CHECK(file["stations"][0]["streams"][0]["password"] == "haslo-b");
  CHECK(f.engine.config().stations.size() == 1);
  CHECK(f.client->Delete("/api/stations/s1", f.auth)->status == 404);

  // one update deletes a station and adds one: the new one never gets the deleted one's id (ruling S4)
  json whole = f.get("/api/config");
  REQUIRE(whole["stations"].size() == 1);
  whole["stations"] = json::array({station_json("Zastepcza", false, "haslo-z", "/z.mp3")});
  r = f.put("/api/config", whole);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["config"]["stations"][0]["id"] == "s3");
  CHECK_FALSE(json::parse(r->body)["config"].contains("next_station_number"));  // bookkeeping of the file only
  CHECK(f.file()["next_station_number"] == 4);
  r = f.client->Delete("/api/stations/s3", f.auth);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  r = f.post("/api/stations", station_json("Kolejna", false, "haslo-k", "/k.mp3"));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["id"] == "s4");

  // the 16-station limit
  for (int i = 0; i < 15; ++i) {
    r = f.post("/api/stations", {{"name", "Stacja " + std::to_string(i)}, {"enabled", false}});
    REQUIRE_MESSAGE(r->status == 200, r->body);
  }
  r = f.post("/api/stations", {{"name", "Za dużo"}, {"enabled", false}});
  CHECK(r->status == 400);
  CHECK(json::parse(r->body)["error"].get<std::string>().find("16") != std::string::npos);
  CHECK(f.file()["stations"].size() == 16);
}

TEST_CASE("encoder HTTP: codecs endpoint (availability, allowed settings, bitrate hints)") {
  Fixture f;
  f.setup_and_login();
  const json c = f.get("/api/codecs");

  CHECK(c["rates"] == json({48000, 44100, 32000, 24000, 22050}));
  REQUIRE(c["codecs"].size() == 5);
  auto codec = [&](const std::string& id) -> json {
    for (const auto& x : c["codecs"])
      if (x["id"] == id) return x;
    FAIL("no codec " << id);
    return {};
  };
  auto rate = [&](const json& codec_json, uint32_t hz) -> json {
    for (const auto& x : codec_json["rates"])
      if (x["sample_rate"] == hz) return x;
    return nullptr;
  };

  for (be::Codec k : be::kCodecs) {
    const json x = codec(be::codec_name(k));
    CHECK(x["available"] == be::codec_available(k));
    CHECK(x["content_type"] == be::codec_content_type(k));
    if (x["available"] == true) {
      CHECK(x["reason"] == "");
    } else {
      CHECK(x["reason"].get<std::string>().find("niedostępny") != std::string::npos);
    }
  }

  const json mp3 = codec("mp3");
  CHECK(mp3["label"] == "MP3");
  CHECK(mp3["lossless"] == false);
  CHECK(mp3["min_kbps"] == 32);
  CHECK(mp3["max_kbps"] == 320);
  // MPEG-1 steps at 44.1 kHz, MPEG-2 steps (at most 160) at 22.05 / 24 kHz
  const json mp3_44 = rate(mp3, 44100);
  CHECK(mp3_44["min_kbps"] == 32);
  CHECK(mp3_44["max_kbps"] == 320);
  CHECK(mp3_44["steps"] == json({32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}));
  const json mp3_22 = rate(mp3, 22050);
  CHECK(mp3_22["max_kbps"] == 160);
  CHECK(mp3_22["steps"].back() == 160);
  for (const auto& s : mp3_22["steps"]) {
    std::string err;
    CHECK(be::validate_encoder_settings({be::Codec::Mp3, s.get<uint32_t>(), 22050, 2}, &err));
  }

  // AAC-LC: any value in the range, at most 12 bits per sample period
  const json aac = codec("aac");
  CHECK(aac["label"] == "AAC-LC");
  CHECK(rate(aac, 48000)["max_kbps"] == 320);
  CHECK(rate(aac, 48000)["steps"].is_null());
  CHECK(rate(aac, 24000)["max_kbps"] == 288);
  CHECK(rate(aac, 22050)["max_kbps"] == 264);

  // HE-AAC v1 / v2: three rates only
  const json he = codec("he-aac");
  CHECK(he["rates"].size() == 3);
  CHECK(rate(he, 24000).is_null());
  CHECK(rate(he, 32000)["min_kbps"] == 24);
  CHECK(rate(he, 32000)["max_kbps"] == 128);
  const json he2 = codec("he-aac-v2");
  CHECK(rate(he2, 48000)["min_kbps"] == 16);
  CHECK(rate(he2, 48000)["max_kbps"] == 64);

  const json flac = codec("flac");
  CHECK(flac["lossless"] == true);
  CHECK(flac["available"] == true);
  CHECK(flac["rates"].size() == 5);

  // spec §8: the MP3 equivalents and the codec recommendation
  const json& eq = c["mp3_equivalents"];
  REQUIRE(eq.size() == 5);
  CHECK(eq[0]["codec"] == "he-aac-v2");
  CHECK(eq[0]["min_kbps"] == 32);
  CHECK(eq[0]["mp3"] == "~96 kb/s");
  CHECK(eq[4]["codec"] == "aac");
  CHECK(eq[4]["min_kbps"] == 192);
  CHECK(eq[4]["max_kbps"].is_null());
  CHECK(eq[4]["mp3"] == "~256–320 kb/s");
  const json& rec = c["recommendations"];
  REQUIRE(rec.size() == 3);
  CHECK(rec[0]["codec"] == "he-aac-v2");
  CHECK(rec[0]["max_kbps"] == 48);
  CHECK(rec[2]["codec"] == "aac");
  CHECK(rec[2]["min_kbps"] == 96);
  CHECK(c["recommendation_text"] == "HE-AACv2 do 48 kb/s, HE-AAC 48–96 kb/s, AAC-LC powyżej 96 kb/s");
}

TEST_CASE("encoder HTTP: status of stations, inputs, streams and PTP") {
  Fixture f;
  f.setup_and_login();
  auto r = f.post("/api/stations", station_json("Radio Status", true, "zrodlo123"));
  REQUIRE_MESSAGE(r->status == 200, r->body);

  json s;
  // nothing listens on 127.0.0.1:1: the stream reports the Icecast error
  REQUIRE(wait_for(
      [&] {
        s = f.get("/api/status");
        return s["stations"].size() == 1 && s["stations"][0]["streams"].size() == 1 &&
               s["stations"][0]["streams"][0]["state"] == "error";
      },
      10000));
  CHECK(s["running"] == true);
  CHECK(s["version"] == "1.1.0");
  CHECK(s["ptp"]["enabled"] == false);
  CHECK(s["ptp"]["state"] == "disabled");
  CHECK(s["ptp"]["free_run"] == true);
  const json& st = s["stations"][0];
  CHECK(st["id"] == "s1");
  CHECK(st["name"] == "Radio Status");
  CHECK(st["running"] == true);
  CHECK(st["peak_db"].size() == 2);
  const json& in = st["inputs"][0];
  CHECK(in["address"] == "239.69.230.1:15230");
  CHECK(in["signal"] == false);
  CHECK(in["rx"].contains("packets"));
  CHECK(in["peak_db"].size() == 2);
  const json& str = st["streams"][0];
  CHECK(str["id"] == "str1");
  CHECK(str["codec"] == "mp3");
  CHECK(str["mount"] == "/radio.mp3");
  CHECK(str["error"] == "serwer nieosiągalny");
  CHECK(str.contains("bytes_sent"));
  CHECK(str.contains("kbps"));
  CHECK(str.contains("reconnects"));
  CHECK(str.contains("dropped_bytes"));
  CHECK(str.contains("queue_dropped_frames"));
  CHECK(str["connected_s"] == 0);  // never connected (the panel's "Połączony" column)
  CHECK(str["encoder_failed"] == !be::codec_available(be::Codec::Mp3));
  if (be::codec_available(be::Codec::Mp3)) {
    CHECK(str["codec_library"] == be::codec_library_path(be::Codec::Mp3));
  } else {
    CHECK(str["codec_library"] == "niedostępny");
  }
  CHECK(s.dump().find("zrodlo123") == std::string::npos);

  // the public health summary the tray polls
  const json h = json::parse(f.client->Get("/api/health")->body);
  CHECK(h["streams_enabled"] == 1);
  CHECK(h["streams_streaming"] == 0);
  CHECK(h["ptp"] == "disabled");

  // the live feed: the status document as server-sent events
  std::string live;
  f.client->Get("/api/live", f.auth, [&](const char* data, size_t len) {
    live.append(data, len);
    return live.find("\n\n") == std::string::npos;  // one event is enough
  });
  REQUIRE(live.rfind("event: status\ndata: ", 0) == 0);
  const json ev = json::parse(live.substr(20, live.find("\n\n") - 20));
  CHECK(ev["stations"][0]["id"] == "s1");
  CHECK(live.find("zrodlo123") == std::string::npos);
  CHECK(f.client->Get("/api/live")->status == 401);

  // logs and network interfaces for the panel
  CHECK(f.get("/api/logs").is_array());
  CHECK(f.get("/api/interfaces").is_array());
}

TEST_CASE("encoder service: startup configuration") {
  SUBCASE("no file: the defaults are written") {
    const std::string path = unique_path("start");
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(c.http_port == 8070);
    CHECK(c.http_bind == "0.0.0.0");  // the product default; tests bind 127.0.0.1 themselves
    CHECK(exists(path));
    std::remove(path.c_str());
  }
  SUBCASE("unreadable file: kept as .bad, the defaults used") {
    const std::string path = unique_path("start");
    std::ofstream(path) << "{ to nie jest konfiguracja";
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(c.stations.empty());
    CHECK(read_file(path + ".bad") == "{ to nie jest konfiguracja");
    CHECK(be::config_from_json(read_file(path), nullptr));
    std::remove(path.c_str());
    std::remove((path + ".bad").c_str());
  }
  SUBCASE("errors: the bad station is disabled, a bad global setting goes back to its default") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["http_port"] = 70000;
    j["stations"] = json::array({station_json("Dobra", true, "haslo1", "/a.mp3"), station_json("Zla", true, "haslo2", "/b.mp3")});
    j["stations"][1]["streams"][0]["bitrate_kbps"] = 999;
    j["stations"][1]["enabled"] = true;
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    CHECK(c.http_port == 8070);
    REQUIRE(c.stations.size() == 2);
    CHECK(c.stations[0].enabled);
    CHECK_FALSE(c.stations[1].enabled);
    CHECK_FALSE(c.stations[1].streams[0].enabled);
    CHECK(c.stations[1].streams[0].bitrate_kbps == 999);  // kept for the user to fix
    CHECK(c.stations[1].inputs[0].enabled);
    CHECK(json::parse(read_file(path))["http_port"] == 70000);  // the file is not rewritten
    std::remove(path.c_str());
  }
  SUBCASE("an invalid panel address fails closed: 127.0.0.1, not every interface") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["http_bind"] = "127.0.0.l";  // a typo for a local-only address
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    CHECK(c.http_bind == "127.0.0.1");
    std::remove(path.c_str());
  }
  SUBCASE("a station still invalid while disabled is left out, the file kept as .bad") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["stations"] = json::array({station_json("Dobra", true, "haslo1", "/a.mp3"), station_json("Zla", true, "haslo2", "/b.mp3")});
    j["stations"][1]["inputs"][0]["gain_db"] = 40;  // a range error: checked on disabled stations too
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    REQUIRE(c.stations.size() == 1);
    CHECK(c.stations[0].name == "Dobra");
    CHECK(exists(path + ".bad"));
    CHECK(json::parse(read_file(path))["stations"].size() == 2);
    std::remove(path.c_str());
    std::remove((path + ".bad").c_str());
  }
  SUBCASE("a bad processing block: the station runs without processing, the bad value is dropped, no key logged") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["stereotool_default_key"] = "SEKRETD-bez-nawiasow";  // a bad global key: dropped (as a default)
    j["stereotool_web_port_base"] = 8065;                  // 8065–8080 holds the panel port 8070
    j["stations"] = json::array({station_json("Jeden", true, "haslo1", "/a.mp3"), station_json("Dwa", true, "haslo2", "/b.mp3"),
                                 station_json("Trzy", true, "haslo3", "/c.mp3")});
    j["stations"][0]["processing"] = {{"enabled", true}, {"license_key", "SEKRETA-bez-nawiasow"}, {"preset", "../zly.sts"}};
    j["stations"][1]["processing"] = {{"enabled", true}, {"license_key", "<SEKRETB-dobry>"}, {"preset", ""}};
    j["stations"][2]["processing"] = {{"enabled", true}, {"license_key", ""}, {"preset", "FM.sts"}};  // needed the default key
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    CHECK(c.stereotool_default_key.empty());
    CHECK(c.stereotool_web_port_base == 8100);
    REQUIRE(c.stations.size() == 3);
    for (const auto& st : c.stations) {
      CHECK_MESSAGE(st.enabled, st.name);  // never silence because of processing
      CHECK_FALSE(st.processing.enabled);
      CHECK(st.streams[0].enabled);
    }
    CHECK(c.stations[0].processing.license_key.empty());
    CHECK(c.stations[0].processing.preset.empty());
    CHECK(c.stations[1].processing.license_key == "<SEKRETB-dobry>");  // valid: kept
    CHECK(c.stations[2].processing.preset == "FM.sts");
    // a dropped key is not lost: the file is kept as .bad (admin-only folder, like config.json)
    REQUIRE(exists(path + ".bad"));
    CHECK(read_file(path + ".bad") == read_file(path));
    CHECK(read_file(path + ".bad").find("SEKRETA-bez-nawiasow") != std::string::npos);
    bool told = false, told_station_key = false, told_default_key = false;
    for (const auto& e : bam::Logger::instance().recent()) {
      CHECK_MESSAGE(e.message.find("SEKRET") == std::string::npos, e.message);
      told |= e.message.find("„Trzy”") != std::string::npos && e.message.find("processing") != std::string::npos;
      told_station_key |= e.message.find("Klucz licencji Stereo Tool stacji „Jeden” usunięty z konfiguracji (kopia pliku: " +
                                         path + ".bad)") != std::string::npos;
      told_default_key |=
          e.message.find("Domyślny klucz Stereo Tool usunięty z konfiguracji (kopia pliku: " + path + ".bad)") != std::string::npos;
    }
    CHECK(told);
    CHECK(told_station_key);
    CHECK(told_default_key);
    std::remove(path.c_str());
    std::remove((path + ".bad").c_str());
  }
  SUBCASE("only a bad preset name: processing off, no copy of the file") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["stations"] = json::array({station_json("Preset", true, "haslo1", "/a.mp3")});
    j["stations"][0]["processing"] = {{"enabled", true}, {"license_key", "<SEKRETC-dobry>"}, {"preset", "zly/preset.sts"}};
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    REQUIRE(c.stations.size() == 1);
    CHECK(c.stations[0].enabled);
    CHECK_FALSE(c.stations[0].processing.enabled);
    CHECK(c.stations[0].processing.preset.empty());
    CHECK(c.stations[0].processing.license_key == "<SEKRETC-dobry>");
    CHECK_FALSE(exists(path + ".bad"));
    std::remove(path.c_str());
  }
  SUBCASE("two stations on one Stereo Tool key: one warning logged, naming both, without the key") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["stations"] = json::array({station_json("Klucz Pierwsza", true, "haslo1", "/a.mp3"),
                                 station_json("Klucz Druga", true, "haslo2", "/b.mp3")});
    for (auto& st : j["stations"]) st["processing"] = {{"enabled", true}, {"license_key", "<SEKRETW-wspolny>"}, {"preset", "FM.sts"}};
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    REQUIRE(c.stations.size() == 2);
    CHECK(c.stations[0].processing.enabled);  // a warning only: both keep processing
    CHECK(c.stations[1].processing.enabled);
    int warnings = 0;
    for (const auto& e : bam::Logger::instance().recent()) {
      CHECK_MESSAGE(e.message.find("SEKRET") == std::string::npos, e.message);
      if (e.message.find("„Klucz Druga”") != std::string::npos && e.message.find("„Klucz Pierwsza”") != std::string::npos &&
          e.message.find("ten sam klucz licencji Stereo Tool") != std::string::npos) {
        ++warnings;
        CHECK(e.level == bam::LogLevel::Warning);
      }
    }
    CHECK(warnings == 1);
    CHECK_FALSE(exists(path + ".bad"));
    std::remove(path.c_str());
  }
  SUBCASE("a bad web port base next to a panel on 8100: a base that does not hold the panel port") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["http_port"] = 8105;
    j["stereotool_web_port_base"] = 80;
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    CHECK(c.http_port == 8105);
    CHECK(c.stereotool_web_port_base == 8200);
    std::remove(path.c_str());
  }
  SUBCASE("duplicate or out-of-range web port slots: a free one, processing stays on, the others keep theirs") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j["stations"] = json::array({station_json("Pierwsza", true, "haslo1", "/a.mp3"), station_json("Druga", true, "haslo2", "/b.mp3"),
                                 station_json("Trzecia", true, "haslo3", "/c.mp3")});
    j["stations"][0]["processing"] = {{"enabled", true}, {"preset", "FM.sts"}, {"license_key", "<SLOT-TEST-KEY>"}, {"web_port_slot", 5}};
    j["stations"][1]["processing"] = {{"enabled", true}, {"preset", "FM.sts"}, {"license_key", "<SLOT-TEST-KEY2>"}, {"web_port_slot", 5}};
    j["stations"][2]["processing"] = {{"web_port_slot", 40}};
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    REQUIRE(c.stations.size() == 3);
    CHECK(c.stations[0].processing.web_port_slot == 5);  // the first holder keeps it
    CHECK(c.stations[1].processing.web_port_slot == 0);  // the lowest free ones
    CHECK(c.stations[2].processing.web_port_slot == 1);
    CHECK(c.stations[1].processing.enabled);  // never a reason to switch processing off
    CHECK(c.stations[1].enabled);
    CHECK_FALSE(exists(path + ".bad"));
    bool logged = false;
    for (const auto& e : bam::Logger::instance().recent())
      logged = logged || e.message.find("Stacja „Druga”: nowy port interfejsu WWW Stereo Tool 8100") != std::string::npos;
    CHECK(logged);
    std::remove(path.c_str());
  }
  SUBCASE("ids that differ only in case (a 1.0 file): the later station gets a new id, nothing is disabled") {
    const std::string path = unique_path("start");
    json j = json::parse(be::config_to_json(be::default_encoder_config()));
    j.erase("next_station_number");  // as Bounce Encoder 1.0 wrote it
    j["stations"] = json::array({station_json("Mala", true, "haslo1", "/a.mp3"), station_json("Wielka", true, "haslo2", "/b.mp3"),
                                 station_json("Trzecia", true, "haslo3", "/c.mp3")});
    j["stations"][0]["id"] = "s1";
    j["stations"][1]["id"] = "S1";
    j["stations"][2]["id"] = "s2";
    std::ofstream(path) << j.dump();
    const be::EncoderConfig c = be::load_startup_config(path);
    CHECK(be::validate_config(c).empty());
    REQUIRE(c.stations.size() == 3);
    CHECK(c.stations[0].id == "s1");
    CHECK(c.stations[1].id == "s3");  // a number never given in this file
    CHECK(c.stations[2].id == "s2");
    for (const auto& st : c.stations) CHECK_MESSAGE(st.enabled, st.name);
    CHECK(c.stations[1].streams[0].password == "haslo2");  // the station as it was, under its new id
    CHECK_FALSE(exists(path + ".bad"));
    bool logged = false;
    for (const auto& e : bam::Logger::instance().recent())
      logged = logged || e.message.find("Stacja „Wielka”: identyfikator „S1” różni się od „s1” tylko wielkością liter; nowy identyfikator „s3”") !=
                             std::string::npos;
    CHECK(logged);
    std::remove(path.c_str());
  }
}

TEST_CASE("encoder HTTP: bodies without a length, chunked or too large are refused before they are read") {
  Fixture f;
  const uint16_t port = f.server->port();

  // public endpoint, chunked: refused, the handler never ran
  CHECK(status_or_reset(raw_status(port, chunked_request("POST", "/api/setup", json{{"password", kPassword}}.dump())), 411));
  CHECK(json::parse(f.client->Get("/api/auth-state")->body)["password_set"] == false);
  // no Content-Length at all (the body would run until the connection closes)
  CHECK(status_or_reset(raw_status(port, "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n\r\n"
                                         "{\"password\":\"zlehaslo123\"}"),
                        411));
  // a 3 MB chunk announced: refused on the headers
  CHECK(status_or_reset(raw_status(port, "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                         "Transfer-Encoding: chunked\r\n\r\n300000\r\n" + std::string(65536, 'a')),
                        411));

  // with a session the same
  f.setup_and_login();
  CHECK(status_or_reset(raw_status(port, chunked_request("PUT", "/api/config", R"({"ptp_domain":7})", f.cookie())), 411));
  CHECK(f.file()["ptp_domain"] == 0);
  // a Content-Length over 1 MB: 413 at once, without waiting for (or skipping) the body
  CHECK(raw_status(port, "PUT /api/config HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nCookie: " +
                             f.cookie() + "\r\nContent-Length: 2000000000\r\n\r\n", 2000) == 413);
  // the server still answers normally
  f.get("/api/config");
}

TEST_CASE("encoder HTTP: a body must be JSON (no cross-origin simple requests)") {
  Fixture f;
  // a form or text/plain POST is what another web page can send without CORS: refused
  auto r = f.client->Post("/api/setup", json{{"password", kPassword}}.dump(), "text/plain");
  REQUIRE(r);
  CHECK(r->status == 415);
  CHECK(json::parse(r->body)["error"].get<std::string>().find("application/json") != std::string::npos);
  CHECK(f.client->Post("/api/setup", "password=tajnehaslo1", "application/x-www-form-urlencoded")->status == 415);
  CHECK(json::parse(f.client->Get("/api/auth-state")->body)["password_set"] == false);
  CHECK(f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "text/plain")->status == 415);
  // a bounded body is read before the answer, so the client always gets the whole answer (with
  // bytes left unread at the close, Windows would reset the connection and lose it)
  auto big = f.client->Post("/api/setup", std::string(200000, 'x'), "text/plain");
  REQUIRE(big);
  CHECK(big->status == 415);
  CHECK(json::parse(big->body)["error"].get<std::string>().find("application/json") != std::string::npos);

  f.setup_and_login();
  CHECK(f.client->Put("/api/config", f.auth, R"({"ptp_domain":3})", "text/plain")->status == 415);
  CHECK(f.file()["ptp_domain"] == 0);
  CHECK(f.client->Put("/api/config", f.auth, R"({"ptp_domain":3})", "application/json; charset=utf-8")->status == 200);
  // a POST without a body needs no type
  CHECK(f.client->Post("/api/logout", f.auth)->status == 200);
}

TEST_CASE("encoder HTTP: the first password only through a loopback Host (no DNS rebinding)") {
  // A web page in a browser on the encoder computer can rebind its own domain to 127.0.0.1: its
  // requests then come from 127.0.0.1 and are same-origin, but carry the page's Host.
  const std::string body = json{{"password", kPassword}}.dump();
  {
    Fixture f;
    for (const char* host : {"evil.example", "evil.example:8070", "127.0.0.1.evil.example", "localhost.evil.example",
                             "192.168.1.10:8070", "[::2]:8070", ""}) {
      auto r = f.client->Post("/api/setup", {{"Host", host}}, body, "application/json");
      REQUIRE(r);
      CHECK_MESSAGE(r->status == 403, host);
      CHECK(json::parse(r->body)["error"].get<std::string>().find("127.0.0.1") != std::string::npos);
      // the panel shows the "open http://127.0.0.1:port" hint instead of the form
      CHECK(json::parse(f.client->Get("/api/auth-state", {{"Host", host}})->body)["local"] == false);
    }
    // HTTP/1.0 without any Host
    CHECK(raw_status(f.server->port(), "POST /api/setup HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: " +
                                           std::to_string(body.size()) + "\r\n\r\n" + body) == 403);
    CHECK(json::parse(f.client->Get("/api/auth-state")->body)["password_set"] == false);
    const json saved = f.file();
    CHECK(saved.value("password_hash", std::string()).empty());
  }
  const auto port_of = [](const Fixture& f) { return std::to_string(f.server->port()); };
  for (const std::string host : {"127.0.0.1:%", "localhost:%", "LocalHost:%", "127.0.0.1", "localhost", "[::1]:%", "[::1]"}) {
    Fixture f;
    std::string h = host;
    if (const size_t pct = h.find('%'); pct != std::string::npos) h.replace(pct, 1, port_of(f));
    CHECK(json::parse(f.client->Get("/api/auth-state", {{"Host", h}})->body)["local"] == true);
    auto r = f.client->Post("/api/setup", {{"Host", h}}, body, "application/json");
    REQUIRE(r);
    CHECK_MESSAGE(r->status == 200, h);
  }
}

TEST_CASE("encoder HTTP: a large form body is refused as the wrong type, not as over 1 MB") {
  Fixture f;
  // httplib refuses urlencoded bodies over 8 KB itself (413) before any handler runs
  auto r = f.client->Post("/api/setup", "password=" + std::string(9000, 'x'), "application/x-www-form-urlencoded");
  if (!r && kResetMayLoseAnswer) return;  // the refused body is unread at the close (see status_or_reset)
  REQUIRE(r);
  CHECK(r->status == 413);
  const std::string error = json::parse(r->body)["error"].get<std::string>();
  CHECK(error.find("application/json") != std::string::npos);
  CHECK(error.find("1 MB") == std::string::npos);
}

TEST_CASE("encoder HTTP: failed logins are throttled without holding a request thread") {
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
  // a session already open is not affected
  f.get("/api/config");
}

TEST_CASE("encoder HTTP: concurrent failed logins cannot outrun the throttle") {
  Fixture f;
  f.setup_and_login();
  check_login_burst(concurrent_wrong_logins(f.server->port(), 24), 24);
  CHECK(f.client->Post("/api/login", json{{"password", kPassword}}.dump(), "application/json")->status == 429);
  f.get("/api/config");
}

TEST_CASE("encoder HTTP: only one first password, even when set concurrently") {
  Fixture f;
  std::atomic<int> ok{0}, conflict{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {  // within httplib's listen backlog of 5
    threads.emplace_back([&, i] {
      httplib::Client c(kLoopback, f.server->port());
      c.set_read_timeout(10, 0);
      auto r = c.Post("/api/setup", json{{"password", "haslo-nr-" + std::to_string(i)}}.dump(), "application/json");
      if (r && r->status == 200) ++ok;
      if (r && r->status == 409) ++conflict;
      if (!r || (r->status != 200 && r->status != 409)) MESSAGE("setup " << i << ": " << (r ? std::to_string(r->status) + " " + r->body : httplib::to_string(r.error())));
    });
  }
  for (auto& t : threads) t.join();
  CHECK(ok == 1);
  CHECK(conflict == 3);
}

TEST_CASE("encoder HTTP: the live feed ends with its session; a password change ends the other sessions") {
  Fixture f;
  f.setup_and_login();

  // reads /api/live with `cookie` until the server ends it (true) or 5 s pass (false)
  auto watch = [&](const std::string& cookie, std::atomic<int>& events, std::atomic<bool>& ended) {
    httplib::Client c(kLoopback, f.server->port());
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
  REQUIRE(wait_for([&] { return events > 0; }, 3000));
  auto change = f.post("/api/password", {{"current", kPassword}, {"password", "nowehaslo99"}});
  REQUIRE(change->status == 200);
  t.join();
  CHECK(ended);
  CHECK(f.client->Get("/api/config", httplib::Headers{{"Cookie", other}})->status == 401);
  // the session that changed the password goes on (with the cookie it was given)
  if (!change->get_header_value("Set-Cookie").empty()) f.take_cookie(change);
  f.get("/api/config");
}

TEST_CASE("encoder HTTP: a start that failed to bind can be retried") {
  be::EncoderEngine engine{[] {
    be::EncoderEngineOptions o;
    o.enable_ptp = false;
    return o;
  }()};
  std::string err;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  REQUIRE(engine.start(cfg, &err));
  auto taken = bp::TcpListener::listen(kLoopback, 0, &err);
  REQUIRE(taken);
  be::EncoderHttpServer server(engine, "");
  CHECK_FALSE(server.start(kLoopback, taken->port(), &err));
  CHECK(err.find("nie można otworzyć portu panelu") != std::string::npos);
  REQUIRE_MESSAGE(server.start(kLoopback, 0, &err), err);
  httplib::Client c(kLoopback, server.port());
  auto r = c.Get("/api/health");
  REQUIRE(r);
  CHECK(r->status == 200);
  CHECK(c.Get("/api/config")->status == 401);
  server.stop();
}

TEST_CASE("encoder HTTP: a server stopped right after it started does not hang") {
  // httplib ignores a stop() that comes before its listen thread runs; start() waits for it
  be::EncoderEngine engine{[] {
    be::EncoderEngineOptions o;
    o.enable_ptp = false;
    return o;
  }()};
  std::string err;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  REQUIRE(engine.start(cfg, &err));
  for (int i = 0; i < 50; ++i) {
    be::EncoderHttpServer server(engine, "");
    REQUIRE_MESSAGE(server.start(kLoopback, 0, &err), err);
    server.stop();  // returns: the listen thread has ended
  }
  {
    be::EncoderHttpServer server(engine, "");
    REQUIRE(server.start(kLoopback, 0, &err));
  }  // the destructor stops it as well
  engine.stop();
}

TEST_CASE("encoder service: the panel address file for the tray") {
  const std::string path = unique_path("panel");
  std::string err;
  REQUIRE_MESSAGE(be::write_panel_file(path, "127.0.0.1", 8070, &err), err);
  json j = json::parse(read_file(path));
  CHECK(j == json({{"bind", "127.0.0.1"}, {"port", 8070}}));
  REQUIRE(be::write_panel_file(path, "0.0.0.0", 8071, &err));
  j = json::parse(read_file(path));
  CHECK(j["bind"] == "0.0.0.0");
  CHECK(j["port"] == 8071);
  CHECK_FALSE(exists(path + ".tmp"));
  std::remove(path.c_str());
}

TEST_CASE("encoder service: panel.json lives in the public folder next to the configuration") {
  // the installer gives Users inheritable read access to public\ only (config.json stays private)
  CHECK(be::panel_file_path("C:\\ProgramData\\Bounce Encoder\\config.json") ==
        "C:\\ProgramData\\Bounce Encoder\\public\\panel.json");
  CHECK(be::panel_public_dir("C:\\ProgramData\\Bounce Encoder\\config.json") == "C:\\ProgramData\\Bounce Encoder\\public");
  CHECK(be::panel_file_path("/var/lib/enc/config.json") == "/var/lib/enc/public/panel.json");
  CHECK(be::panel_file_path("bounce-encoder-config.json") == "./public/panel.json");
  CHECK(be::panel_file_path("D:/test/moja.json") == "D:/test/public/panel.json");

  // write_panel_file creates the missing public folder and replaces the file in it
  struct TempDir {
    std::string path = "bam_enc_pubtest_" + std::to_string(bp::current_pid());
    ~TempDir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } temp;
  const std::string& dir = temp.path;
  REQUIRE(bp::ensure_directory(dir));
  const std::string config_path = dir + "/config.json";
  const std::string path = be::panel_file_path(config_path);
  std::string err;
  REQUIRE_MESSAGE(be::write_panel_file(path, "10.0.0.5", 8070, &err), err);
  REQUIRE_MESSAGE(be::write_panel_file(path, "10.0.0.6", 8071, &err), err);
  const json j = json::parse(read_file(path));
  CHECK(j == json({{"bind", "10.0.0.6"}, {"port", 8071}}));
  CHECK_FALSE(exists(path + ".tmp"));
  CHECK_FALSE(exists(dir + "/panel.json"));  // never next to config.json itself
}

TEST_CASE("encoder service: a missing data folder is created with the installer's protected access") {
  // the service's own folder: protected DACL, Administrators + SYSTEM only; public\ adds Users read
  const auto dirs = be::data_dirs("C:\\ProgramData\\Bounce Encoder\\config.json", true);
  REQUIRE(dirs.size() == 2);
  CHECK(dirs[0].path == "C:\\ProgramData\\Bounce Encoder");
  CHECK(dirs[0].sddl == "D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
  CHECK(dirs[1].path == "C:\\ProgramData\\Bounce Encoder\\public");
  CHECK(dirs[1].sddl == "D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FRFX;;;BU)");
  // the installer sets the same DACL on the folder
  const std::string nsi = read_file(BAM_TEST_DATA "/../../installer/nsis/BounceEncoder-setup.nsi");
  CHECK(nsi.find(std::string("!define FOLDER_SDDL \"") + be::kDataDirSddl + "\"") != std::string::npos);
  // a configuration elsewhere (--config, POSIX) gets no SDDL
  const auto plain = be::data_dirs("bounce-encoder-config.json", false);
  REQUIRE(plain.size() == 2);
  CHECK(plain[0].path == ".");
  CHECK(plain[0].sddl.empty());
  CHECK(plain[1].path == "./public");
  CHECK(plain[1].sddl.empty());

  struct TempDir {
    std::string path;
    ~TempDir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };
  const std::string tag = std::to_string(bp::current_pid());
  TempDir temp{"bam_enc_datadir_" + tag};
  const std::string config_path = temp.path + "/config.json";
  std::string err;
  REQUIRE_MESSAGE(be::create_data_dirs(be::data_dirs(config_path, false), &err), err);
  REQUIRE_MESSAGE(be::create_data_dirs(be::data_dirs(config_path, false), &err), err);  // existing: fine
  CHECK(std::filesystem::is_directory(temp.path));
  CHECK(std::filesystem::is_directory(temp.path + "/public"));
#ifdef _WIN32
  // the real security descriptors, each on a folder of its own in the working directory (a
  // non-elevated test could not create a folder inside one that only Administrators may use)
  TempDir data{"bam_enc_datadir_a_" + tag}, pub{"bam_enc_datadir_p_" + tag};
  REQUIRE_MESSAGE(be::create_data_dirs({{data.path, be::kDataDirSddl}, {pub.path, be::kPublicDirSddl}}, &err), err);
  const std::vector<std::pair<std::string, DWORD>> expected = {{data.path, 2}, {pub.path, 3}};
  for (const auto& [path, aces] : expected) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const std::filesystem::path native(path);
    REQUIRE(GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                                  &sd) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    CHECK(GetSecurityDescriptorControl(sd, &control, &revision));
    CHECK_MESSAGE((control & SE_DACL_PROTECTED) != 0, path);  // nothing inherited from the parent
    CHECK(dacl != nullptr);
    if (dacl) {
      CHECK_MESSAGE(dacl->AceCount == aces, path);
    }
    LocalFree(sd);
  }
#endif
  CHECK_FALSE(be::create_data_dirs({{temp.path + "/missing-parent/x", ""}}, &err));
  CHECK(err.find("missing-parent") != std::string::npos);
}

TEST_CASE("encoder HTTP: a request hidden in a refused request's body is never executed") {
  Fixture f;
  const uint16_t port = f.server->port();
  auto password_set = [&] { return json::parse(f.client->Get("/api/auth-state")->body)["password_set"].get<bool>(); };
  // read-ahead boundaries, and next to them
  const std::vector<size_t> offsets = {4096, 4095, 4097, 8192, 2000};

  SUBCASE("(a) inside a text/plain body: 415, then the connection closes") {
    for (size_t offset : offsets) {
      CAPTURE(offset);
      const auto head = [](size_t n) { return head_with_length("POST", "/api/setup", "text/plain", n); };
      const RawExchange x = raw_exchange(port, hiding_request(head, offset, kLastRequest));
      CHECK(x.closed);
      CHECK(statuses_or_reset(x.statuses, 415));  // one request per connection: nothing after it is read
      CHECK_FALSE(password_set());
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
      CHECK_FALSE(password_set());
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
      CHECK_FALSE(password_set());
    }
  }
  SUBCASE("(d) inside an unauthenticated request to a protected endpoint: 401, then the connection closes") {
    for (const std::string path : {"/api/stations", "/api/password", "/api/logout", "/api/config"}) {
      for (size_t offset : offsets) {
        CAPTURE(path);
        CAPTURE(offset);
        const auto head = [&](size_t n) { return head_with_length(path == "/api/config" ? "PUT" : "POST", path, "application/json", n); };
        const RawExchange x = raw_exchange(port, hiding_request(head, offset, kLastRequest));
        CHECK(x.closed);
        CHECK(statuses_or_reset(x.statuses, 401));
        CHECK_FALSE(password_set());
      }
    }
  }
}

TEST_CASE("encoder HTTP: an open live feed does not keep its session alive") {
  struct ResetClock {
    ~ResetClock() { be::testing::set_http_clock_offset_ns(0); }
  } reset;
  Fixture f;
  f.setup_and_login();
  constexpr int64_t hour = 3600LL * 1'000'000'000;
  std::atomic<int> events{0};
  std::atomic<bool> ended{false};
  std::thread t([&] {
    httplib::Client c(kLoopback, f.server->port());
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
  be::testing::set_http_clock_offset_ns(12 * hour - 60'000'000'000);
  const int seen = events;
  REQUIRE(wait_for([&] { return events > seen + 2; }, 3000));
  CHECK_FALSE(ended);
  // 12 h after the login (not after the last event) the session is over and the feed ends
  be::testing::set_http_clock_offset_ns(12 * hour + 1'000'000'000);
  REQUIRE(wait_for([&] { return ended.load(); }, 3000));
  t.join();
  CHECK(ended);
  CHECK(f.client->Get("/api/config", f.auth)->status == 401);
}

TEST_CASE("encoder HTTP: a login that checked the old password does not survive a password change") {
  struct ResetHook {
    ~ResetHook() { be::testing::set_login_verified_hook({}); }
  } reset;
  Fixture f;
  f.setup_and_login();  // the session that will change the password

  std::mutex m;
  std::condition_variable cv;
  bool in_login = false, changed = false;
  be::testing::set_login_verified_hook([&] {
    std::unique_lock lock(m);
    in_login = true;
    cv.notify_all();
    cv.wait_for(lock, std::chrono::seconds(5), [&] { return changed; });
  });
  int login_status = 0;
  std::string login_cookie;
  std::thread t([&] {
    httplib::Client c(kLoopback, f.server->port());
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
  be::testing::set_login_verified_hook({});
  CHECK(login_status == 401);
  CHECK(login_cookie.empty());
  f.take_cookie(change);
  f.get("/api/config");  // the session that changed it goes on
}

TEST_CASE("encoder HTTP: one request per connection, whatever httplib makes of a request") {
  Fixture f;
  const uint16_t port = f.server->port();
  auto password_set = [&] { return json::parse(f.client->Get("/api/auth-state")->body)["password_set"].get<bool>(); };
  const std::vector<size_t> offsets = {4096, 8192, 12288};  // httplib's 4 KB read-ahead boundaries
  // The first response only, then the server closes: the hidden setup copies are never parsed.
  auto check_vector = [&](const std::string& name, const std::function<std::string(size_t)>& head, const std::string& tail = "") {
    for (size_t offset : offsets) {
      CAPTURE(name);
      CAPTURE(offset);
      const RawExchange x = raw_exchange(port, hiding_request(head, offset, tail + kLastRequest));
      CHECK(x.closed);
      CHECK(x.statuses.size() <= 1);  // a reset may (rarely) cost the client even the first answer
      if (!x.statuses.empty()) CHECK(x.statuses[0] >= 400);
      CHECK_FALSE(password_set());
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

TEST_CASE("encoder HTTP: methods, encodings, ranges and body types outside the allowlist are refused") {
  Fixture f;
  const uint16_t port = f.server->port();
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
  CHECK(json::parse(f.client->Get("/api/auth-state")->body)["password_set"] == false);
  // the panel files still answer, and the live feed still runs on its own connection
  CHECK(f.client->Get("/")->status == 200);
}
// ---- Stereo Tool processing API (Bounce Encoder 1.1) ----

namespace {

namespace fs = std::filesystem;

// test keys only, never a real one
constexpr const char* kStationKey = "<FAKE-HTTP-STATION-KEY-77>";
constexpr const char* kDefaultKey = "<FAKE-HTTP-DEFAULT-KEY-88>";

// A key, with or without its brackets, anywhere in `text`.
bool has_key(const std::string& text) {
  for (const std::string k : {kStationKey, kDefaultKey})
    if (text.find(k) != std::string::npos || text.find(k.substr(1, k.size() - 2)) != std::string::npos) return true;
  return false;
}

// A stereotool folder of its own for a test server.
struct StFolder {
  std::string path;
  explicit StFolder(const std::string& tag) {
    path = (fs::temp_directory_path() / ("bam_enc_http_st_" + tag + "_" + std::to_string(bp::current_pid()))).string();
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  ~StFolder() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  StFolder(const StFolder&) = delete;
  StFolder& operator=(const StFolder&) = delete;
  std::vector<std::string> files() const {  // every file under it, relative
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file()) out.push_back(fs::relative(it->path(), path).generic_string());
    return out;
  }
};

std::string fake_sts(double gain, int latency) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "[FakeStereoTool]\ngain=%.9g\nlatency=%d\n", gain, latency);
  return buf;
}

json upload_body(const std::string& name, const std::string& data) { return {{"name", name}, {"data_base64", be::testing::base64(data)}}; }

// A station with processing and nothing else (no inputs: it mixes silence; no streams).
json processing_station(const std::string& name, bool enabled, const std::string& preset, const std::string& key) {
  return {{"name", name}, {"enabled", enabled}, {"processing", {{"enabled", true}, {"preset", preset}, {"license_key", key}}}};
}

std::string url_path(const std::string& name) {  // a preset name in a path (only spaces need it here)
  std::string out;
  for (char ch : name) out += ch == ' ' ? std::string("%20") : std::string(1, ch);
  return "/api/stereotool/presets/" + out;
}

}  // namespace

TEST_CASE("encoder HTTP: every Stereo Tool route needs a session") {
  Fixture f;
  f.setup_and_login();
  httplib::Client anon(kLoopback, f.server->port());
  CHECK(anon.Get("/api/stereotool")->status == 401);
  CHECK(anon.Get("/api/stereotool/presets")->status == 401);
  CHECK(anon.Get("/api/stereotool/presets/FM.sts")->status == 401);
  CHECK(anon.Delete("/api/stereotool/presets/FM.sts")->status == 401);
  CHECK(anon.Post("/api/stereotool/presets", upload_body("FM.sts", "x").dump(), "application/json")->status == 401);
  CHECK(anon.Post("/api/stations/s1/processing/save-preset", R"({"name":"A.sts"})", "application/json")->status == 401);
  CHECK(anon.Post("/api/stations/s1/processing/restart", "", "application/json")->status == 401);
  CHECK(anon.Get("/api/stations/s1/processing/web-password")->status == 401);
  // JSON only, on every route
  CHECK(f.client->Post("/api/stereotool/presets", f.auth, upload_body("FM.sts", "x").dump(), "text/plain")->status == 415);
  CHECK(f.client->Post("/api/stations/s1/processing/save-preset", f.auth, R"({"name":"A.sts"})", "text/plain")->status == 415);
  CHECK(f.client->Post("/api/stations/s1/processing/restart", f.auth, "x", "text/plain")->status == 415);
}

#ifdef BAM_FAKE_STEREOTOOL_PATH

TEST_CASE("encoder HTTP: Stereo Tool presets are uploaded, listed, downloaded and deleted, with sanitised names and limits") {
  fake_st::UseFake use;
  StFolder dir("presets");
  Fixture f(dir.path);
  f.setup_and_login();

  json st = f.get("/api/stereotool");
  CHECK(st["library"]["available"] == true);  // the fake, looked for by this request
  CHECK(st["library"]["checked"] == true);
  CHECK(st["library"]["version"] == 11070);
  CHECK(st["library"]["version_text"] == "11.07");
  CHECK(st["library"]["abandoned"] == 0);
  CHECK(st["presets"].empty());
  CHECK(st["limits"]["max_preset_bytes"] == be::kMaxPresetBytes);
  CHECK(st["limits"]["max_presets"] == be::kMaxPresets);
  CHECK(st["limits"]["max_upload_bytes"] == be::kMaxPresetUploadBytes);
  CHECK(st["web_port_base"] == 8100);
  CHECK(st["default_key_set"] == false);
  CHECK(st["stations"].empty());
  CHECK(st["warnings"].empty());

  // upload
  auto r = f.post("/api/stereotool/presets", upload_body("FM Rock.sts", fake_sts(0.5, 128)));
  REQUIRE(r);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  json out = json::parse(r->body);
  CHECK(out["ok"] == true);
  CHECK(out["preset"] == json{{"name", "FM Rock.sts"}, {"bytes", fake_sts(0.5, 128).size()}});
  REQUIRE(out["presets"].size() == 1);
  CHECK(out["presets"][0]["name"] == "FM Rock.sts");
  CHECK(out["presets"][0]["used_by"].empty());
  CHECK(f.get("/api/stereotool/presets")["presets"] == out["presets"]);

  // download, in any case
  json got = f.get(url_path("fm rock.STS"));
  CHECK(got["name"] == "FM Rock.sts");
  CHECK(got["bytes"] == fake_sts(0.5, 128).size());
  CHECK(got["data_base64"] == be::testing::base64(fake_sts(0.5, 128)));
  r = f.client->Get("/api/stereotool/presets/Brak.sts", f.auth);
  CHECK(r->status == 404);
  CHECK(json::parse(r->body)["code"] == "not_found");

  // a duplicate in another case
  r = f.post("/api/stereotool/presets", upload_body("fm rock.STS", "x"));
  CHECK(r->status == 409);
  CHECK(json::parse(r->body)["code"] == "exists");
  CHECK(json::parse(r->body)["field"] == "name");

  // names: refused, never quoted, nothing written anywhere
  const std::vector<std::string> before = dir.files();
  for (const std::string& bad : std::vector<std::string>{"../config.json", "..\\x.sts", "../../x.sts", "C:x.sts", "a/b.sts", "a\\b.sts", "CON.sts", "nul.sts",
                                "zażółć.sts", " spacja.sts", "x.txt", ".sts", "x.sts\nGET", std::string("x\0y.sts", 7),
                                std::string(kStationKey) + ".sts"}) {
    CAPTURE(bad);
    r = f.post("/api/stereotool/presets", upload_body(bad, "dane"));
    REQUIRE(r);
    CHECK(r->status == 400);
    const json e = json::parse(r->body);
    CHECK(e["code"] == "bad_name");
    CHECK(e["field"] == "name");
    CHECK(e["ok"] == false);
    if (bad.size() > 4) CHECK_MESSAGE(r->body.find(json(bad).dump().substr(1, bad.size() - 1)) == std::string::npos, r->body);
    CHECK_FALSE(has_key(r->body));
  }
  CHECK(dir.files() == before);
  CHECK_FALSE(exists((fs::path(dir.path).parent_path() / "config.json").string()));
  for (const json& bad : {json{{"data_base64", "eA=="}}, json{{"name", 5}, {"data_base64", "eA=="}}, json{{"name", ""}, {"data_base64", "eA=="}}}) {
    r = f.post("/api/stereotool/presets", bad);
    CHECK(r->status == 400);
    CHECK(json::parse(r->body)["field"] == "name");
  }
  CHECK(f.post("/api/stereotool/presets", json::array())->status == 400);
  CHECK(f.client->Post("/api/stereotool/presets", f.auth, "{nie json", "application/json")->status == 400);

  // the data: strict base64, not empty, at most 4 MB
  for (const std::string bad : {"eA", "eA=", "e A==", "eA==\n", "!!!!", "eA==eA==", "=eA=", "data:;base64,eA==",
                                 "eB==", "eHl="}) {
    CAPTURE(bad);
    r = f.post("/api/stereotool/presets", {{"name", "Zle.sts"}, {"data_base64", bad}});
    CHECK(r->status == 400);
    CHECK(json::parse(r->body)["code"] == "bad_data");
    CHECK(json::parse(r->body)["field"] == "data_base64");
  }
  r = f.post("/api/stereotool/presets", {{"name", "Zle.sts"}});
  CHECK(r->status == 400);
  CHECK(json::parse(r->body)["field"] == "data_base64");
  r = f.post("/api/stereotool/presets", {{"name", "Pusty.sts"}, {"data_base64", ""}});
  CHECK(r->status == 400);
  CHECK(json::parse(r->body)["code"] == "empty");
  // the base64 decoder against the encoder, every tail length
  for (const std::string& data : std::vector<std::string>{"a", "ab", "abc", "abcd", std::string("\0\xff\x10", 3)}) {
    const std::string name = "Dlugosc " + std::to_string(data.size()) + ".sts";
    REQUIRE_MESSAGE(f.post("/api/stereotool/presets", upload_body(name, data))->status == 200, name);
    CHECK(f.get(url_path(name))["data_base64"] == be::testing::base64(data));
    REQUIRE(f.client->Delete(url_path(name), f.auth)->status == 200);
  }
  const std::string max(be::kMaxPresetBytes, 'p');
  r = f.post("/api/stereotool/presets", upload_body("Duzy.sts", max + "p"));  // a body over 1 MB: only this route takes it
  REQUIRE(r);
  CHECK(r->status == 413);
  CHECK(json::parse(r->body)["code"] == "too_large");
  r = f.post("/api/stereotool/presets", upload_body("Duzy.sts", max));
  REQUIRE(r);
  REQUIRE_MESSAGE(r->status == 200, r->body.substr(0, 300));
  got = f.get(url_path("Duzy.sts"));
  CHECK(got["bytes"] == be::kMaxPresetBytes);
  CHECK(got["data_base64"].get<std::string>().size() == (be::kMaxPresetBytes + 2) / 3 * 4);
  r = f.post("/api/stereotool/presets", {{"name", "Za duzy.sts"}, {"data_base64", std::string((be::kMaxPresetBytes + 2) / 3 * 4 + 4, 'A')}});
  CHECK(r->status == 413);  // refused before decoding

  // traversal through the path: only names in the presets folder
  for (const std::string path : {"/api/stereotool/presets/..%2Fconfig.json", "/api/stereotool/presets/..%2F..%2Fx.sts",
                                 "/api/stereotool/presets/%2E%2E%2Fpresets%2FFM%20Rock.sts", "/api/stereotool/presets/a/b.sts"}) {
    CAPTURE(path);
    r = f.client->Get(path, f.auth);
    REQUIRE(r);
    CHECK(r->status == 404);
    CHECK(r->body.find("data_base64") == std::string::npos);
    r = f.client->Delete(path, f.auth);
    CHECK(r->status == 404);
  }
  CHECK(f.get("/api/stereotool")["presets"].size() == 2);

  // a preset a station uses (here a disabled one) cannot be deleted; the list says who uses it
  json cfg = f.get("/api/config");
  cfg["stations"] = json::array({processing_station("Radio Preset", false, "fm rock.sts", kStationKey)});
  r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  st = f.get("/api/stereotool");
  CHECK(st["presets"][0]["name"] == "Duzy.sts");  // sorted without regard to case
  CHECK(st["presets"][1]["used_by"] == json::array({{{"id", "s1"}, {"name", "Radio Preset"}}}));
  r = f.client->Delete(url_path("FM Rock.sts"), f.auth);
  CHECK(r->status == 409);
  CHECK(json::parse(r->body)["code"] == "in_use");
  CHECK_MESSAGE(json::parse(r->body)["error"].get<std::string>().find("„Radio Preset”") != std::string::npos, r->body);
  CHECK(f.get(url_path("FM Rock.sts"))["bytes"] == fake_sts(0.5, 128).size());
  r = f.put("/api/stations/s1", processing_station("Radio Preset", false, "Duzy.sts", ""));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  r = f.client->Delete(url_path("fm rock.sts"), f.auth);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  out = json::parse(r->body);
  CHECK(out["ok"] == true);
  REQUIRE(out["presets"].size() == 1);
  CHECK(out["presets"][0]["name"] == "Duzy.sts");
  CHECK(f.client->Delete(url_path("FM Rock.sts"), f.auth)->status == 404);
  CHECK_FALSE(has_key(f.get("/api/stereotool").dump()));
}

TEST_CASE("encoder HTTP: processing through the API: settings, status, save as preset, restart; the licence key never leaves") {
  fake_st::UseFake use;
  StFolder dir("processing");
  Fixture f(dir.path);
  f.setup_and_login();
  std::vector<std::string> bodies;  // every answer, scanned for the keys at the end
  auto keep = [&](httplib::Result res) {
    REQUIRE(res);
    bodies.push_back(res->body);
    return res;
  };
  REQUIRE(keep(f.post("/api/stereotool/presets", upload_body("Start.sts", fake_sts(0.5, 128))))->status == 200);

  // a preset that is not uploaded is refused for an enabled station, allowed for a disabled one
  json cfg = f.get("/api/config");
  cfg["stations"] = json::array({processing_station("Radio Brak", true, "Nie ma.sts", kStationKey)});
  auto r = keep(f.put("/api/config", cfg));
  CHECK(r->status == 400);
  json out = json::parse(r->body);
  CHECK(out["issues"][0]["field"] == "stations[0].processing.preset");
  CHECK_MESSAGE(out["error"].get<std::string>().find("nie jest wgrany") != std::string::npos, r->body);
  CHECK(out["warnings"].empty());
  CHECK(f.engine.config().stations.empty());
  cfg["stations"][0]["enabled"] = false;
  REQUIRE_MESSAGE(keep(f.put("/api/config", cfg))->status == 200, bodies.back());
  cfg = f.get("/api/config");
  cfg["stations"][0]["enabled"] = true;  // switching that station on needs the preset
  CHECK(keep(f.put("/api/config", cfg))->status == 400);

  // three stations with processing (one own key, two on the default key), one without
  cfg = f.get("/api/config");
  cfg["stereotool_default_key"] = kDefaultKey;
  cfg["stations"] = json::array({processing_station("Radio Jeden", true, "start.STS", kStationKey),
                                 processing_station("Radio Dwa", true, "Start.sts", ""),
                                 processing_station("Radio Trzy", true, "Start.sts", ""),
                                 {{"name", "Radio Bez"}, {"enabled", true}}});
  r = keep(f.put("/api/config", cfg));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  out = json::parse(r->body);
  CHECK(out["config"]["stereotool_default_key"] == "");
  CHECK(out["config"]["stereotool_default_key_set"] == true);
  CHECK(out["config"]["stations"][0]["processing"]["license_key"] == "");
  CHECK(out["config"]["stations"][0]["processing"]["license_key_set"] == true);
  CHECK(out["config"]["stations"][1]["processing"]["license_key_set"] == false);
  const std::vector<int> slots = {out["config"]["stations"][0]["processing"]["web_port_slot"].get<int>(),
                                  out["config"]["stations"][1]["processing"]["web_port_slot"].get<int>()};
  // the same default key on two stations: a warning naming both, never the key
  REQUIRE(out["warnings"].size() == 1);
  CHECK(out["warnings"][0]["field"] == "stations[2].processing.license_key");
  CHECK_MESSAGE(out["warnings"][0]["message"].get<std::string>().find("Radio Dwa") != std::string::npos, r->body);
  const std::string s1 = out["config"]["stations"][0]["id"], s4 = out["config"]["stations"][3]["id"];
  CHECK(f.file()["stations"][0]["processing"]["license_key"] == kStationKey);  // only the file holds it

  // the status: per station and the library
  const bool running = wait_for(
      [&] {
        const json s = f.get("/api/status");
        for (const json& station : s["stations"])
          if (station["processing"]["state"] != "running" && station["processing"]["enabled"] == true) return false;
        return true;
      },
      10000);
  REQUIRE(running);
  json status = f.get("/api/status");
  bodies.push_back(status.dump());
  const json& p = status["stations"][0]["processing"];
  CHECK(p["enabled"] == true);
  CHECK(p["preset"] == "start.STS");
  CHECK(p["reason_code"] == "");
  CHECK(p["reason"] == "");
  CHECK(p["latency_ms"] == doctest::Approx((128 + 240) / 48.0).epsilon(0.01));
  CHECK(p["license_valid"] == true);
  CHECK(p["unlicensed_features"] == "");
  CHECK(p["web_port"] == 8100 + slots[0]);
  CHECK(p["web_url"] == "http://127.0.0.1:" + std::to_string(8100 + slots[0]) + "/");
  CHECK(p["retries"] == 0);
  CHECK(p["retry_in_s"] == -1);
  CHECK(p.contains("cpu_pct"));
  CHECK(p.contains("queue_dropped_frames"));
  CHECK(status["stations"][3]["processing"]["state"] == "off");
  CHECK(status["stations"][3]["processing"]["enabled"] == false);
  CHECK(status["stations"][3]["processing"]["web_port"] == 0);
  CHECK(status["stereotool"]["available"] == true);
  CHECK(status["stereotool"]["version_text"] == "11.07");
  CHECK_FALSE(status["stereotool"].contains("path"));
  json st = f.get("/api/stereotool");
  bodies.push_back(st.dump());
  CHECK(st["default_key_set"] == true);
  CHECK(st["stations"][0]["license_key_set"] == true);
  CHECK(st["stations"][0]["web_port_slot"] == slots[0]);
  CHECK(st["stations"][0]["web_port"] == 8100 + slots[0]);
  CHECK(st["stations"][0]["web_port_active"] == 8100 + slots[0]);
  CHECK(st["stations"][1]["web_url"] == "http://127.0.0.1:" + std::to_string(8100 + slots[1]) + "/");
  CHECK(st["stations"][3]["processing_enabled"] == false);
  CHECK(st["stations"][3]["web_port_active"] == 0);
  CHECK(st["warnings"].size() == 1);
  CHECK(st["presets"][0]["used_by"].size() == 3);
  CHECK(fake_st::fake().text("create_key").has_value());  // the keys did reach Stereo Tool

  // save the running settings as a preset
  r = keep(f.post("/api/stations/" + s1 + "/processing/save-preset", {{"name", "Strojony.sts"}}));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  out = json::parse(r->body);
  CHECK(out["ok"] == true);
  CHECK(out["preset"]["name"] == "Strojony.sts");
  CHECK(out["presets"].size() == 2);
  {
    // what the instance runs with, then its .sts values (the fake writes them all)
    std::string saved;
    REQUIRE(f.engine.get_preset("Strojony.sts", &saved).ok());
    CHECK(saved.rfind(fake_sts(0.5, 128), 0) == 0);
    CHECK(out["preset"]["bytes"] == saved.size());
    CHECK(f.get(url_path("Strojony.sts"))["data_base64"] == be::testing::base64(saved));
  }
  auto refused = [&](httplib::Result res, int status, const std::string& code) {
    REQUIRE(res);
    bodies.push_back(res->body);
    CHECK_MESSAGE(res->status == status, res->body);
    CHECK_MESSAGE(json::parse(res->body)["code"] == code, res->body);
  };
  refused(f.post("/api/stations/" + s1 + "/processing/save-preset", {{"name", "STROJONY.sts"}}), 409, "exists");
  refused(f.post("/api/stations/" + s1 + "/processing/save-preset", {{"name", "../x.sts"}}), 400, "bad_name");
  refused(f.post("/api/stations/" + s1 + "/processing/save-preset", json::object()), 400, "bad_name");
  refused(f.post("/api/stations/" + s1 + "/processing/save-preset", json::array()), 400, "bad_request");
  refused(f.post("/api/stations/s99/processing/save-preset", {{"name", "Inny.sts"}}), 404, "no_station");
  refused(f.post("/api/stations/" + s4 + "/processing/save-preset", {{"name", "Inny.sts"}}), 409, "not_running");
  fake_st::fake().set("save_fails", 1);
  refused(f.post("/api/stations/" + s1 + "/processing/save-preset", {{"name", "Nieudany.sts"}}), 500, "failed");
  fake_st::fake().set("save_fails", 0);
  // a second save while one is under way: 409
  fake_st::fake().set("save_sleep_us", 800'000);
  std::thread first([&] {
    httplib::Client c(kLoopback, f.server->port());
    c.set_read_timeout(20, 0);
    c.Post("/api/stations/" + s1 + "/processing/save-preset", f.auth, json{{"name", "Pierwszy.sts"}}.dump(), "application/json");
  });
  JoinOnExit join_first{first};
  bp::sleep_ns(200'000'000);
  refused(f.post("/api/stations/" + s1 + "/processing/save-preset", {{"name", "Drugi.sts"}}), 409, "busy");
  first.join();
  fake_st::fake().set("save_sleep_us", 0);

  // restart
  r = keep(f.client->Post("/api/stations/" + s1 + "/processing/restart", f.auth, "", "application/json"));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["ok"] == true);
  refused(f.client->Post("/api/stations/s99/processing/restart", f.auth, "", "application/json"), 404, "no_station");
  refused(f.client->Post("/api/stations/" + s4 + "/processing/restart", f.auth, "", "application/json"), 409, "not_running");

  // an update without "processing" keeps it; a partial one changes only what it has
  r = keep(f.put("/api/stations/" + s1, {{"name", "Radio Jeden Nowe"}, {"enabled", true}}));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  be::EncoderConfig c = f.engine.config();
  CHECK(c.stations[0].name == "Radio Jeden Nowe");
  CHECK(c.stations[0].processing.enabled);
  CHECK(c.stations[0].processing.preset == "start.STS");
  CHECK(c.stations[0].processing.license_key == kStationKey);
  CHECK(c.stations[0].processing.web_port_slot == slots[0]);
  cfg = f.get("/api/config");
  for (json& station : cfg["stations"]) station.erase("processing");
  r = keep(f.put("/api/config", cfg));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  c = f.engine.config();
  for (size_t i = 0; i < 3; ++i) CHECK(c.stations[i].processing.enabled);
  CHECK(c.stations[1].processing.web_port_slot == slots[1]);
  r = keep(f.put("/api/stations/" + s1, {{"name", "Radio Jeden Nowe"}, {"enabled", true}, {"processing", {{"enabled", false}}}}));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  c = f.engine.config();
  CHECK_FALSE(c.stations[0].processing.enabled);
  CHECK(c.stations[0].processing.preset == "start.STS");
  CHECK(c.stations[0].processing.license_key == kStationKey);
  CHECK(c.stations[0].processing.web_port_slot == slots[0]);
  r = keep(f.put("/api/stations/" + s1, {{"processing", "wlaczony"}}));
  CHECK(r->status == 400);
  // the key goes only on request
  r = keep(f.put("/api/stations/" + s1, {{"name", "Radio Jeden Nowe"}, {"processing", {{"license_key_clear", true}}}}));
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["config"]["stations"][0]["processing"]["license_key_set"] == false);
  CHECK(f.engine.config().stations[0].processing.license_key.empty());

  // nothing the server says carries a key: answers, the status, the live feed, the logs
  bodies.push_back(f.get("/api/config").dump());
  bodies.push_back(f.get("/api/status").dump());
  bodies.push_back(f.get("/api/logs").dump());
  {
    httplib::Client live(kLoopback, f.server->port());
    std::string first;
    live.Get("/api/live", f.auth, [&](const char* data, size_t n) {
      first.append(data, n);
      return first.find("\n\n") == std::string::npos;  // one event
    });
    CHECK(first.find("\"processing\"") != std::string::npos);
    bodies.push_back(first);
  }
  for (const auto& e : bam::Logger::instance().recent()) bodies.push_back(e.message);
  for (const std::string& b : bodies) CHECK_MESSAGE(!has_key(b), b.substr(0, 400));
}

// Stereo Tool's own web interface password (owner decision, final review): only a logged-in panel
// user gets it, on request; never in the status, the live feed, the configuration, the logs or a
// saved preset.
TEST_CASE("encoder HTTP: the Stereo Tool web interface password is given only on request, with a session") {
  fake_st::UseFake use;
  StFolder dir("webpw");
  Fixture f(dir.path);
  f.setup_and_login();
  REQUIRE(f.post("/api/stereotool/presets", upload_body("Start.sts", fake_sts(0.5, 128)))->status == 200);
  json cfg = f.get("/api/config");
  cfg["stations"] = json::array({processing_station("Radio Haslo", true, "Start.sts", kStationKey),
                                 {{"name", "Radio Bez"}, {"enabled", true}}});
  auto r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  const json out = json::parse(r->body);
  const std::string s1 = out["config"]["stations"][0]["id"], s2 = out["config"]["stations"][1]["id"];
  REQUIRE(wait_for([&] { return f.get("/api/status")["stations"][0]["processing"]["state"] == "running"; }, 10000));

  r = f.client->Get("/api/stations/" + s1 + "/processing/web-password", f.auth);
  REQUIRE(r);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  const std::string pw = json::parse(r->body)["password"];
  CHECK(pw.size() >= 16);
  CHECK(fake_st::fake().sts(("station-" + s1).c_str(), be::detail::st::kParamPassword0) == std::optional<std::string>(pw));
  CHECK(json::parse(r->body)["web_port"] == f.get("/api/status")["stations"][0]["processing"]["web_port"]);
  CHECK(r->get_header_value("Cache-Control") == "no-store");
  httplib::Client anon(kLoopback, f.server->port());
  CHECK(anon.Get("/api/stations/" + s1 + "/processing/web-password")->status == 401);
  r = f.client->Get("/api/stations/" + s2 + "/processing/web-password", f.auth);
  CHECK(r->status == 409);
  CHECK(json::parse(r->body)["code"] == "not_running");
  CHECK(f.client->Get("/api/stations/s99/processing/web-password", f.auth)->status == 404);

  // a restart keeps the worker's password
  REQUIRE(f.client->Post("/api/stations/" + s1 + "/processing/restart", f.auth, "", "application/json")->status == 200);
  REQUIRE(wait_for([&] { return f.get("/api/status")["stations"][0]["processing"]["state"] == "running"; }, 10000));
  CHECK(json::parse(f.client->Get("/api/stations/" + s1 + "/processing/web-password", f.auth)->body)["password"] == pw);

  // nowhere else: a saved preset, the status, the live feed, the configuration, the Stereo Tool page, the logs
  REQUIRE(f.post("/api/stations/" + s1 + "/processing/save-preset", {{"name", "Zapisany.sts"}})->status == 200);
  std::vector<std::string> bodies;
  std::string saved;
  REQUIRE(f.engine.get_preset("Zapisany.sts", &saved).ok());
  bodies.push_back(saved);
  bodies.push_back(f.get(url_path("Zapisany.sts")).dump());
  bodies.push_back(f.get("/api/status").dump());
  bodies.push_back(f.get("/api/config").dump());
  bodies.push_back(f.get("/api/stereotool").dump());
  bodies.push_back(f.get("/api/logs").dump());
  {
    httplib::Client live(kLoopback, f.server->port());
    std::string first;
    live.Get("/api/live", f.auth, [&](const char* data, size_t n) {
      first.append(data, n);
      return first.find("\n\n") == std::string::npos;  // one event
    });
    bodies.push_back(first);
  }
  for (const auto& e : bam::Logger::instance().recent()) bodies.push_back(e.message);
  for (const std::string& b : bodies) CHECK_MESSAGE(b.find(pw) == std::string::npos, b.substr(0, 300));
  CHECK(saved.find("sts.") != std::string::npos);  // the fake wrote its values (the password among them)
}

// The crash-loop guard (owner decision): a station held after an unclean end with processing gets
// its processing back when it is saved in the panel (or restarted by hand).
TEST_CASE("encoder HTTP: a save of a station held by the crash-loop guard brings its processing back") {
  fake_st::UseFake use;
  StFolder dir("guard");
  const std::string marker = (fs::path(dir.path) / "running.marker").string();
  Fixture f(dir.path, marker);
  f.setup_and_login();
  REQUIRE(f.post("/api/stereotool/presets", upload_body("Start.sts", fake_sts(0.5, 128)))->status == 200);
  json cfg = f.get("/api/config");
  cfg["stations"] = json::array({processing_station("Radio Jeden", true, "Start.sts", kStationKey),
                                 processing_station("Radio Dwa", true, "Start.sts", kDefaultKey)});
  auto r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  const json out = json::parse(r->body);
  const std::string s1 = out["config"]["stations"][0]["id"], s2 = out["config"]["stations"][1]["id"];
  // the service ends unclean while Stereo Tool runs, and starts again
  const be::EncoderConfig running_cfg = f.engine.config();
  f.engine.stop();
  write_file(marker, "processing=1\n");
  std::string err;
  REQUIRE_MESSAGE(f.engine.start(running_cfg, &err), err);
  json status = f.get("/api/status");
  for (int i : {0, 1}) {
    CHECK(status["stations"][i]["processing"]["state"] == "error");
    CHECK(status["stations"][i]["processing"]["reason_code"] == "previous_crash");
    CHECK_MESSAGE(status["stations"][i]["processing"]["reason"].get<std::string>().find("awaria przy poprzednim uruchomieniu") !=
                      std::string::npos,
                  status.dump());
  }
  // a save of the first station (as the panel sends it): its processing is back, the other one waits
  json station = f.get("/api/config")["stations"][0];
  r = f.put("/api/stations/" + s1, station);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(wait_for([&] { return f.get("/api/status")["stations"][0]["processing"]["state"] == "running"; }, 10000));
  CHECK(f.get("/api/status")["stations"][1]["processing"]["reason_code"] == "previous_crash");
  // "Uruchom ponownie processing" of the other
  r = f.client->Post("/api/stations/" + s2 + "/processing/restart", f.auth, "", "application/json");
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(wait_for([&] { return f.get("/api/status")["stations"][1]["processing"]["state"] == "running"; }, 10000));
}

TEST_CASE("encoder HTTP: a station replaced by one with an id in another case gets its own instance; a deleted station's folder goes") {
  fake_st::UseFake use;
  StFolder dir("replace");
  Fixture f(dir.path);
  f.setup_and_login();
  REQUIRE(f.post("/api/stereotool/presets", upload_body("P.sts", fake_sts(0.5, 64)))->status == 200);
  REQUIRE(f.post("/api/stations", processing_station("Stara", true, "P.sts", kStationKey))->status == 200);
  auto state_of = [&](const std::string& id) {
    const json status = f.get("/api/status");
    for (const json& st : status["stations"])
      if (st["id"] == id) return st["processing"]["state"].get<std::string>();
    return std::string("brak");
  };
  REQUIRE(wait_for([&] { return state_of("s1") == "running"; }, 10000));
  REQUIRE(fs::is_directory(fs::path(dir.path) / "station-s1"));
  json cfg = f.get("/api/config");
  json fresh = processing_station("Nowa", true, "P.sts", kDefaultKey);
  fresh["id"] = "S1";
  cfg["stations"] = json::array({fresh});
  auto r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(json::parse(r->body)["config"]["stations"][0]["id"] == "s2");
  REQUIRE(wait_for([&] { return state_of("s2") == "running"; }, 10000));
  CHECK(fake_st::fake().text("create_name") == std::optional<std::string>("station-s2"));
  CHECK(fake_st::fake().value("created") == 2);
  CHECK(wait_for([&] { return fake_st::fake().value("live") == 1; }, 5000));
  CHECK_FALSE(fs::exists(fs::path(dir.path) / "station-s1"));  // gone with its station
  CHECK(fs::is_directory(fs::path(dir.path) / "station-s2"));
  // a disabled station keeps its folder; deleting it removes it
  REQUIRE(f.put("/api/stations/s2", {{"name", "Nowa"}, {"enabled", false}})->status == 200);
  CHECK(wait_for([&] { return fake_st::fake().value("live") == 0; }, 5000));
  CHECK(fs::is_directory(fs::path(dir.path) / "station-s2"));
  REQUIRE(f.client->Delete("/api/stations/s2", f.auth)->status == 200);
  CHECK_FALSE(fs::exists(fs::path(dir.path) / "station-s2"));
  CHECK(fs::is_directory(fs::path(dir.path) / "presets"));
}

TEST_CASE("encoder HTTP: the status names no folder in the library error; the Stereo Tool page does") {
  fake_st::UseFake use;
  StFolder dir("nolib");
  StFolder empty("nolib-empty");
  fs::create_directories(empty.path);
  be::set_stereotool_library_dirs({empty.path});
  Fixture f(dir.path);
  f.setup_and_login();
  const json st = f.get("/api/stereotool");
  REQUIRE(st["library"]["available"] == false);
  CHECK_MESSAGE(st["library"]["error"].get<std::string>().find(empty.path) != std::string::npos, st.dump());
  const json status = f.get("/api/status");
  const std::string error = status["stereotool"]["error"];
  CHECK_MESSAGE(error.find(empty.path) == std::string::npos, error);
  CHECK_MESSAGE(error.find("folder") != std::string::npos, error);
  CHECK_FALSE(error.empty());
}

#endif  // BAM_FAKE_STEREOTOOL_PATH

TEST_CASE("encoder HTTP: only the preset upload takes up to 6 MB, and only with a session; one request per connection holds for it") {
  StFolder dir("limits");
  Fixture f(dir.path);
  f.setup_and_login();
  const uint16_t port = f.server->port();
  const size_t big = 2'000'000;  // over the 1 MB of every other request
  auto head = [&](const std::string& method, const std::string& path, size_t n, const std::string& cookie, const std::string& type = "application/json") {
    return head_with_length(method, path, type, n, cookie);
  };
  auto ptp_domain = [&] { return f.engine.config().ptp_domain; };

  // with a session: a body over 1 MB is read and answered
  {
    const std::string data(1'200'000, 'd');
    auto r = f.post("/api/stereotool/presets", upload_body("Ponad 1 MB.sts", data));
    REQUIRE(r);
    REQUIRE_MESSAGE(r->status == 200, r->body.substr(0, 300));
  }
  // over 6 MB: refused on its headers
  CHECK(raw_status(port, head("POST", "/api/stereotool/presets", be::kMaxPresetUploadBytes + 1, f.cookie())) == 413);
  {
    const RawExchange x = raw_exchange(port, head("POST", "/api/stereotool/presets", be::kMaxPresetUploadBytes + 1, f.cookie()));
    CHECK(x.closed);
    CHECK(x.statuses == std::vector<int>{413});
    CHECK(x.text.find("najwyżej 6 MB") != std::string::npos);
  }
  // without a session (none, forged, logged out) an upload over 1 MB is refused on its headers with
  // 401, unread; on any other route or method 1 MB holds (413)
  for (const std::string& cookie : {std::string(), std::string("bounce_encoder_session=podrobiona")}) {
    CAPTURE(cookie);
    const RawExchange x = raw_exchange(port, head("POST", "/api/stereotool/presets", big, cookie));
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 401));
    if (!x.statuses.empty() && !kResetMayLoseAnswer) CHECK(x.text.find("wymagane logowanie") != std::string::npos);
  }
  for (const auto& [method, path] : std::vector<std::pair<std::string, std::string>>{{"PUT", "/api/config"},
                                                                                       {"POST", "/api/stations"},
                                                                                       {"POST", "/api/stereotool/presets/"},
                                                                                       {"POST", "/api/stereotool/presets/x.sts"},
                                                                                       {"POST", "/api/stereotool/presets/../presets"},
                                                                                       {"POST", "/API/stereotool/presets"},
                                                                                       {"PUT", "/api/stereotool/presets"},
                                                                                       {"DELETE", "/api/stereotool/presets"},
                                                                                       {"POST", "/api/stations/s1/processing/save-preset"},
                                                                                       {"POST", "/api/login"}}) {
    CAPTURE(method);
    CAPTURE(path);
    CHECK(status_or_reset(raw_status(port, head(method, path, big, f.cookie())), 413));
  }
  // a small upload without a session: read, then 401
  CHECK(raw_status(port, head("POST", "/api/stereotool/presets", 2, "") + "{}") == 401);
  // the session is checked when the headers come: an upload over 1 MB after logout is refused
  {
    Fixture g(dir.path + "-2");
    g.setup_and_login();
    const std::string cookie = g.cookie();
    REQUIRE(g.client->Post("/api/logout", g.auth, "", "application/json")->status == 200);
    const RawExchange x = raw_exchange(g.server->port(), head("POST", "/api/stereotool/presets", big, cookie));
    CHECK(statuses_or_reset(x.statuses, 401));
    std::error_code ec;
    fs::remove_all(dir.path + "-2", ec);
  }

  // requests hidden in an upload body, beyond the old 1 MB mark too, are never executed (each copy
  // an authenticated change that would set the PTP domain to 99)
  const std::string cookie = f.cookie();
  auto smuggled = [&](int) {
    const std::string body = R"({"ptp_domain":99})";
    return "PUT /api/config HTTP/1.0\r\nHost: 127.0.0.1\r\nCookie: " + cookie + "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body + "\r\n";
  };
  const std::vector<size_t> offsets = {4096, 1024 * 1024, 1024 * 1024 + 4096, 3 * 1024 * 1024};
  for (size_t offset : offsets) {
    CAPTURE(offset);
    // (a) a body that is not JSON (text/plain): read, 415, closed
    RawExchange x = raw_exchange(port,
                                 hiding_request_with([&](size_t n) { return head("POST", "/api/stereotool/presets", n, cookie, "text/plain"); },
                                                     offset, smuggled, kLastRequest),
                                 10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 415));  // the request after it is left unread (one per connection)
    // (b) JSON type, not JSON: read, 400, closed
    x = raw_exchange(port, hiding_request_with([&](size_t n) { return head("POST", "/api/stereotool/presets", n, cookie); }, offset, smuggled,
                                               kLastRequest),
                     10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 400));
    // (c) without a session: over 1 MB refused unread, under it read; 401 and closed either way
    x = raw_exchange(port, hiding_request_with([&](size_t n) { return head("POST", "/api/stereotool/presets", n, ""); }, offset, smuggled,
                                               kLastRequest),
                     10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 401));
    // (d) chunked, (e) two lengths, (f) an encoding, (g) multipart: refused on the headers
    const auto with = [&](const std::string& extra, const std::string& type = "application/json") {
      return [&, extra, type](size_t n) {
        return "POST /api/stereotool/presets HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " + cookie + "\r\nContent-Type: " + type + "\r\n" +
               extra + "Content-Length: " + std::to_string(n) + "\r\n\r\n";
      };
    };
    x = raw_exchange(port, hiding_request_with(with("Transfer-Encoding: chunked\r\n"), offset, smuggled, kLastRequest), 10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 411));
    x = raw_exchange(port, hiding_request_with(with("Content-Length: 5\r\n"), offset, smuggled, kLastRequest), 10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 400));
    x = raw_exchange(port, hiding_request_with(with("Content-Encoding: gzip\r\n"), offset, smuggled, kLastRequest), 10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 415));
    x = raw_exchange(port, hiding_request_with(with("", "multipart/form-data; boundary=xyz"), offset, smuggled, kLastRequest), 10000);
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 415));
    CHECK(ptp_domain() == 0);
  }
  // (h) pipelining after an accepted upload: only the first request is answered
  {
    const std::string body = upload_body("Pierwszy.sts", "dane").dump();
    const std::string second = upload_body("Drugi.sts", "dane").dump();
    const RawExchange x = raw_exchange(port, head("POST", "/api/stereotool/presets", body.size(), cookie) + body +
                                                 head("POST", "/api/stereotool/presets", second.size(), cookie) + second + smuggled(0));
    CHECK(x.closed);
    CHECK(statuses_or_reset(x.statuses, 200));
    const json presets = f.get("/api/stereotool/presets")["presets"];
    CHECK(presets.dump().find("Pierwszy.sts") != std::string::npos);
    CHECK(presets.dump().find("Drugi.sts") == std::string::npos);
  }
  // (i) no Content-Length, a Transfer-Encoding of any kind (identity too): 411 on the headers
  CHECK(status_or_reset(raw_status(port, "POST /api/stereotool/presets HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " + cookie +
                                             "\r\nContent-Type: application/json\r\n\r\n{}"),
                        411));
  CHECK(status_or_reset(raw_status(port, "POST /api/stereotool/presets HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: " + cookie +
                                             "\r\nContent-Type: application/json\r\nTransfer-Encoding: identity\r\nContent-Length: 2\r\n\r\n{}"),
                        411));
  // (j) Expect: 100-continue: a refusal comes instead of the 100, so the client never sends the body
  auto expect = [&](const std::string& c, size_t n) {
    return "POST /api/stereotool/presets HTTP/1.1\r\nHost: 127.0.0.1\r\n" + (c.empty() ? std::string() : "Cookie: " + c + "\r\n") +
           "Content-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " + std::to_string(n) + "\r\n\r\n";
  };
  {
    RawExchange x = raw_exchange(port, expect("", big));
    CHECK(x.closed);
    CHECK(x.statuses == std::vector<int>{401});
    x = raw_exchange(port, expect(cookie, be::kMaxPresetUploadBytes + 1));
    CHECK(x.closed);
    CHECK(x.statuses == std::vector<int>{413});
    const std::string body = upload_body("Po 100.sts", "dane").dump();
    x = raw_exchange(port, expect(cookie, body.size()) + body);
    CHECK(x.closed);
    CHECK(x.statuses == std::vector<int>{100, 200});
    // screened twice (before the 100 and at routing), its security headers are sent once
    auto count = [&](const std::string& what) {
      size_t n = 0;
      for (size_t at = x.text.find(what); at != std::string::npos; at = x.text.find(what, at + 1)) ++n;
      return n;
    };
    CHECK(count("X-Frame-Options: DENY") == 1);
    CHECK(count("X-Content-Type-Options: nosniff") == 1);
    CHECK(count("Cache-Control: no-store") == 1);
  }
  // (k) the path spelled with %2F is the same route and the same limit; a different path is not
  {
    const std::string data(1'200'000, 'e');
    const std::string body = upload_body("Przez 2F.sts", data).dump();
    const RawExchange x = raw_exchange(port, head("POST", "/api%2Fstereotool%2Fpresets", body.size(), cookie) + body, 10000);
    CHECK(statuses_or_reset(x.statuses, 200));
    CHECK(status_or_reset(raw_status(port, head("POST", "/api%2Fstereotool%2Fpresets", big, "")), 401));
    CHECK(status_or_reset(raw_status(port, head("POST", "/api/stereotool/presets%2F", big, cookie)), 413));
    CHECK(status_or_reset(raw_status(port, head("POST", "/api/stereotool/presets%00", big, cookie)), 413));
  }
  CHECK(ptp_domain() == 0);
  f.get("/api/config");  // still answering
}

TEST_CASE("encoder HTTP: an id the client gives never reuses a deleted id or an id in another case (ruling S4)") {
  Fixture f;
  f.setup_and_login();
  auto ids = [&] {
    std::vector<std::string> out;
    for (const auto& st : f.engine.config().stations) out.push_back(st.id);
    return out;
  };
  REQUIRE(f.post("/api/stations", station_json("A", false, "haslo-a", "/a.mp3"))->status == 200);
  // s1 replaced in one update by a station that names itself "S1": a new station, a new id
  json cfg = f.get("/api/config");
  json other = station_json("B", false, "haslo-b", "/b.mp3");
  other["id"] = "S1";
  cfg["stations"] = json::array({other});
  auto r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(ids() == std::vector<std::string>{"s2"});
  // a deleted id given back explicitly: a new one instead; an id never used is kept
  cfg = f.get("/api/config");
  cfg["stations"][0]["streams"][0]["password"] = "haslo-b";
  for (const char* id : {"s1", "radio"}) {
    json st = station_json(std::string("Jawny ") + id, false, "haslo-c", std::string("/") + id + ".mp3");
    st["id"] = id;
    cfg["stations"].push_back(st);
  }
  r = f.put("/api/config", cfg);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  CHECK(ids() == std::vector<std::string>{"s2", "s3", "radio"});
  // a huge explicit number: numbering goes on past it and never comes back
  cfg = f.get("/api/config");
  json huge = station_json("Wielka", false, "haslo-d", "/d.mp3");
  huge["id"] = "s999999999";
  cfg["stations"] = json::array({huge});
  REQUIRE(f.put("/api/config", cfg)->status == 200);
  REQUIRE(f.post("/api/stations", station_json("Po niej", false, "haslo-e", "/e.mp3"))->status == 200);
  CHECK(ids() == std::vector<std::string>{"s999999999", "s1000000000"});
  REQUIRE(f.client->Delete("/api/stations/s1000000000", f.auth)->status == 200);
  REQUIRE(f.post("/api/stations", station_json("Kolejna", false, "haslo-f", "/f.mp3"))->status == 200);
  CHECK(ids() == std::vector<std::string>{"s999999999", "s1000000001"});
  // longer numbers too (ids are up to 32 characters)
  cfg = f.get("/api/config");
  json longer = station_json("Dluga", false, "haslo-g", "/g.mp3");
  longer["id"] = "s99999999999";
  cfg["stations"] = json::array({longer});
  REQUIRE(f.put("/api/config", cfg)->status == 200);
  REQUIRE(f.post("/api/stations", station_json("Za nia", false, "haslo-h", "/h.mp3"))->status == 200);
  CHECK(ids() == std::vector<std::string>{"s99999999999", "s100000000000"});
  // the largest number: what would come after it is refused, never wrapped back to a given id
  cfg = f.get("/api/config");
  json largest = station_json("Najwieksza", false, "haslo-i", "/i.mp3");
  largest["id"] = "s99999999999999999";
  cfg["stations"] = json::array({largest});
  REQUIRE(f.put("/api/config", cfg)->status == 200);
  r = f.post("/api/stations", station_json("Za nia", false, "haslo-j", "/j.mp3"));
  CHECK(r->status == 400);
  CHECK_MESSAGE(r->body.find("za duży numer") != std::string::npos, r->body);
  // and what to do about it (the panel shows the message as it is), naming the station that holds
  // the largest number and its Stereo Tool folder left behind by a new id
  CHECK_MESSAGE(r->body.find("next_station_number") != std::string::npos, r->body);
  CHECK_MESSAGE(json::parse(r->body)["error"].get<std::string>().find("„Najwieksza” z ręcznie nadanym identyfikatorem „s99999999999999999”") !=
                    std::string::npos,
                r->body);
  CHECK_MESSAGE(json::parse(r->body)["error"].get<std::string>().find("stereotool\\station-s99999999999999999") != std::string::npos,
                r->body);
  cfg = f.get("/api/config");
  cfg["stations"] = json::array({station_json("Zamiast", false, "haslo-k", "/k.mp3")});  // drops it, adds one
  r = f.put("/api/config", cfg);
  CHECK(r->status == 400);
  CHECK(ids() == std::vector<std::string>{"s99999999999999999"});
  for (const char* id : {"s100000000000000000", "s1234567890123456789"}) {
    json too_big = station_json("Za duza", false, "haslo-l", "/l.mp3");
    too_big["id"] = id;
    cfg = f.get("/api/config");
    cfg["stations"].push_back(too_big);
    CHECK(f.put("/api/config", cfg)->status == 400);
  }
}

TEST_CASE("encoder HTTP: deeply nested, container-heavy or very wide JSON is refused before it is parsed, even without a session") {
  Fixture f;
  const auto error_of = [](const httplib::Result& r) { return json::parse(r->body)["error"].get<std::string>(); };
  const auto login = [&](const std::string& body) { return f.client->Post("/api/login", body, "application/json"); };
  // a password is needed for /api/login to look at the body; /api/setup is the first one
  const std::string nested = std::string(500'000, '[') + std::string(500'000, ']');  // valid JSON, 1 MB
  const int64_t t0 = bp::monotonic_ns();
  auto r = f.client->Post("/api/setup", nested, "application/json");
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zagnieżdż") != std::string::npos, r->body);
  CHECK(json::parse(f.client->Get("/api/auth-state")->body)["password_set"] == false);
  f.setup_and_login();
  r = login(nested);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zagnieżdż") != std::string::npos, r->body);
  // objects too
  std::string objects;
  for (int i = 0; i < 40; ++i) objects += "{\"a\":";
  objects += "1" + std::string(40, '}');
  r = login(objects);
  REQUIRE(r);
  CHECK(r->status == 400);
  // many small containers (a flat list of empty arrays)
  std::string flat = "[";
  for (int i = 0; i < 200'000; ++i) flat += i ? ",[]" : "[]";
  flat += "]";
  r = login(flat);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zbyt wiele") != std::string::npos, r->body);
  // a flat list of 1 MB of numbers, and an object of 90 000 keys (parsed in quadratic time)
  std::string numbers = "[0";
  while (numbers.size() < 1'000'000) numbers += ",0";
  numbers += "]";
  r = login(numbers);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zbyt wiele wartości") != std::string::npos, r->body);
  std::string keys = "{";
  for (int i = 0; i < 90'000; ++i) keys += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
  keys += "}";
  const int64_t t1 = bp::monotonic_ns();
  r = login(keys);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK(bp::monotonic_ns() - t1 < 1'000'000'000);
  std::string wide = "{";  // under the value cap, over the members of one object
  for (int i = 0; i < 2'500; ++i) wide += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
  wide += "}";
  r = login(wide);
  REQUIRE(r);
  CHECK(r->status == 400);
  CHECK_MESSAGE(error_of(r).find("zbyt wiele pól") != std::string::npos, r->body);
  CHECK(bp::monotonic_ns() - t0 < 5'000'000'000);  // refused quickly: nothing was built
  // brackets, commas and colons in a string are not structure
  r = login(json{{"password", std::string(100'000, '[') + std::string(50'000, ',') + std::string(50'000, ':')}}.dump());
  REQUIRE(r);
  CHECK(r->status == 401);
  // 16 levels are fine, 17 are not
  const auto deep = [](int levels) {  // the object counts as one level
    return R"({"password":"zlehaslo123","x":)" + std::string(size_t(levels - 1), '[') + std::string(size_t(levels - 1), ']') + "}";
  };
  CHECK(login(deep(16))->status == 401);
  CHECK(login(deep(17))->status == 400);
  // the largest configuration there can be (16 stations, 3 inputs with SDP and 8 streams each)
  // still goes through, with room to spare under every cap
  auto cfg = f.get("/api/config");
  cfg["stations"] = json::array();
  for (int st = 0; st < be::kMaxStations; ++st) {
    json station = station_json("Radio " + std::to_string(st), false, "haslo123", "/r" + std::to_string(st) + "-0.mp3");
    station["inputs"] = json::array();
    for (int i = 0; i < be::kMaxInputsPerStation; ++i)
      station["inputs"].push_back({{"enabled", true},
                                   {"name", "Wejscie " + std::to_string(i)},
                                   {"sdp", loopback_sdp("Wejscie", "239.69.231." + std::to_string(st * 3 + i + 1), 15230)},
                                   {"gain_db", 0},
                                   {"delay_s", 0},
                                   {"latency_us", 20000}});
    const json stream = station["streams"][0];
    station["streams"] = json::array();
    for (int k = 0; k < be::kMaxStreamsPerStation; ++k) {
      json s2 = stream;
      s2["mount"] = "/r" + std::to_string(st) + "-" + std::to_string(k) + ".mp3";
      station["streams"].push_back(s2);
    }
    station["processing"] = {{"enabled", false}, {"preset", "Jakis preset.sts"}, {"license_key", ""}, {"web_port_slot", st}};
    cfg["stations"].push_back(station);
  }
  const std::string body = cfg.dump();
  CAPTURE(json_value_separators(body));
  CHECK(json_value_separators(body) * 3 < bam::http::kMaxJsonValues);  // three times the largest configuration fits
  r = f.put("/api/config", cfg);
  REQUIRE(r);
  CHECK_MESSAGE(r->status == 200, r->body.substr(0, 400));
  CHECK(f.engine.config().stations.size() == size_t(be::kMaxStations));
}

TEST_CASE("encoder HTTP: a login with a 100 KB password is answered within a second") {
  Fixture f;
  f.setup_and_login();
  const int64_t t0 = bp::monotonic_ns();
  auto r = f.client->Post("/api/login", json{{"password", std::string(100'000, 'x')}}.dump(), "application/json");
  REQUIRE(r);
  CHECK(r->status == 401);
  CHECK(bp::monotonic_ns() - t0 < 1'000'000'000);
}

#endif  // BAM_WITH_CODECS

TEST_CASE("encoder HTTP: AES67 output in the station API, the status and the SDP download") {
  Fixture f;
  f.setup_and_login();
  httplib::Client anon(kLoopback, f.server->port());

  json st = station_json("Radio Źródło", true, "zrodlo123");
  st["aes67_output"] = {{"enabled", false}, {"name", "Wyjście"}, {"address", "239.69.110.9"}, {"ptime_us", 4000}};
  auto r = f.post("/api/stations", st);
  REQUIRE_MESSAGE(r->status == 200, r->body);
  const std::string id = json::parse(r->body)["id"];

  SUBCASE("a PUT without aes67_output keeps the stored one, with it replaces it") {
    json body = station_json("Radio Źródło 2", true, "zrodlo123");
    r = f.put("/api/stations/" + id, body);
    REQUIRE_MESSAGE(r->status == 200, r->body);
    json a = f.file()["stations"][0]["aes67_output"];
    CHECK(a["name"] == "Wyjście");
    CHECK(a["address"] == "239.69.110.9");
    CHECK(a["ptime_us"] == 4000);
    CHECK(a["enabled"] == false);

    body["aes67_output"] = {{"enabled", false}, {"name", "Inne"}, {"address", "239.69.110.10"}, {"ptime_us", 1333}};
    r = f.put("/api/stations/" + id, body);
    REQUIRE_MESSAGE(r->status == 200, r->body);
    a = f.file()["stations"][0]["aes67_output"];
    CHECK(a["name"] == "Inne");
    CHECK(a["address"] == "239.69.110.10");
    CHECK(a["ptime_us"] == 1333);
  }

  SUBCASE("output off: status says off, the SDP is refused") {
    json s;
    REQUIRE(wait_for([&] { s = f.get("/api/status"); return s["stations"].size() == 1 && s["stations"][0]["running"] == true; }, 10000));
    CHECK(s["stations"][0]["aes67"]["state"] == "off");

    CHECK(anon.Get(("/api/stations/" + id + "/aes67.sdp").c_str())->status == 401);
    CHECK(f.client->Get("/api/stations/nope/aes67.sdp", f.auth)->status == 404);
    auto sdp = f.client->Get("/api/stations/" + id + "/aes67.sdp", f.auth);
    REQUIRE(sdp);
    CHECK(sdp->status == 409);
    const json e = json::parse(sdp->body);
    CHECK(e["code"] == "not_running");
    CHECK(!e["error"].get<std::string>().empty());
  }

  SUBCASE("output on: full status object and the SDP file") {
    st["aes67_output"]["enabled"] = true;
    r = f.put("/api/stations/" + id, st);
    REQUIRE_MESSAGE(r->status == 200, r->body);
    json a;
    REQUIRE(wait_for(
        [&] {
          a = f.get("/api/status")["stations"][0]["aes67"];
          return a["state"] == "sending";
        },
        10000));
    for (const char* k : {"state", "reason", "address", "name", "ptime_us", "output_latency_ms", "packets", "gaps", "send_errors", "conflict"})
      CHECK_MESSAGE(a.contains(k), k);
    CHECK(a["address"] == "239.69.110.9");
    CHECK(a["name"] == "Wyjście");
    CHECK(a["ptime_us"] == 4000);

    auto sdp = f.client->Get("/api/stations/" + id + "/aes67.sdp", f.auth);
    REQUIRE(sdp);
    CHECK_MESSAGE(sdp->status == 200, sdp->body);
    CHECK(sdp->get_header_value("Content-Type").rfind("application/sdp", 0) == 0);
    CHECK(sdp->get_header_value("Content-Disposition") == "attachment; filename=\"Radio Zrodlo.sdp\"");
    CHECK(sdp->body.rfind("v=0", 0) == 0);
  }
}
