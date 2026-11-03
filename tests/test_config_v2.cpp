// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/config.hpp"
#include "test_util.hpp"

namespace {
bool has_issue(const std::vector<bam::ConfigIssue>& issues, int device, const std::string& field_contains) {
  for (const auto& i : issues)
    if (i.device == device && i.field.find(field_contains) != std::string::npos) return true;
  return false;
}
}  // namespace

TEST_CASE("default config v2 round trip") {
  auto c = bam::default_config();
  CHECK(c.ptp_role == bam::PtpRole::Auto);
  CHECK(c.ptp_priority1 == 250);
  CHECK(c.routes.at("net-out:1") == "asio-out:1");
  CHECK(c.asio_names[0] == "BounceAES 01");
  std::string err;
  auto back = bam::config_from_json(bam::config_to_json(c), &err);
  REQUIRE_MESSAGE(back, err);
  CHECK(bam::config_to_json(*back) == bam::config_to_json(c));
  CHECK(bam::config_to_json(c).find("\"version\": 2") != std::string::npos);
  CHECK(bam::validate_config(c).empty());
}

TEST_CASE("migration from 1.0") {
  const std::string orban = read_file(BAM_TEST_DATA "/orban.sdp");
  std::string v1 = R"({"interface_ip":"10.80.14.50","ptp_domain":0,"devices":[)";
  v1 += R"({"enabled":true,"name":"Warszawa","tx_address":"239.69.100.1","tx_ptime_us":1000,"keep_active":true,"rx_sdp":"","rx_latency_us":5000},)";
  // device 2 has an Orban subscription
  std::string sdp_escaped;
  for (char ch : orban) {
    if (ch == '"') sdp_escaped += "\\\"";
    else if (ch == '\n') sdp_escaped += "\\n";
    else if (ch == '\r') sdp_escaped += "\\r";
    else sdp_escaped += ch;
  }
  v1 += R"({"enabled":true,"name":"Odbiornik","tx_address":"239.69.100.2","tx_ptime_us":1000,"keep_active":true,"rx_sdp":")" + sdp_escaped + R"(","rx_latency_us":5000})";
  v1 += "]}";

  std::string err;
  bool migrated = false;
  auto c = bam::config_from_json(v1, &err, &migrated);
  REQUIRE_MESSAGE(c, err);
  CHECK(migrated);
  CHECK(c->outputs[0].name == "Warszawa");
  CHECK(c->outputs[0].address == "239.69.100.1");
  CHECK(c->outputs[1].enabled);
  CHECK(c->subscriptions[1].enabled);
  CHECK(c->subscriptions[1].sdp == orban);
  CHECK_FALSE(c->subscriptions[0].enabled);  // device 1 had no rx_sdp
  CHECK(c->asio_names[0] == "Warszawa");
  CHECK(c->routes.at("net-out:1") == "asio-out:1");
  CHECK(c->routes.at("net-out:2") == "asio-out:2");
  CHECK(c->routes.at("asio-in:2") == "net-in:2");
  CHECK(c->routes.count("asio-in:1") == 0);  // device 1 had no subscription
}

TEST_CASE("validation of v2 fields") {
  auto c = bam::default_config();
  c.ptp_priority1 = 300;
  c.outputs[2].ptime_us = 999;
  c.subscriptions[3].enabled = true;
  c.subscriptions[3].sdp = "";
  c.routes["net-out:5"] = "nonsense:1";
  c.routes["bad-dest:1"] = "net-in:1";
  c.station_name = std::string(50, 'x');
  auto issues = bam::validate_config(c);
  CHECK(has_issue(issues, 0, "ptp_priority1"));
  CHECK(has_issue(issues, 3, "ptime_us"));
  CHECK(has_issue(issues, 4, "sdp"));
  CHECK(has_issue(issues, 0, "routes.net-out:5"));
  CHECK(has_issue(issues, 0, "routes.bad-dest:1"));
  CHECK(has_issue(issues, 0, "station_name"));
}

TEST_CASE("too many Windows routes flagged") {
  auto c = bam::default_config();
  for (int i = 0; i < 17; ++i) c.routes["win-out:dev" + std::to_string(i)] = "net-in:1";
  CHECK(has_issue(bam::validate_config(c), 0, "routes"));
}

TEST_CASE("normalize raises subscription latency and names it") {
  auto c = bam::default_config();
  c.subscriptions[0].enabled = true;
  c.subscriptions[0].sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  c.subscriptions[0].latency_us = 1500;
  auto notices = bam::normalize_config(c);
  CHECK(c.subscriptions[0].latency_us == 2334);
  CHECK(c.subscriptions[0].name == "Optimod_114969_2");
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].find("01") != std::string::npos);
}

TEST_CASE("ptp role names") {
  CHECK(std::string(bam::ptp_role_name(bam::PtpRole::Preferred)) == "preferred");
  CHECK(bam::parse_ptp_role("slave") == bam::PtpRole::Slave);
  CHECK_FALSE(bam::parse_ptp_role("boss"));
}

TEST_CASE("config: mixes round trip and validation") {
  auto c = bam::default_config();
  c.mixes["asio-in:3"].sources = {{"asio-out:1", 0, false}, {"net-in:2", -6.5, true}};
  auto back = bam::config_from_json(bam::config_to_json(c), nullptr);
  REQUIRE(back);
  REQUIRE(back->mixes.count("asio-in:3"));
  const auto& m = back->mixes.at("asio-in:3");
  REQUIRE(m.sources.size() == 2);
  CHECK(m.sources[1].source == "net-in:2");
  CHECK(m.sources[1].gain_db == doctest::Approx(-6.5));
  CHECK(m.sources[1].mute);
  CHECK(bam::validate_config(c).empty());

  auto bad = c;
  bad.routes["asio-in:3"] = "asio-out:4";  // a destination is either routed or mixed
  CHECK_FALSE(bam::validate_config(bad).empty());
  bad = c;
  bad.mixes["asio-in:3"].sources[0].gain_db = 13;
  CHECK_FALSE(bam::validate_config(bad).empty());
  bad = c;
  bad.mixes["asio-in:3"].sources.push_back({"asio-out:1", 0, false});  // duplicate
  CHECK_FALSE(bam::validate_config(bad).empty());
  bad = c;
  for (int i = 1; i <= 17; ++i) bad.mixes["asio-in:4"].sources.push_back({"asio-out:" + std::to_string(i), 0, false});
  CHECK_FALSE(bam::validate_config(bad).empty());
  bad = c;
  bad.mixes["nie:cel"].sources = {};
  CHECK_FALSE(bam::validate_config(bad).empty());

  auto old = bam::config_from_json(R"({"version":2})", nullptr);  // 1.1 file: no mixes
  REQUIRE(old);
  CHECK(old->mixes.empty());
}

TEST_CASE("config: a mix with one unmuted 0 dB source is direct") {
  bam::MixConfig m;
  m.sources = {{"asio-out:1", 0, false}, {"asio-out:2", -3, true}};
  CHECK(bam::direct_source(m) == std::optional<std::string>("asio-out:1"));
  m.sources[0].gain_db = -0.5;
  CHECK_FALSE(bam::direct_source(m));
  m.sources = {{"asio-out:1", 0, false}, {"asio-out:2", 0, false}};
  CHECK_FALSE(bam::direct_source(m));
  CHECK_FALSE(bam::direct_source(bam::MixConfig{}));
}
