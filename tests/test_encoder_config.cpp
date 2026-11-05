// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder configuration (spec §7): JSON round trip, validation of the limits (§2) and the
// codec table (§5.2), normalisation, password masking and merging, the configuration file.
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

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

  auto c = sample();
  c.stations[0].name = "Radio Bounce";
  std::string text = be::config_to_json(c);
  const std::string aac = "\"codec\": \"aac\"";
  REQUIRE(contains(text, aac));
  text.replace(text.find(aac), aac.size(), "\"codec\": \"opus\"");
  CHECK_FALSE(be::config_from_json(text, &err));
  CHECK_MESSAGE(contains(err, "Stacja „Radio Bounce”, strumień 2"), err);
  CHECK(contains(err, "opus"));

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
  CHECK_FALSE(check([](be::StreamConfig& s) { s.user = "a:b"; }, "stations[0].streams[1].user", ":"));
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
  c.stations[0].streams[1].host = "stream.example.com";
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
  const auto* i = find_issue(issues, "stations[1].id");
  REQUIRE_MESSAGE(i, dump(issues));
  CHECK(contains(i->message, "Stacja „Kopia”"));
  CHECK(contains(i->message, "„Radio Bounce”"));

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
  fs::remove_all(dir);
}

#endif  // BAM_WITH_CODECS
