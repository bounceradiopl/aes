// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder HTTP API (spec §7). Every server here listens on 127.0.0.1 on a free port; the
// engine runs without PTP on the loopback interface, and the only stream server is 127.0.0.1.
#ifdef BAM_WITH_CODECS
#include <doctest/doctest.h>
#include <httplib.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

#include "bam/auth.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/encoder/http_server.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "bam/sdp.hpp"
#include "test_util.hpp"

namespace be = bam::encoder;
namespace bp = bam::platform;
using json = nlohmann::json;

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

bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
  const int64_t end = bp::monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (bp::monotonic_ns() < end) {
    if (pred()) return true;
    bp::sleep_ns(20'000'000);
  }
  return pred();
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
  CHECK(h["version"] == "1.0.0");

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

  // a body over 1 MB is refused before it is parsed
  const std::string huge(be::kMaxApiBodyBytes + 1, ' ');
  r = f.client->Put("/api/config", f.auth, huge, "application/json");
  REQUIRE(r);
  CHECK(r->status == 413);
  CHECK(json::parse(r->body)["error"].get<std::string>().find("za duża") != std::string::npos);
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
  CHECK(s["version"] == "1.0.0");
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
#endif  // BAM_WITH_CODECS
