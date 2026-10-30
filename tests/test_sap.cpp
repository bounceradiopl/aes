// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/sap.hpp"
#include "test_util.hpp"

TEST_CASE("SAP build/parse round trip") {
  const std::string sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  auto pkt = bam::build_sap(false, 0x1234, {10, 80, 14, 100}, sdp);
  CHECK(pkt[0] == 0x20);
  auto p = bam::parse_sap(pkt);
  REQUIRE(p);
  CHECK_FALSE(p->deletion);
  CHECK(p->hash == 0x1234);
  CHECK(p->origin == std::array<uint8_t, 4>{10, 80, 14, 100});
  CHECK(p->payload_type == "application/sdp");
  CHECK(p->sdp == sdp);

  auto del = bam::parse_sap(bam::build_sap(true, 0x1234, {10, 80, 14, 100}, sdp));
  REQUIRE(del);
  CHECK(del->deletion);
}

TEST_CASE("SAP without payload type and garbage") {
  std::vector<uint8_t> pkt = {0x20, 0, 0x00, 0x05, 192, 168, 1, 2};
  const std::string sdp = "v=0\r\ns=x\r\n";
  pkt.insert(pkt.end(), sdp.begin(), sdp.end());
  auto p = bam::parse_sap(pkt);
  REQUIRE(p);
  CHECK(p->payload_type.empty());
  CHECK(p->sdp == sdp);

  std::vector<uint8_t> garbage = {0xFF, 0x00, 0x01};
  CHECK_FALSE(bam::parse_sap(garbage));
  std::vector<uint8_t> ipv6 = {0x30, 0, 0, 1, 0, 0, 0, 0};  // A bit set
  CHECK_FALSE(bam::parse_sap(ipv6));
}

TEST_CASE("SAP hash is stable and non-zero") {
  CHECK(bam::sap_hash("abc") == bam::sap_hash("abc"));
  CHECK(bam::sap_hash("abc") != bam::sap_hash("abd"));
  CHECK(bam::sap_hash("") != 0);
}

TEST_CASE("SAP directory") {
  const std::string sdp = read_file(BAM_TEST_DATA "/orban.sdp");
  bam::SapDirectory dir;
  const int64_t s = 1'000'000'000;
  dir.on_packet(bam::build_sap(false, 0x77, {10, 80, 14, 100}, sdp), 5 * s);
  dir.on_packet(std::vector<uint8_t>{1, 2, 3}, 5 * s);
  auto list = dir.list();
  REQUIRE(list.size() == 1);
  CHECK(list[0].name == "Optimod_114969_2");
  CHECK(list[0].id == "10.80.14.100/119");
  CHECK(list[0].origin == "10.80.14.100");
  CHECK(list[0].parsed.multicast_address == "239.1.14.100");
  CHECK(dir.find("10.80.14.100/119"));

  dir.expire(5 * s + 299 * s);
  CHECK(dir.list().size() == 1);
  dir.expire(5 * s + 301 * s);
  CHECK(dir.list().empty());

  dir.on_packet(bam::build_sap(false, 0x77, {10, 80, 14, 100}, sdp), 400 * s);
  dir.on_packet(bam::build_sap(true, 0x77, {10, 80, 14, 100}, sdp), 401 * s);
  CHECK(dir.list().empty());
}
