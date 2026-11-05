// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder configuration (spec §7): JSON round trip, validation of the limits (§2) and the
// codec table (§5.2), normalisation, password masking and merging, the configuration file.
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bam/encoder/config.hpp"
#include "bam/platform/process.hpp"
#include "test_util.hpp"

namespace be = bam::encoder;

namespace {

be::InputConfig orban_input() {
  be::InputConfig in;
  in.sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  in.latency_us = 5000;
  return in;
}

be::StreamConfig stream(std::string id, be::Codec codec, uint32_t kbps, uint32_t rate, std::string mount) {
  be::StreamConfig s;
  s.id = std::move(id);
  s.codec = codec;
  s.bitrate_kbps = kbps;
  s.sample_rate = rate;
  s.host = "127.0.0.1";
  s.port = 8000;
  s.mount = std::move(mount);
  s.user = "source";
  s.password = "tajne-" + s.id;
  return s;
}

// A valid configuration: "Radio Bounce" with one input and two streams.
be::EncoderConfig sample() {
  auto c = be::default_encoder_config();
  be::StationConfig st;
  st.id = "s1";
  st.name = "Radio Bounce";
  st.inputs.push_back(orban_input());
  st.streams.push_back(stream("str1", be::Codec::Mp3, 128, 44100, "/radio.mp3"));
  st.streams.push_back(stream("str2", be::Codec::AacLc, 96, 48000, "/radio.aac"));
  c.stations.push_back(st);
  return c;
}

const be::EncoderConfigIssue* find_issue(const std::vector<be::EncoderConfigIssue>& issues, const std::string& field) {
  for (const auto& i : issues)
    if (i.field == field) return &i;
  return nullptr;
}

std::string dump(const std::vector<be::EncoderConfigIssue>& issues) {
  std::string s;
  for (const auto& i : issues) s += i.field + ": " + i.message + "\n";
  return s;
}

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

bool valid_utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

}  // namespace

TEST_CASE("encoder config: defaults and JSON round trip") {
  auto d = be::default_encoder_config();
  CHECK(d.http_port == 8070);
  CHECK(d.http_bind == "0.0.0.0");
  CHECK(d.ptp_domain == 0);
  CHECK(d.stations.empty());
  CHECK(be::validate_config(d).empty());
  CHECK(contains(be::config_to_json(d), "\"version\": 1"));

  auto c = sample();
  c.interface_ip = "127.0.0.1";
  c.ptp_domain = 3;
  c.password_hash = "pbkdf2-sha256$1$aa$bb";
  c.stations[0].inputs[0].gain_db = -6.5;
  c.stations[0].inputs[0].delay_s = 2.3;
  c.stations[0].inputs[0].name = "Studio";
  c.stations[0].streams[1].enabled = false;
  c.stations[0].streams[1].gain_db = 1.5;
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
  std::string err;
  auto back = be::config_from_json(be::config_to_json(c), &err);
  REQUIRE_MESSAGE(back, err);
  CHECK(be::config_to_json(*back) == be::config_to_json(c));
  CHECK(back->interface_ip == "127.0.0.1");
  CHECK(back->ptp_domain == 3);
  CHECK(back->password_hash == c.password_hash);
  REQUIRE(back->stations.size() == 1);
  const auto& st = back->stations[0];
  CHECK(st.id == "s1");
  CHECK(st.name == "Radio Bounce");
  REQUIRE(st.inputs.size() == 1);
  CHECK(st.inputs[0].sdp == c.stations[0].inputs[0].sdp);
  CHECK(st.inputs[0].gain_db == doctest::Approx(-6.5));
  CHECK(st.inputs[0].delay_s == doctest::Approx(2.3));
  REQUIRE(st.streams.size() == 2);
  CHECK(st.streams[1].id == "str2");
  CHECK(st.streams[1].codec == be::Codec::AacLc);
  CHECK(st.streams[1].bitrate_kbps == 96);
  CHECK(st.streams[1].sample_rate == 48000);
  CHECK_FALSE(st.streams[1].enabled);
  CHECK(st.streams[0].password == "tajne-str1");
  CHECK(st.streams[0].mount == "/radio.mp3");
}

TEST_CASE("encoder config: the spec example parses") {
  const std::string text = R"({
    "version": 1,
    "interface_ip": "", "ptp_domain": 0, "http_bind": "0.0.0.0", "http_port": 8070, "password_hash": "",
    "stations": [
      { "id": "s1", "name": "Radio Bounce", "enabled": true,
        "inputs": [ { "enabled": true, "name": "", "sdp": "v=0", "gain_db": 0, "delay_s": 0.0, "latency_us": 5000 } ],
        "streams": [ { "enabled": true, "codec": "mp3", "bitrate_kbps": 128, "sample_rate": 44100, "gain_db": 0,
                       "host": "stream.example", "port": 8000, "mount": "/radio.mp3", "user": "source", "password": "x" } ] } ]
  })";
  std::string err;
  auto c = be::config_from_json(text, &err);
  REQUIRE_MESSAGE(c, err);
  REQUIRE(c->stations.size() == 1);
  REQUIRE(c->stations[0].streams.size() == 1);
  CHECK(c->stations[0].streams[0].id.empty());  // the example has no stream ids: normalize assigns them
  CHECK(c->stations[0].streams[0].host == "stream.example");
  be::normalize_config(*c);
  CHECK(c->stations[0].streams[0].id == "str1");
}

TEST_CASE("encoder config: parse errors are Polish and say where") {
  std::string err;
  CHECK_FALSE(be::config_from_json("[]", &err));
  CHECK(contains(err, "obiekt"));
  CHECK_FALSE(be::config_from_json("{nie json", &err));
  CHECK(contains(err, "nieprawidłowa konfiguracja"));
  CHECK_FALSE(be::config_from_json(R"({"version": 2})", &err));
  CHECK(contains(err, "nowszej wersji"));
  CHECK_FALSE(be::config_from_json(R"({"version": 0})", &err));
  CHECK(contains(err, "version"));
  CHECK_FALSE(be::config_from_json(R"({"version": -1})", &err));
  CHECK(contains(err, "version"));

  auto c = sample();
  c.stations[0].name = "Radio Bounce";
  std::string text = be::config_to_json(c);
  const std::string aac = "\"codec\": \"aac\"";
  REQUIRE(contains(text, aac));
  text.replace(text.find(aac), aac.size(), "\"codec\": \"opus\"");
  CHECK_FALSE(be::config_from_json(text, &err));
  CHECK_MESSAGE(contains(err, "Stacja „Radio Bounce”, strumień 2"), err);
  CHECK(contains(err, "opus"));
  CHECK(contains(err, "(dozwolone: mp3, aac, he-aac, he-aac-v2, flac)"));

  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":[{"name":"A","streams":[{"port":"8000"}]}]})", &err));
  CHECK_MESSAGE(contains(err, "stations[0].streams[0].port"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":[{"name":"A","streams":[{"bitrate_kbps":-1}]}]})", &err));
  CHECK_MESSAGE(contains(err, "stations[0].streams[0].bitrate_kbps"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":{}})", &err));
  CHECK(contains(err, "stations"));
}

TEST_CASE("encoder config: limits of stations, inputs and streams") {
  auto c = sample();
  for (int i = 2; i <= 17; ++i) {
    be::StationConfig st;
    st.id = "s" + std::to_string(i);
    st.name = "Stacja " + std::to_string(i);
    c.stations.push_back(st);
  }
  auto issues = be::validate_config(c);
  REQUIRE_MESSAGE(find_issue(issues, "stations"), dump(issues));
  CHECK(contains(find_issue(issues, "stations")->message, "16"));
  c.stations.pop_back();
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));

  c = sample();
  for (int i = 0; i < 3; ++i) c.stations[0].inputs.push_back(orban_input());
  issues = be::validate_config(c);
  REQUIRE_MESSAGE(find_issue(issues, "stations[0].inputs"), dump(issues));
  CHECK(contains(find_issue(issues, "stations[0].inputs")->message, "Stacja „Radio Bounce”"));
  CHECK(contains(find_issue(issues, "stations[0].inputs")->message, "3"));
  c.stations[0].inputs.pop_back();
  CHECK(be::validate_config(c).empty());

  c = sample();
  for (int i = 3; i <= 9; ++i)
    c.stations[0].streams.push_back(stream("str" + std::to_string(i), be::Codec::Flac, 0, 48000, "/f" + std::to_string(i)));
  issues = be::validate_config(c);
  REQUIRE_MESSAGE(find_issue(issues, "stations[0].streams"), dump(issues));
  CHECK(contains(find_issue(issues, "stations[0].streams")->message, "8"));
  c.stations[0].streams.pop_back();
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
}

TEST_CASE("encoder config: input gain, delay, latency and SDP") {
  auto check = [](auto edit, const std::string& field, const std::string& text) {
    auto c = sample();
    edit(c.stations[0].inputs[0]);
    auto issues = be::validate_config(c);
    const auto* i = find_issue(issues, field);
    REQUIRE_MESSAGE(i, (field + "\n" + dump(issues)));
    CHECK_MESSAGE(contains(i->message, "Stacja „Radio Bounce”, wejście 1: "), i->message);
    CHECK_MESSAGE(contains(i->message, text), i->message);
  };
  check([](be::InputConfig& in) { in.gain_db = -60.5; }, "stations[0].inputs[0].gain_db", "−60…+12 dB");
  check([](be::InputConfig& in) { in.gain_db = 12.5; }, "stations[0].inputs[0].gain_db", "−60…+12 dB");
  check([](be::InputConfig& in) { in.delay_s = -0.1; }, "stations[0].inputs[0].delay_s", "0–60 s");
  check([](be::InputConfig& in) { in.delay_s = 60.1; }, "stations[0].inputs[0].delay_s", "0–60 s");
  check([](be::InputConfig& in) { in.latency_us = 999; }, "stations[0].inputs[0].latency_us", "1–1000 ms");
  check([](be::InputConfig& in) { in.latency_us = 2000; }, "stations[0].inputs[0].latency_us", "ptime");
  check([](be::InputConfig& in) { in.sdp.clear(); }, "stations[0].inputs[0].sdp", "SDP");
  check([](be::InputConfig& in) { in.sdp = "v=0\r\ns=nic\r\n"; }, "stations[0].inputs[0].sdp", "SDP: brak sekcji m=audio");
  check([](be::InputConfig& in) { in.sdp += std::string(64 * 1024, ' '); }, "stations[0].inputs[0].sdp", "64 KB");
  check([](be::InputConfig& in) { in.enabled = false; in.sdp = std::string(64 * 1024 + 1, 'x'); }, "stations[0].inputs[0].sdp", "64 KB");

  // the limits themselves are allowed
  auto c = sample();
  c.stations[0].inputs[0].gain_db = -60;
  c.stations[0].inputs[0].delay_s = 60;
  c.stations[0].streams[0].gain_db = 12;
  CHECK(be::validate_config(c).empty());

  // a disabled input may be left without an SDP (still range checked)
  c = sample();
  c.stations[0].inputs[0].enabled = false;
  c.stations[0].inputs[0].sdp.clear();
  CHECK(be::validate_config(c).empty());
  c.stations[0].inputs[0].gain_db = 20;
  CHECK(find_issue(be::validate_config(c), "stations[0].inputs[0].gain_db"));

  // unnamed station: its position names it
  c = sample();
  c.stations[0].name.clear();
  c.stations[0].inputs[0].gain_db = 20;
  const auto issues = be::validate_config(c);
  const auto* i = find_issue(issues, "stations[0].inputs[0].gain_db");
  REQUIRE(i);
  CHECK_MESSAGE(contains(i->message, "Stacja 1, wejście 1: "), i->message);
}

TEST_CASE("encoder config: stream codec settings use the encoder's own validation") {
  struct Case {
    be::Codec codec;
    uint32_t kbps, rate;
    bool ok;
  };
  const Case cases[] = {
      {be::Codec::Mp3, 320, 48000, true},     {be::Codec::Mp3, 160, 24000, true},
      {be::Codec::Mp3, 192, 24000, false},    // MPEG-2 tops out at 160 kb/s
      {be::Codec::Mp3, 130, 44100, false},    // not a standard step
      {be::Codec::Mp3, 128, 16000, false},    // not a stream rate
      {be::Codec::AacLc, 320, 48000, true},   {be::Codec::AacLc, 288, 24000, true},
      {be::Codec::AacLc, 289, 24000, false},  {be::Codec::AacLc, 265, 22050, false},
      {be::Codec::AacLc, 31, 48000, false},   {be::Codec::HeAac, 64, 32000, true},
      {be::Codec::HeAac, 64, 24000, false},   {be::Codec::HeAac, 129, 48000, false},
      {be::Codec::HeAacV2, 32, 44100, true},  {be::Codec::HeAacV2, 65, 44100, false},
      {be::Codec::HeAacV2, 32, 22050, false}, {be::Codec::Flac, 0, 22050, true},
      {be::Codec::Flac, 9999, 32000, true},   {be::Codec::Flac, 0, 96000, false},
  };
  for (const auto& k : cases) {
    auto c = sample();
    auto& s = c.stations[0].streams[1];
    s.codec = k.codec;
    s.bitrate_kbps = k.kbps;
    s.sample_rate = k.rate;
    auto issues = be::validate_config(c);
    CAPTURE(be::codec_name(k.codec));
    CAPTURE(k.kbps);
    CAPTURE(k.rate);
    CHECK_MESSAGE(issues.empty() == k.ok, dump(issues));
    if (!k.ok) {
      REQUIRE(issues.size() == 1);
      std::string expected;
      CHECK_FALSE(be::validate_encoder_settings({k.codec, k.kbps, k.rate, 2}, &expected));
      CHECK(issues[0].field == "stations[0].streams[1]");
      CHECK(issues[0].message == "Stacja „Radio Bounce”, strumień 2: " + expected);
    }
    // a disabled stream may hold any combination
    s.enabled = false;
    CHECK(be::validate_config(c).empty());
  }
}

TEST_CASE("encoder config: stream target, gain and ids") {
  auto check = [](auto edit, const std::string& field, const std::string& text) {
    auto c = sample();
    edit(c.stations[0].streams[1]);
    auto issues = be::validate_config(c);
    const auto* i = find_issue(issues, field);
    REQUIRE_MESSAGE(i, (field + "\n" + dump(issues)));
    CHECK_MESSAGE(contains(i->message, "Stacja „Radio Bounce”, strumień 2: "), i->message);
    CHECK_MESSAGE(contains(i->message, text), i->message);
    // a disabled stream keeps only the gain and id checks
    edit(c.stations[0].streams[1]);
    c.stations[0].streams[1].enabled = false;
    return find_issue(be::validate_config(c), field) != nullptr;
  };
  CHECK_FALSE(check([](be::StreamConfig& s) { s.host.clear(); }, "stations[0].streams[1].host", "serwer"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.host = "radio example"; }, "stations[0].streams[1].host", "serwer"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.port = 0; }, "stations[0].streams[1].port", "1–65535"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.port = 65536; }, "stations[0].streams[1].port", "1–65535"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.mount = "radio.aac"; }, "stations[0].streams[1].mount", "/"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.mount = "/"; }, "stations[0].streams[1].mount", "/"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.mount = "/a b"; }, "stations[0].streams[1].mount", "/"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.host = "radio.example:8000"; }, "stations[0].streams[1].host",
                    "port wpisz w osobnym polu"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.host = "radió.pl"; }, "stations[0].streams[1].host", "serwer"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.host = "a/b"; }, "stations[0].streams[1].host", "serwer"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.mount = "/radio?x=1"; }, "stations[0].streams[1].mount", "/"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.mount = "/radio#x"; }, "stations[0].streams[1].mount", "/"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.user = "a:b"; }, "stations[0].streams[1].user", ":"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.user = "ab\x01"; }, "stations[0].streams[1].user", "sterujących"));
  // lengths are bounded whether the stream is enabled or not
  CHECK(check([](be::StreamConfig& s) { s.mount = "/" + std::string(255, 'm'); }, "stations[0].streams[1].mount", "255"));
  CHECK(check([](be::StreamConfig& s) { s.user = std::string(256, 'u'); }, "stations[0].streams[1].user", "255"));
  CHECK(check([](be::StreamConfig& s) { s.password = std::string(256, 'p'); }, "stations[0].streams[1].password", "255"));
  CHECK(check([](be::StreamConfig& s) { s.host = std::string(254, 'h'); }, "stations[0].streams[1].host", "253"));
  CHECK_FALSE(check([](be::StreamConfig& s) { s.password.clear(); }, "stations[0].streams[1].password", "hasło"));
  CHECK(check([](be::StreamConfig& s) { s.gain_db = -61; }, "stations[0].streams[1].gain_db", "−60…+12 dB"));
  CHECK(check([](be::StreamConfig& s) { s.gain_db = 13; }, "stations[0].streams[1].gain_db", "−60…+12 dB"));
  CHECK(check([](be::StreamConfig& s) { s.id = "str1"; }, "stations[0].streams[1].id", "str1"));
  CHECK(check([](be::StreamConfig& s) { s.id = "a/b"; }, "stations[0].streams[1].id", "identyfikator"));

  // the limits are allowed; an empty user means the Icecast default
  auto c = sample();
  c.stations[0].streams[0].port = 1;
  c.stations[0].streams[1].port = 65535;
  c.stations[0].streams[1].user.clear();
  c.stations[0].streams[1].host = "stream_1.example-radio.com";
  c.stations[0].streams[0].mount = "/" + std::string(254, 'm');
  c.stations[0].streams[0].user = std::string(255, 'u');
  c.stations[0].streams[0].password = std::string(255, 'p');
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
  c.stations[0].streams[0].host = "10.0.0.5";
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
}

TEST_CASE("encoder config: two running streams may not share a server mountpoint") {
  auto c = sample();
  be::StationConfig other;
  other.id = "s2";
  other.name = "Bounce Dwa";
  other.streams.push_back(stream("str1", be::Codec::Flac, 0, 48000, "/radio.mp3"));
  c.stations.push_back(other);
  auto issues = be::validate_config(c);
  const auto* i = find_issue(issues, "stations[1].streams[0].mount");
  REQUIRE_MESSAGE(i, dump(issues));
  CHECK_MESSAGE(contains(i->message, "Stacja „Bounce Dwa”, strumień 1: "), i->message);
  CHECK_MESSAGE(contains(i->message, "stacja „Radio Bounce”, strumień 1"), i->message);

  c.stations[1].streams[0].port = 8001;  // another server
  CHECK(be::validate_config(c).empty());
  c.stations[1].streams[0].port = 8000;
  c.stations[1].streams[0].host = "LOCALHOST";
  c.stations[0].streams[0].host = "localhost";  // host names compare case-insensitively
  CHECK_FALSE(be::validate_config(c).empty());
  c.stations[1].enabled = false;  // a disabled station does not stream
  CHECK(be::validate_config(c).empty());
}

TEST_CASE("encoder config: station ids, names and global settings") {
  auto c = sample();
  be::StationConfig dup;
  dup.id = "s1";
  dup.name = "Kopia";
  c.stations.push_back(dup);
  auto issues = be::validate_config(c);
  const be::EncoderConfigIssue* i = find_issue(issues, "stations[1].id");
  REQUIRE_MESSAGE(i, dump(issues));
  CHECK(contains(i->message, "Stacja „Kopia”"));
  CHECK(contains(i->message, "„Radio Bounce”"));

  c = sample();
  c.stations[0].id.clear();
  issues = be::validate_config(c);
  i = find_issue(issues, "stations[0].id");
  REQUIRE_MESSAGE(i, dump(issues));
  CHECK_MESSAGE(contains(i->message, "Stacja „Radio Bounce”: brak identyfikatora"), i->message);
  be::normalize_config(c);
  CHECK(c.stations[0].id == "s1");
  CHECK(be::validate_config(c).empty());

  c = sample();
  c.stations[0].id = "s 1";
  CHECK(find_issue(be::validate_config(c), "stations[0].id"));
  c.stations[0].id = std::string(33, 'a');
  CHECK(find_issue(be::validate_config(c), "stations[0].id"));
  c.stations[0].id = "Studio_2-B";
  CHECK(be::validate_config(c).empty());

  c = sample();
  c.stations[0].name = "Radio\r\nX-Evil: 1";
  CHECK(find_issue(be::validate_config(c), "stations[0].name"));
  c.stations[0].name = std::string(65, 'x');
  CHECK(find_issue(be::validate_config(c), "stations[0].name"));

  c = sample();
  c.interface_ip = "10.0.0";
  c.http_bind = "";
  c.http_port = 0;
  c.ptp_domain = 128;
  issues = be::validate_config(c);
  CHECK(find_issue(issues, "interface_ip"));
  CHECK(find_issue(issues, "http_bind"));
  CHECK(find_issue(issues, "http_port"));
  CHECK(find_issue(issues, "ptp_domain"));
  c.http_port = 65536;
  CHECK(find_issue(be::validate_config(c), "http_port"));
}

TEST_CASE("encoder config: normalize rounds steps, raises latency and assigns ids") {
  auto c = sample();
  auto& in = c.stations[0].inputs[0];
  in.gain_db = -6.3;
  in.delay_s = 2.34;
  in.latency_us = 1500;  // the Orban source has ptime 1.333 ms
  c.stations[0].streams[0].gain_db = 1.26;
  c.stations[0].streams[1].gain_db = -0.74;
  auto notices = be::normalize_config(c);
  CHECK(in.gain_db == -6.5);
  CHECK(in.delay_s == doctest::Approx(2.3).epsilon(1e-12));
  CHECK(in.latency_us == 2334);
  CHECK(in.name == "Optimod_114969_2");  // named after the SDP session
  CHECK(c.stations[0].streams[0].gain_db == 1.5);
  CHECK(c.stations[0].streams[1].gain_db == -0.5);
  REQUIRE(notices.size() == 1);
  CHECK_MESSAGE(contains(notices[0], "Stacja „Radio Bounce”, wejście 1"), notices[0]);
  CHECK(be::validate_config(c).empty());
  CHECK(be::normalize_config(c).empty());  // idempotent

  // ids: the smallest free number; existing ids are kept
  c = sample();
  c.stations[0].id = "s2";
  c.stations[0].streams[0].id = "str2";
  c.stations[0].streams[1].id.clear();
  be::StationConfig a, b;
  a.name = "A";
  b.name = "B";
  b.streams.push_back(stream("", be::Codec::Flac, 0, 48000, "/b"));
  c.stations.push_back(a);
  c.stations.push_back(b);
  be::normalize_config(c);
  CHECK(c.stations[0].id == "s2");
  CHECK(c.stations[1].id == "s1");
  CHECK(c.stations[2].id == "s3");
  CHECK(c.stations[0].streams[0].id == "str2");
  CHECK(c.stations[0].streams[1].id == "str1");
  CHECK(c.stations[2].streams[0].id == "str1");
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));

  // gaps are filled first, then numbering continues past the highest id
  c = be::default_encoder_config();
  for (const char* id : {"s1", "", "s3", "", ""}) {
    be::StationConfig st;
    st.id = id;
    for (const char* sid : {"", "str2", "", ""}) st.streams.push_back(stream(sid, be::Codec::Flac, 0, 48000, "/x"));
    c.stations.push_back(st);
  }
  be::normalize_config(c);
  std::vector<std::string> ids, stream_ids;
  for (const auto& st : c.stations) ids.push_back(st.id);
  for (const auto& s : c.stations[1].streams) stream_ids.push_back(s.id);
  CHECK(ids == std::vector<std::string>{"s1", "s2", "s3", "s4", "s5"});
  CHECK(stream_ids == std::vector<std::string>{"str1", "str2", "str3", "str4"});
}

TEST_CASE("encoder config: an input named after its SDP gets a valid name") {
  // 80+ bytes of session name with a tab in it and two-byte characters across the 64-byte cut
  std::string session = "Studio\tA ";
  while (session.size() < 90) session += "żółć ";
  std::string sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  const std::string orig = "s=Optimod_114969_2";
  REQUIRE(contains(sdp, orig));
  sdp.replace(sdp.find(orig), orig.size(), "s=" + session);
  auto c = sample();
  c.stations[0].inputs[0].sdp = sdp;
  c.stations[0].inputs[0].name.clear();
  be::normalize_config(c);
  const std::string& name = c.stations[0].inputs[0].name;
  CAPTURE(name);
  CHECK(name.size() <= 64);
  CHECK(name.size() >= 60);
  CHECK(name.find('\t') == std::string::npos);
  CHECK(name.rfind("StudioA ", 0) == 0);
  CHECK(valid_utf8(name));  // cut on a whole character
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
}

TEST_CASE("encoder config: oversized arrays are refused at parse time") {
  auto json_with = [](int stations, int inputs, int streams) {
    std::string t = R"({"version":1,"stations":[)";
    for (int i = 0; i < stations; ++i) {
      if (i) t += ",";
      t += R"({"inputs":[)";
      for (int k = 0; k < inputs; ++k) t += k ? ",{}" : "{}";
      t += R"(],"streams":[)";
      for (int k = 0; k < streams; ++k) t += k ? ",{}" : "{}";
      t += "]}";
    }
    return t + "]}";
  };
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(be::config_from_json(json_with(1, 0, 20000), &err));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 1000);  // generous for a loaded machine: it takes ~1 ms
  CHECK_MESSAGE(contains(err, "stations[0].streams"), err);
  CHECK_MESSAGE(contains(err, "32"), err);
  CHECK_FALSE(be::config_from_json(json_with(65, 0, 0), &err));
  CHECK_MESSAGE(contains(err, "stations"), err);
  CHECK_MESSAGE(contains(err, "64"), err);
  CHECK_FALSE(be::config_from_json(json_with(1, 13, 0), &err));
  CHECK_MESSAGE(contains(err, "stations[0].inputs"), err);
  CHECK_MESSAGE(contains(err, "12"), err);

  // a small overshoot parses, so validation can explain the real limit
  auto c = be::config_from_json(json_with(17, 4, 9), &err);
  REQUIRE_MESSAGE(c, err);
  be::normalize_config(*c);
  auto issues = be::validate_config(*c);
  CHECK(find_issue(issues, "stations"));
  CHECK(find_issue(issues, "stations[0].inputs"));
  CHECK(find_issue(issues, "stations[0].streams"));
  // the largest accepted input normalises quickly
  c = be::config_from_json(json_with(64, 12, 32), &err);
  REQUIRE_MESSAGE(c, err);
  const auto t1 = std::chrono::steady_clock::now();
  be::normalize_config(*c);
  const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  CHECK(ms2 < 1000);
  CHECK(c->stations[63].id == "s64");
  CHECK(c->stations[63].streams[31].id == "str32");
}

TEST_CASE("encoder config: the API form carries no secrets") {
  auto c = sample();
  c.password_hash = "pbkdf2-sha256$100000$abcd$ef01";
  c.stations[0].streams[1].password.clear();
  const auto masked = be::mask_passwords(c);
  CHECK(masked.password_hash.empty());
  for (const auto& s : masked.stations[0].streams) CHECK(s.password.empty());
  CHECK(c.stations[0].streams[0].password == "tajne-str1");  // the original is untouched

  const std::string api = be::config_to_api_json(c);
  CHECK_FALSE(contains(api, "tajne"));
  CHECK_FALSE(contains(api, "password_hash"));
  CHECK_FALSE(contains(api, "pbkdf2"));
  CHECK(contains(api, "\"password\": \"\""));
  std::string err;
  auto back = be::config_from_json(api, &err);  // the panel sends this form back
  REQUIRE_MESSAGE(back, err);
  CHECK(back->stations[0].streams[0].password.empty());
  CHECK(contains(api, "\"password_set\": true"));
  CHECK(contains(api, "\"password_set\": false"));
  CHECK(be::config_to_json(c).find("tajne-str1") != std::string::npos);  // the file keeps them
}

TEST_CASE("encoder config: merge_passwords keeps stored passwords by stream id") {
  const auto previous = sample();  // str1 "tajne-str1", str2 "tajne-str2"

  SUBCASE("a masked round trip keeps every password and the panel hash") {
    auto prev = previous;
    prev.password_hash = "pbkdf2-sha256$1$aa$bb";
    std::string err;
    auto next = be::config_from_json(be::config_to_api_json(prev), &err);
    REQUIRE(next);
    be::merge_passwords(*next, prev);
    CHECK(next->stations[0].streams[0].password == "tajne-str1");
    CHECK(next->stations[0].streams[1].password == "tajne-str2");
    CHECK(next->password_hash == prev.password_hash);
    CHECK(be::config_to_json(*next) == be::config_to_json(prev));
  }
  SUBCASE("a password_hash sent by the client is replaced by the stored one") {
    auto prev = previous;
    prev.password_hash = "pbkdf2-sha256$1$aa$bb";
    std::string err;
    auto next = be::config_from_json(R"({"version":1,"password_hash":"pbkdf2-sha256$1$00$11","stations":[]})", &err);
    REQUIRE_MESSAGE(next, err);
    CHECK(next->password_hash == "pbkdf2-sha256$1$00$11");
    be::merge_passwords(*next, prev);
    CHECK(next->password_hash == prev.password_hash);
    auto empty = be::default_encoder_config();
    empty.password_hash = "x";
    be::merge_passwords(empty, be::default_encoder_config());
    CHECK(empty.password_hash.empty());  // no hash stored: none can be set this way
  }
  SUBCASE("a new password replaces the stored one") {
    auto next = be::mask_passwords(previous);
    next.stations[0].streams[1].password = "nowe";
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].streams[0].password == "tajne-str1");
    CHECK(next.stations[0].streams[1].password == "nowe");
  }
  SUBCASE("deleting the first stream keeps the second stream's password") {
    auto next = be::mask_passwords(previous);
    next.stations[0].streams.erase(next.stations[0].streams.begin());
    be::merge_passwords(next, previous);
    REQUIRE(next.stations[0].streams.size() == 1);
    CHECK(next.stations[0].streams[0].id == "str2");
    CHECK(next.stations[0].streams[0].password == "tajne-str2");  // not str1's, which held position 1
  }
  SUBCASE("reordered streams and stations keep their own passwords") {
    auto prev = previous;
    be::StationConfig two;
    two.id = "s2";
    two.name = "Dwa";
    two.streams.push_back(stream("str1", be::Codec::Flac, 0, 48000, "/dwa"));
    two.streams[0].password = "haslo-dwa";
    prev.stations.push_back(two);
    auto next = be::mask_passwords(prev);
    std::swap(next.stations[0], next.stations[1]);
    std::swap(next.stations[1].streams[0], next.stations[1].streams[1]);
    be::merge_passwords(next, prev);
    CHECK(next.stations[0].streams[0].password == "haslo-dwa");
    CHECK(next.stations[1].streams[0].password == "tajne-str2");
    CHECK(next.stations[1].streams[1].password == "tajne-str1");
  }
  SUBCASE("a new stream (no id yet) gets no stored password") {
    auto next = be::mask_passwords(previous);
    next.stations[0].streams.erase(next.stations[0].streams.begin() + 1);
    next.stations[0].streams.push_back(stream("", be::Codec::Flac, 0, 48000, "/nowy"));
    next.stations[0].streams.back().password.clear();
    be::merge_passwords(next, previous);  // before normalize_config, which would name it str2
    CHECK(next.stations[0].streams[1].password.empty());
    be::normalize_config(next);
    CHECK(next.stations[0].streams[1].id == "str2");
    CHECK(find_issue(be::validate_config(next), "stations[0].streams[1].password"));
  }
  SUBCASE("a stream moved to another server needs its password again") {
    auto next = be::mask_passwords(previous);
    next.stations[0].streams[0].host = "inny.serwer";
    next.stations[0].streams[1].port = 8443;
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].streams[0].password.empty());
    CHECK(next.stations[0].streams[1].password.empty());
    auto issues = be::validate_config(next);
    CHECK(find_issue(issues, "stations[0].streams[0].password"));
    CHECK(find_issue(issues, "stations[0].streams[1].password"));
    // same server, other spelling of the host name case: still the same server
    next = be::mask_passwords(previous);
    auto prev = previous;
    prev.stations[0].streams[0].host = "Stream.Example";
    next.stations[0].streams[0].host = "stream.example";
    be::merge_passwords(next, prev);
    CHECK(next.stations[0].streams[0].password == "tajne-str1");
  }
  SUBCASE("a deleted station's passwords do not reach a station with another id") {
    auto next = be::mask_passwords(previous);
    next.stations[0].id = "s7";
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].streams[0].password.empty());
  }
}

TEST_CASE("encoder config: file save and load") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("bam-encoder-config-" + std::to_string(bam::platform::current_pid()));
  fs::remove_all(dir);
  const std::string path = (dir / "Bounce Encoder" / "config.json").string();

  std::string err;
  bool missing = false;
  CHECK_FALSE(be::load_config_file(path, &err, &missing));
  CHECK(missing);

  auto c = sample();
  c.password_hash = "pbkdf2-sha256$1$aa$bb";
  REQUIRE_MESSAGE(be::save_config_file(path, c, &err), err);
  CHECK_FALSE(fs::exists(path + ".tmp"));
  auto back = be::load_config_file(path, &err, &missing);
  REQUIRE_MESSAGE(back, err);
  CHECK_FALSE(missing);
  CHECK(be::config_to_json(*back) == be::config_to_json(c));

  c.stations[0].name = "Nowa nazwa";
  REQUIRE(be::save_config_file(path, c, &err));  // replaces the existing file
  back = be::load_config_file(path, &err);
  REQUIRE(back);
  CHECK(back->stations[0].name == "Nowa nazwa");

  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f);
    std::fputs("{zepsute", f);
    std::fclose(f);
  }
  CHECK_FALSE(be::load_config_file(path, &err, &missing));
  CHECK_FALSE(missing);
  CHECK(contains(err, "nieprawidłowa konfiguracja"));

  // the target is a non-empty directory: the rename fails and the temporary file goes away
  const fs::path blocked = dir / "blocked.json";
  fs::create_directories(blocked / "inside");
  CHECK_FALSE(be::save_config_file(blocked.string(), c, &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(fs::exists(blocked.string() + ".tmp"));
  // the temporary file cannot be created: nothing is left either
  const fs::path no_tmp = dir / "no-tmp.json";
  fs::create_directories(no_tmp.string() + ".tmp");
  CHECK_FALSE(be::save_config_file(no_tmp.string(), c, &err));
  CHECK_FALSE(fs::exists(no_tmp));
  CHECK(fs::is_directory(no_tmp.string() + ".tmp"));  // not ours: left alone
  fs::remove_all(dir);
}

#endif  // BAM_WITH_CODECS
