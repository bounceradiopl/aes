// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/bridge_protocol.hpp"

TEST_CASE("bridge request round trip") {
  bam::BridgeRequest r;
  r.generation = 7;
  r.sources.push_back({0, "win-app:chrome.exe"});
  r.sources.push_back({3, "win-loop:{0.0.0.00000000}.{abc}"});
  r.dests.push_back({"win-out:{0.0.0.00000000}.{def}", bam::ShmSource::Net, 2, 240});
  auto back = bam::request_from_json(bam::to_json(r));
  REQUIRE(back);
  CHECK(back->generation == 7);
  REQUIRE(back->sources.size() == 2);
  CHECK(back->sources[1].slot == 3);
  CHECK(back->sources[1].id == "win-loop:{0.0.0.00000000}.{abc}");
  REQUIRE(back->dests.size() == 1);
  CHECK(back->dests[0].kind == bam::ShmSource::Net);
  CHECK(back->dests[0].pair == 2);
  CHECK(back->dests[0].delay_frames == 240);
}

TEST_CASE("bridge report round trip") {
  bam::BridgeReport r;
  r.generation = 9;
  r.os_build = "26100";
  r.endpoints.push_back({"{0.0.0.00000000}.{a}", "Głośniki (Realtek)", true, true});
  r.endpoints.push_back({"{0.0.1.00000000}.{b}", "Mikrofon", false, false});
  r.apps.push_back({"spotify.exe", "Spotify", 1234});
  bam::BridgeSlotState s;
  s.id = "win-app:spotify.exe";
  s.ok = true;
  s.peak_db[0] = -12.5f;
  s.peak_db[1] = -13.0f;
  r.slots.push_back(s);
  r.slots.push_back({"win-app:zoom.exe", false, "aplikacja nie działa", {-120, -120}});
  auto back = bam::report_from_json(bam::to_json(r));
  REQUIRE(back);
  CHECK(back->generation == 9);
  CHECK(back->os_build == "26100");
  REQUIRE(back->endpoints.size() == 2);
  CHECK(back->endpoints[0].name == "Głośniki (Realtek)");
  CHECK(back->endpoints[0].default_device);
  CHECK_FALSE(back->endpoints[1].render);
  REQUIRE(back->apps.size() == 1);
  CHECK(back->apps[0].pid == 1234);
  REQUIRE(back->slots.size() == 2);
  CHECK(back->slots[0].peak_db[0] == doctest::Approx(-12.5));
  CHECK(back->slots[1].reason == "aplikacja nie działa");
}

TEST_CASE("bridge protocol: device channel count and channel pair, with defaults when absent") {
  bam::BridgeRequest r;
  r.generation = 1;
  r.dests.push_back({"win-out:{0.0.0.00000000}.{usb}#3", bam::ShmSource::Net, 0, 0, 3});
  auto req_back = bam::request_from_json(bam::to_json(r));
  REQUIRE(req_back);
  REQUIRE(req_back->dests.size() == 1);
  CHECK(req_back->dests[0].channel_pair == 3);

  bam::BridgeReport rep;
  rep.generation = 1;
  rep.endpoints.push_back({"{0.0.0.00000000}.{usb}", "Speakers (USB Audio)", true, false, 8});
  auto rep_back = bam::report_from_json(bam::to_json(rep));
  REQUIRE(rep_back);
  REQUIRE(rep_back->endpoints.size() == 1);
  CHECK(rep_back->endpoints[0].channels == 8);

  // a report/request without the new fields parses with defaults: channels = 2, channel_pair = 1
  auto req_default =
      bam::request_from_json(R"({"generation":1,"sources":[],"dests":[{"id":"win-out:x","kind":"net","pair":0,"delay_frames":0}]})");
  REQUIRE(req_default);
  REQUIRE(req_default->dests.size() == 1);
  CHECK(req_default->dests[0].channel_pair == 1);

  auto rep_default = bam::report_from_json(
      R"({"generation":1,"endpoints":[{"id":"x","name":"X","render":true,"default":false}],"apps":[],"slots":[]})");
  REQUIRE(rep_default);
  REQUIRE(rep_default->endpoints.size() == 1);
  CHECK(rep_default->endpoints[0].channels == 2);
}

TEST_CASE("malformed bridge JSON is rejected") {
  CHECK_FALSE(bam::request_from_json("{"));
  CHECK_FALSE(bam::request_from_json("{\"generation\":1}"));
  CHECK_FALSE(bam::request_from_json(R"({"generation":1,"sources":[{"slot":99,"id":"win-app:x.exe"}],"dests":[]})"));
  CHECK_FALSE(bam::report_from_json(""));
  CHECK_FALSE(bam::report_from_json(R"({"generation":"x","endpoints":[],"apps":[],"slots":[]})"));
}
