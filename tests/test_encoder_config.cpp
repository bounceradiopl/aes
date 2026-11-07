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
#include <nlohmann/json.hpp>

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

// The same source on another multicast group (n = 1, 2, ...): another input.
be::InputConfig orban_input_on(int n) {
  be::InputConfig in = orban_input();
  const std::string group = "239.1.14.100", other = "239.1.15." + std::to_string(n);
  for (size_t pos; (pos = in.sdp.find(group)) != std::string::npos;) in.sdp.replace(pos, group.size(), other);
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
  for (int i = 1; i <= 3; ++i) c.stations[0].inputs.push_back(orban_input_on(i));
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

TEST_CASE("encoder config: one station may not receive the same source twice") {
  // the same stream on two inputs would be mixed twice (+6 dB)
  auto c = sample();
  c.stations[0].inputs.push_back(orban_input());
  auto issues = be::validate_config(c);
  const auto* i = find_issue(issues, "stations[0].inputs[1].sdp");
  REQUIRE_MESSAGE(i, dump(issues));
  CHECK_MESSAGE(contains(i->message, "Stacja „Radio Bounce”, wejście 2: "), i->message);
  CHECK_MESSAGE(contains(i->message, "wejście 1"), i->message);
  CHECK_MESSAGE(contains(i->message, "+6 dB"), i->message);
  CHECK(find_issue(issues, "stations[0].inputs[0].sdp") == nullptr);

  // another SDP text for the same group, port and sender is the same source
  std::string sdp = c.stations[0].inputs[1].sdp;
  const std::string orig = "s=Optimod_114969_2";
  REQUIRE(contains(sdp, orig));
  sdp.replace(sdp.find(orig), orig.size(), "s=Inna nazwa");
  c.stations[0].inputs[1].sdp = sdp;
  CHECK(find_issue(be::validate_config(c), "stations[0].inputs[1].sdp"));

  // an input without a source filter receives every sender on its group and port, the filtered
  // one's too: also the same source, whichever comes first
  const auto unfiltered = [] {
    be::InputConfig in = orban_input();
    const size_t at = in.sdp.find("a=source-filter:");
    REQUIRE(at != std::string::npos);
    in.sdp.erase(at, in.sdp.find('\n', at) + 1 - at);
    REQUIRE(in.sdp.find("source-filter") == std::string::npos);
    return in;
  };
  c = sample();
  c.stations[0].inputs.push_back(unfiltered());
  issues = be::validate_config(c);
  i = find_issue(issues, "stations[0].inputs[1].sdp");
  REQUIRE_MESSAGE(i, dump(issues));
  CHECK_MESSAGE(contains(i->message, "Stacja „Radio Bounce”, wejście 2: "), i->message);
  CHECK_MESSAGE(contains(i->message, "wejście 1"), i->message);
  c = sample();
  c.stations[0].inputs.insert(c.stations[0].inputs.begin(), unfiltered());
  CHECK_MESSAGE(find_issue(be::validate_config(c), "stations[0].inputs[1].sdp"), dump(be::validate_config(c)));

  // two different senders on one group and port (each filtered to its own): two sources
  c = sample();
  c.stations[0].inputs.push_back(orban_input());
  {
    std::string& sdp = c.stations[0].inputs[1].sdp;
    const std::string sender = "239.1.14.100 10.80.14.100";
    REQUIRE(contains(sdp, sender));
    sdp.replace(sdp.find(sender), sender.size(), "239.1.14.100 10.80.14.101");
  }
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));

  // a disabled duplicate is allowed (it is not received), and so is the same source in another station
  c = sample();
  c.stations[0].inputs.push_back(orban_input());
  c.stations[0].inputs[1].enabled = false;
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
  c = sample();
  be::StationConfig other;
  other.id = "s2";
  other.name = "Bounce Dwa";
  other.inputs.push_back(orban_input());
  c.stations.push_back(other);
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
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
  // 80+ bytes of session name with a tab in it and a two-byte character across the 64-byte cut:
  // without the tab, "StudioAB " is 9 bytes and each "żółć " 9 more, so byte 64 is the second
  // byte of a "ż"
  std::string session = "Studio\tAB ";
  while (session.size() < 90) session += "żółć ";
  {
    std::string cleaned = session;
    cleaned.erase(cleaned.find('\t'), 1);
    REQUIRE(cleaned.size() > 64);
    REQUIRE((static_cast<unsigned char>(cleaned[64]) & 0xC0) == 0x80);  // a plain cut would split a character
  }
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
  CHECK(name.size() == 63);  // the split character left out whole
  CHECK(name.find('\t') == std::string::npos);
  CHECK(name.rfind("StudioAB ", 0) == 0);
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

// ---- Stereo Tool processing (Bounce Encoder 1.1) ----

namespace {
// Made-up keys in the Stereo Tool shape: never a real licence in tests.
const std::string kKeyA = "<Test A - SEKRETA-0123456789abcdef>";
const std::string kKeyB = "<Test B - SEKRETB-fedcba9876543210>";
const std::string kKeyDefault = "<Test default - SEKRETD-00112233>";

// sample() with a second station "Radio Dwa" (own stream), both running.
be::EncoderConfig two_stations() {
  auto c = sample();
  be::StationConfig two;
  two.id = "s2";
  two.name = "Radio Dwa";
  two.inputs.push_back(orban_input_on(1));
  two.streams.push_back(stream("str1", be::Codec::Mp3, 128, 44100, "/dwa.mp3"));
  c.stations.push_back(two);
  return c;
}

void enable_processing(be::StationConfig& st, std::string key, std::string preset = "FM Rock.sts") {
  st.processing.enabled = true;
  st.processing.license_key = std::move(key);
  st.processing.preset = std::move(preset);
}

bool any_contains(const std::vector<be::EncoderConfigIssue>& issues, const std::string& needle) {
  for (const auto& i : issues)
    if (contains(i.field, needle) || contains(i.message, needle)) return true;
  return false;
}
}  // namespace

TEST_CASE("encoder config: processing settings, defaults and JSON round trip") {
  const auto d = be::default_encoder_config();
  CHECK(d.stereotool_default_key.empty());
  CHECK(d.stereotool_web_port_base == 8100);
  CHECK(be::StationConfig{}.processing.enabled == false);
  CHECK(be::StationConfig{}.processing.license_key.empty());
  CHECK(be::StationConfig{}.processing.preset.empty());

  auto c = two_stations();
  c.stereotool_default_key = kKeyDefault;
  c.stereotool_web_port_base = 9100;
  enable_processing(c.stations[0], kKeyA, "FM Rock.sts");
  c.stations[1].processing.preset = "Mowa_2.v1-test.sts";  // stored while processing is off
  CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));

  const std::string file = be::config_to_json(c);
  CHECK(contains(file, "\"processing\": {"));
  CHECK(contains(file, kKeyA));  // the file keeps the keys (admin-only folder)
  CHECK(contains(file, kKeyDefault));
  CHECK_FALSE(contains(file, "_clear"));
  CHECK_FALSE(contains(file, "_set"));
  std::string err;
  auto back = be::config_from_json(file, &err);
  REQUIRE_MESSAGE(back, err);
  CHECK(be::config_to_json(*back) == file);
  CHECK(back->stereotool_default_key == kKeyDefault);
  CHECK(back->stereotool_web_port_base == 9100);
  CHECK(back->stations[0].processing.enabled);
  CHECK(back->stations[0].processing.license_key == kKeyA);
  CHECK(back->stations[0].processing.preset == "FM Rock.sts");
  CHECK_FALSE(back->stations[1].processing.enabled);
  CHECK(back->stations[1].processing.preset == "Mowa_2.v1-test.sts");

  // a 1.0 file (no processing keys at all) reads with processing off
  auto old = be::config_from_json(R"({"version":1,"stations":[{"id":"s1","name":"A"}]})", &err);
  REQUIRE_MESSAGE(old, err);
  CHECK_FALSE(old->stations[0].processing.enabled);
  CHECK(old->stereotool_web_port_base == 8100);

  // wrong types name the field, never the value
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":[{"processing":[]}]})", &err));
  CHECK_MESSAGE(contains(err, "stations[0].processing"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":[{"processing":{"enabled":"tak"}}]})", &err));
  CHECK_MESSAGE(contains(err, "stations[0].processing.enabled"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":[{"processing":{"license_key":5}}]})", &err));
  CHECK_MESSAGE(contains(err, "stations[0].processing.license_key"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stations":[{"processing":{"license_key_clear":"yes"}}]})", &err));
  CHECK_MESSAGE(contains(err, "stations[0].processing.license_key_clear"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stereotool_web_port_base":"8100"})", &err));
  CHECK_MESSAGE(contains(err, "stereotool_web_port_base"), err);
  CHECK_FALSE(be::config_from_json(R"({"version":1,"stereotool_default_key":true})", &err));
  CHECK_MESSAGE(contains(err, "stereotool_default_key"), err);
}

TEST_CASE("encoder config: Stereo Tool keys never leave through the API or an error") {
  auto c = two_stations();
  c.stereotool_default_key = kKeyDefault;
  enable_processing(c.stations[0], kKeyA);
  enable_processing(c.stations[1], "");  // uses the default key

  const std::string api = be::config_to_api_json(c);
  CHECK_FALSE(contains(api, "SEKRET"));
  const auto j = nlohmann::json::parse(api);
  CHECK(j["stereotool_default_key"] == "");
  CHECK(j["stereotool_default_key_set"] == true);
  CHECK(j["stereotool_web_port_base"] == 8100);
  CHECK(j["stations"][0]["processing"]["license_key"] == "");
  CHECK(j["stations"][0]["processing"]["license_key_set"] == true);
  CHECK(j["stations"][0]["processing"]["enabled"] == true);
  CHECK(j["stations"][0]["processing"]["preset"] == "FM Rock.sts");
  CHECK(j["stations"][1]["processing"]["license_key_set"] == false);
  CHECK(nlohmann::json::parse(be::config_to_api_json(be::default_encoder_config()))["stereotool_default_key_set"] == false);

  const auto masked = be::mask_passwords(c);
  CHECK(masked.stereotool_default_key.empty());
  CHECK(masked.stations[0].processing.license_key.empty());
  CHECK(c.stations[0].processing.license_key == kKeyA);  // the original is untouched

  // a JSON syntax error inside a key: the parser's "last read" echo is not passed on
  std::string err;
  const std::string broken = "{\"version\":1,\"stereotool_default_key\":\"<Test SEKRETX-77\x01>\"}";
  CHECK_FALSE(be::config_from_json(broken, &err));
  CHECK(contains(err, "nieprawidłowa konfiguracja"));
  CHECK_MESSAGE(!contains(err, "SEKRET"), err);
  const std::string unterminated = "{\"version\":1,\"stations\":[{\"processing\":{\"license_key\":\"<SEKRETY-12";
  CHECK_FALSE(be::config_from_json(unterminated, &err));
  CHECK_MESSAGE(!contains(err, "SEKRET"), err);
}

TEST_CASE("encoder config: merge_passwords keeps Stereo Tool keys") {
  auto previous = two_stations();
  previous.stereotool_default_key = kKeyDefault;
  enable_processing(previous.stations[0], kKeyA);
  enable_processing(previous.stations[1], kKeyB);

  SUBCASE("the API form sent back keeps every key") {
    std::string err;
    auto next = be::config_from_json(be::config_to_api_json(previous), &err);
    REQUIRE_MESSAGE(next, err);
    be::merge_passwords(*next, previous);
    CHECK(next->stations[0].processing.license_key == kKeyA);
    CHECK(next->stations[1].processing.license_key == kKeyB);
    CHECK(next->stereotool_default_key == kKeyDefault);
    CHECK(be::config_to_json(*next) == be::config_to_json(previous));
  }
  SUBCASE("a new key replaces the stored one") {
    auto next = be::mask_passwords(previous);
    next.stations[1].processing.license_key = "<Test nowy - SEKRETN-1>";
    next.stereotool_default_key = "<Test nowy domyslny - SEKRETM-2>";
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].processing.license_key == kKeyA);
    CHECK(next.stations[1].processing.license_key == "<Test nowy - SEKRETN-1>");
    CHECK(next.stereotool_default_key == "<Test nowy domyslny - SEKRETM-2>");
  }
  SUBCASE("no server rule: the key stays when the streams move") {
    auto next = be::mask_passwords(previous);
    next.stations[0].streams[0].host = "inny.serwer";
    next.stations[0].streams[0].port = 8443;
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].processing.license_key == kKeyA);
  }
  SUBCASE("an explicit clear removes a stored key (and only that one)") {
    std::string err;
    auto doc = nlohmann::json::parse(be::config_to_api_json(previous));
    doc["stations"][1]["processing"]["license_key_clear"] = true;
    doc["stereotool_default_key_clear"] = true;
    auto next = be::config_from_json(doc.dump(), &err);
    REQUIRE_MESSAGE(next, err);
    CHECK(next->stations[1].processing.license_key_clear);
    CHECK(next->stereotool_default_key_clear);
    be::merge_passwords(*next, previous);
    CHECK(next->stations[0].processing.license_key == kKeyA);
    CHECK(next->stations[1].processing.license_key.empty());
    CHECK(next->stereotool_default_key.empty());
    CHECK_FALSE(next->stations[1].processing.license_key_clear);  // the request flag is consumed
    CHECK_FALSE(next->stereotool_default_key_clear);
    CHECK_FALSE(contains(be::config_to_json(*next), "_clear"));
    // station 2 has processing on and now no key at all: the update is refused, not run unlicensed
    CHECK(find_issue(be::validate_config(*next), "stations[1].processing.license_key"));
  }
  SUBCASE("a clear together with a new key stores the new key") {
    auto next = be::mask_passwords(previous);
    next.stations[0].processing.license_key = "<Test nowy - SEKRETN-3>";
    next.stations[0].processing.license_key_clear = true;
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].processing.license_key == "<Test nowy - SEKRETN-3>");
    CHECK_FALSE(next.stations[0].processing.license_key_clear);
  }
  SUBCASE("a key of spaces only keeps the stored one, normalize trims a pasted key") {
    auto next = be::mask_passwords(previous);
    next.stations[0].processing.license_key = "  \t";
    next.stations[1].processing.license_key = "  <Test wklejony - SEKRETW-4>\r\n";
    be::merge_passwords(next, previous);
    be::normalize_config(next);
    CHECK(next.stations[0].processing.license_key == kKeyA);
    CHECK(next.stations[1].processing.license_key == "<Test wklejony - SEKRETW-4>");
    CHECK_MESSAGE(be::validate_config(next).empty(), dump(be::validate_config(next)));
  }
  SUBCASE("a new station or a station with another id gets no stored key") {
    auto next = be::mask_passwords(previous);
    next.stations[0].id = "s7";
    next.stations[1].id = "";
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].processing.license_key.empty());
    CHECK(next.stations[1].processing.license_key.empty());
  }
  SUBCASE("reordered stations keep their own keys") {
    auto next = be::mask_passwords(previous);
    std::swap(next.stations[0], next.stations[1]);
    be::merge_passwords(next, previous);
    CHECK(next.stations[0].processing.license_key == kKeyB);
    CHECK(next.stations[1].processing.license_key == kKeyA);
  }
}

TEST_CASE("encoder config: preset names") {
  for (const char* ok : {"FM Rock.sts", "a.sts", "Mowa_2.v1-test.sts", "x.STS", "012345678901234567890123456789012345678901234567890123456789.sts"})
    CHECK_MESSAGE(be::valid_preset_name(ok), ok);
  CHECK(std::string("012345678901234567890123456789012345678901234567890123456789.sts").size() == 64);
  CHECK(be::valid_preset_name("-a.sts"));  // a letter or digit somewhere before ".sts" is enough
  for (const char* bad : {"", ".sts", " .sts", "..sts", "-.sts", "_ .sts", " FM.sts", "0123456789012345678901234567890123456789012345678901234567890.sts", "FM Rock", "FM Rock.sts.txt", "../x.sts", "a/b.sts", "a\\b.sts", "C:x.sts", "zła.sts",
                          "a\tb.sts", "CON.sts", "nul.sts", "com1.sts", "COM0.sts", "LPT9.sts", "prn.x.sts", "a*.sts", "a?.sts", "a\".sts"})
    CHECK_MESSAGE(!be::valid_preset_name(bad), bad);
}

TEST_CASE("encoder config: processing validation names the station and never the key") {
  auto c = two_stations();

  SUBCASE("processing on needs a preset and a key") {
    c.stations[1].processing.enabled = true;
    auto issues = be::validate_config(c);
    REQUIRE_MESSAGE(find_issue(issues, "stations[1].processing.preset"), dump(issues));
    CHECK(contains(find_issue(issues, "stations[1].processing.preset")->message, "Stacja „Radio Dwa”"));
    CHECK(contains(find_issue(issues, "stations[1].processing.preset")->message, "preset"));
    REQUIRE(find_issue(issues, "stations[1].processing.license_key"));
    CHECK(contains(find_issue(issues, "stations[1].processing.license_key")->message, "Stacja „Radio Dwa”"));
    CHECK(contains(find_issue(issues, "stations[1].processing.license_key")->message, "klucz"));
    CHECK_FALSE(find_issue(issues, "stations[0].processing.preset"));

    c.stations[1].processing.preset = "FM Rock.sts";
    c.stereotool_default_key = kKeyDefault;  // the default key is enough
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
    c.stereotool_default_key.clear();
    c.stations[1].processing.license_key = kKeyB;  // so is the station's own
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
    // a disabled station with processing on is checked too (like an enabled stream of it)
    c.stations[1].enabled = false;
    c.stations[1].processing.preset.clear();
    CHECK(find_issue(be::validate_config(c), "stations[1].processing.preset"));
  }
  SUBCASE("processing off: empty preset and key are fine, a bad shape is not") {
    CHECK(be::validate_config(c).empty());
    c.stations[0].processing.preset = "../../Windows/x.sts";
    auto issues = be::validate_config(c);
    REQUIRE_MESSAGE(find_issue(issues, "stations[0].processing.preset"), dump(issues));
    CHECK(contains(find_issue(issues, "stations[0].processing.preset")->message, "Stacja „Radio Bounce”"));
    CHECK(contains(find_issue(issues, "stations[0].processing.preset")->message, ".sts"));
    c.stations[0].processing.preset = std::string(70, 'a') + ".sts";
    CHECK(find_issue(be::validate_config(c), "stations[0].processing.preset"));
    c.stations[0].processing.preset = "<SEKRETP-wklejony>";  // a key pasted into the preset field is not echoed
    issues = be::validate_config(c);
    REQUIRE(find_issue(issues, "stations[0].processing.preset"));
    CHECK_FALSE(any_contains(issues, "SEKRETP"));
    c.stations[0].processing.preset = "x<SEKRETP>.sts";
    CHECK_FALSE(any_contains(be::validate_config(c), "SEKRETP"));
    c.stations[0].processing.preset = "bad\x01name.sts";  // control characters are not echoed
    issues = be::validate_config(c);
    REQUIRE(find_issue(issues, "stations[0].processing.preset"));
    CHECK_FALSE(contains(find_issue(issues, "stations[0].processing.preset")->message, "\x01"));
  }
  SUBCASE("a key of the wrong shape is refused without quoting it") {
    for (const std::string& bad : {std::string("SEKRETQ-bez-nawiasow"), std::string("<SEKRETQ-bez-konca"), std::string("SEKRETQ>"),
                                   std::string("<SEKRETQ\x01>"), std::string("<SEKRETQ\x7f>"), std::string("<SEKRETQ-zażółć>"),
                                   "<SEKRETQ" + std::string(260, 'x') + ">"}) {
      CAPTURE(bad.size());
      c.stations[0].processing.license_key = bad;
      c.stereotool_default_key = bad;
      const auto issues = be::validate_config(c);
      REQUIRE_MESSAGE(find_issue(issues, "stations[0].processing.license_key"), dump(issues));
      CHECK(contains(find_issue(issues, "stations[0].processing.license_key")->message, "Stacja „Radio Bounce”"));
      REQUIRE(find_issue(issues, "stereotool_default_key"));
      CHECK(contains(find_issue(issues, "stereotool_default_key")->message, "Domyślny klucz Stereo Tool"));
      CHECK_FALSE(any_contains(issues, "SEKRETQ"));
      CHECK_FALSE(any_contains(be::config_warnings(c), "SEKRETQ"));
    }
    c.stations[0].processing.license_key = "<" + std::string(254, 'x') + ">";  // 256 characters: the limit
    c.stereotool_default_key = c.stations[0].processing.license_key;
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
  }
}

TEST_CASE("encoder config: Stereo Tool web interface port base") {
  auto c = sample();
  for (int ok : {1024, 8100, 65000}) {
    c.stereotool_web_port_base = ok;
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
  }
  for (int bad : {0, 80, 1023, 65001, 65520, -5}) {
    c.stereotool_web_port_base = bad;
    auto issues = be::validate_config(c);
    REQUIRE_MESSAGE(find_issue(issues, "stereotool_web_port_base"), dump(issues));
    CHECK(contains(find_issue(issues, "stereotool_web_port_base")->message, "1024–65000"));
  }
  // the 16 station ports base … base + 15 may not include the panel port
  c.http_port = 8070;
  for (int clash : {8070, 8060, 8055}) {
    c.stereotool_web_port_base = clash;
    auto issues = be::validate_config(c);
    REQUIRE_MESSAGE(find_issue(issues, "stereotool_web_port_base"), dump(issues));
    CHECK(contains(find_issue(issues, "stereotool_web_port_base")->message, "8070"));
  }
  for (int fine : {8071, 8054}) {
    c.stereotool_web_port_base = fine;
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
  }
}

TEST_CASE("encoder config: the same Stereo Tool key on two stations is a warning, not an error") {
  auto c = two_stations();
  enable_processing(c.stations[0], kKeyA);
  enable_processing(c.stations[1], kKeyB);
  CHECK(be::config_warnings(c).empty());
  CHECK(be::config_warnings(be::default_encoder_config()).empty());

  SUBCASE("the same station key") {
    c.stations[1].processing.license_key = kKeyA;
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
    const auto w = be::config_warnings(c);
    REQUIRE_MESSAGE(w.size() == 1, dump(w));
    CHECK(w[0].field == "stations[1].processing.license_key");
    CHECK(contains(w[0].message, "Stacja „Radio Dwa”"));
    CHECK(contains(w[0].message, "„Radio Bounce”"));
    CHECK_FALSE(contains(w[0].message, "SEKRET"));
  }
  SUBCASE("the default key used by two stations") {
    c.stereotool_default_key = kKeyDefault;
    c.stations[0].processing.license_key.clear();
    c.stations[1].processing.license_key.clear();
    const auto w = be::config_warnings(c);
    REQUIRE_MESSAGE(w.size() == 1, dump(w));
    CHECK(contains(w[0].message, "domyślny"));
    CHECK_FALSE(contains(w[0].message, "SEKRET"));
  }
  SUBCASE("a station key equal to the default key used by another station") {
    c.stereotool_default_key = kKeyA;
    c.stations[1].processing.license_key.clear();
    CHECK(be::config_warnings(c).size() == 1);
  }
  SUBCASE("a stopped station or processing off does not count") {
    c.stations[1].processing.license_key = kKeyA;
    c.stations[1].enabled = false;
    CHECK(be::config_warnings(c).empty());
    c.stations[1].enabled = true;
    c.stations[1].processing.enabled = false;
    CHECK(be::config_warnings(c).empty());
  }
  SUBCASE("three stations on one key: each later one is warned") {
    be::StationConfig three = c.stations[1];
    three.id = "s3";
    three.name = "Radio Trzy";
    three.inputs[0] = orban_input_on(2);
    three.streams[0].mount = "/trzy.mp3";
    c.stations.push_back(three);
    for (auto& st : c.stations) st.processing.license_key = kKeyB;
    CHECK_MESSAGE(be::validate_config(c).empty(), dump(be::validate_config(c)));
    const auto w = be::config_warnings(c);
    REQUIRE_MESSAGE(w.size() == 2, dump(w));
    CHECK(w[0].field == "stations[1].processing.license_key");
    CHECK(w[1].field == "stations[2].processing.license_key");
  }
}

#endif  // BAM_WITH_CODECS
