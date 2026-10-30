// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/config.hpp"
#include "test_util.hpp"

namespace {
bool has_issue(const std::vector<bam::ConfigIssue>& issues, int device, const std::string& field) {
  for (const auto& i : issues)
    if (i.device == device && i.field == field) return true;
  return false;
}
}  // namespace

TEST_CASE("default config") {
  auto c = bam::default_config();
  CHECK(c.devices[0].enabled);
  CHECK_FALSE(c.devices[1].enabled);
  CHECK(c.devices[0].name == "BounceAES 01");
  CHECK(c.devices[31].name == "BounceAES 32");
  CHECK(c.devices[0].tx_address == "239.69.100.1");
  CHECK(c.devices[31].tx_address == "239.69.100.32");
  CHECK(c.devices[0].tx_ptime_us == 1000);
  CHECK(c.devices[0].keep_active);
  CHECK(c.devices[0].rx_latency_us == 5000);
  CHECK(c.pc_nominal_level_dbfs == -20);
  CHECK(c.http_port == 8067);
  CHECK(bam::validate_config(c).empty());
}

TEST_CASE("JSON round trip") {
  auto c = bam::default_config();
  c.interface_ip = "10.80.14.50";
  c.ptp_domain = 3;
  c.devices[4].enabled = true;
  c.devices[4].name = "Kraków";
  c.devices[4].rx_sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  c.devices[4].tx_ptime_us = 1333;
  c.password_hash = "x";
  std::string err;
  auto back = bam::config_from_json(bam::config_to_json(c), &err);
  REQUIRE_MESSAGE(back, err);
  CHECK(bam::config_to_json(*back) == bam::config_to_json(c));
  CHECK(back->devices[4].name == "Kraków");
  CHECK(back->ptp_domain == 3);
}

TEST_CASE("missing keys use defaults, unknown keys ignored, bad JSON reported") {
  std::string err;
  auto c = bam::config_from_json(R"({"ptp_domain": 2, "foo": 1, "devices": [{"name": "A"}]})", &err);
  REQUIRE_MESSAGE(c, err);
  CHECK(c->ptp_domain == 2);
  CHECK(c->devices[0].name == "A");
  CHECK(c->devices[0].tx_address == "239.69.100.1");
  CHECK(c->devices[1].name == "BounceAES 02");
  CHECK(c->http_port == 8067);

  CHECK_FALSE(bam::config_from_json("{not json", &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(bam::config_from_json(R"({"http_port": "abc"})", &err));
}

TEST_CASE("validation") {
  auto c = bam::default_config();
  c.devices[2].tx_ptime_us = 999;
  c.devices[3].enabled = true;
  c.devices[3].tx_address = "10.0.0.1";
  c.devices[1].enabled = true;
  c.devices[1].tx_address = "239.69.100.1";  // duplicates device 1
  c.devices[5].rx_sdp = "garbage";
  c.devices[6].rx_latency_us = 50;
  c.devices[7].tx_address = "224.0.0.5";
  c.pc_nominal_level_dbfs = 5;
  c.http_port = 0;
  auto issues = bam::validate_config(c);
  CHECK(has_issue(issues, 3, "tx_ptime_us"));
  CHECK(has_issue(issues, 4, "tx_address"));
  CHECK(has_issue(issues, 2, "tx_address"));
  CHECK(has_issue(issues, 6, "rx_sdp"));
  CHECK(has_issue(issues, 7, "rx_latency_us"));
  CHECK(has_issue(issues, 8, "tx_address"));
  CHECK(has_issue(issues, 0, "pc_nominal_level_dbfs"));
  CHECK(has_issue(issues, 0, "http_port"));
}

TEST_CASE("normalize raises rx latency to ptime + 1 ms") {
  auto c = bam::default_config();
  c.devices[0].rx_sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  c.devices[0].rx_latency_us = 1500;
  auto notices = bam::normalize_config(c);
  CHECK(c.devices[0].rx_latency_us == 2334);
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].find("01") != std::string::npos);
  c.devices[0].rx_latency_us = 5000;
  CHECK(bam::normalize_config(c).empty());
  CHECK(c.devices[0].rx_latency_us == 5000);
}

TEST_CASE("ptime frames and gains") {
  CHECK(bam::ptime_us_to_frames(1000) == 48);
  CHECK(bam::ptime_us_to_frames(1333) == 64);
  CHECK(bam::ptime_us_to_frames(4000) == 192);
  auto c = bam::default_config();
  CHECK(bam::gain_to_app(c) == 1.0);
  CHECK(bam::gain_to_network(c) == 1.0);
  c.pc_nominal_level_dbfs = -8;
  CHECK(bam::gain_to_app(c) == doctest::Approx(3.981).epsilon(1e-3));
  CHECK(bam::gain_to_network(c) == doctest::Approx(0.2512).epsilon(1e-3));
}
