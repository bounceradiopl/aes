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
#include "bam/encoder/paths.hpp"
#include "bam/http/guard.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"
#include "bam/sdp.hpp"
#include "http_attack.hpp"
#include "test_util.hpp"

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

struct Fixture {
  std::string config_path = unique_path("http");
  be::EncoderEngine engine{[] {
    be::EncoderEngineOptions o;
    o.enable_ptp = false;
    return o;
  }()};
  std::unique_ptr<be::EncoderHttpServer> server;
  std::unique_ptr<httplib::Client> client;
  httplib::Headers auth;

  Fixture() {
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
  CHECK(h["version"] == "1.0.3");

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
  CHECK(s["version"] == "1.0.3");
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
