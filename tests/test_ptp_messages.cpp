// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/ptp_messages.hpp"

namespace {
bam::PortIdentity port(uint8_t last) {
  bam::PortIdentity p;
  p.clock.bytes = {0x30, 0xD6, 0x59, 0xFF, 0xFE, 0x01, 0xC1, last};
  p.port = 1;
  return p;
}
}  // namespace

TEST_CASE("clock identity formatting") {
  CHECK(port(0x19).clock.to_string() == "30-D6-59-FF-FE-01-C1-19");
  CHECK(bam::ClockIdentity{}.is_zero());
}

TEST_CASE("timestamp conversion") {
  auto t = bam::PtpTimestamp::from_ns(1'700'000'000'123'456'789);
  CHECK(t.seconds == 1'700'000'000);
  CHECK(t.nanoseconds == 123'456'789);
  CHECK(t.to_ns() == 1'700'000'000'123'456'789);
}

TEST_CASE("sync and follow up") {
  auto sync = bam::build_sync(0, port(1), 77, true, bam::PtpTimestamp{});
  CHECK(sync.size() == 44);
  auto m = bam::parse_ptp(sync);
  REQUIRE(m);
  CHECK(m->header.type == bam::PtpMsgType::Sync);
  CHECK(m->header.two_step());
  CHECK(m->header.sequence == 77);
  CHECK(m->header.source == port(1));
  CHECK(m->header.version == 2);

  auto fu = bam::build_follow_up(0, port(1), 77, bam::PtpTimestamp{5, 6});
  CHECK(fu.size() == 44);
  auto f = bam::parse_ptp(fu);
  REQUIRE(f);
  CHECK(f->header.type == bam::PtpMsgType::FollowUp);
  CHECK(f->timestamp.seconds == 5);
  CHECK(f->timestamp.nanoseconds == 6);
}

TEST_CASE("one-step sync carries its timestamp") {
  auto sync = bam::build_sync(3, port(1), 1, false, bam::PtpTimestamp{10, 20});
  auto m = bam::parse_ptp(sync);
  REQUIRE(m);
  CHECK_FALSE(m->header.two_step());
  CHECK(m->header.domain == 3);
  CHECK(m->timestamp.to_ns() == 10'000'000'020);
}

TEST_CASE("delay request and response") {
  auto req = bam::build_delay_req(0, port(9), 5);
  CHECK(req.size() == 44);
  auto r = bam::parse_ptp(req);
  REQUIRE(r);
  CHECK(r->header.type == bam::PtpMsgType::DelayReq);
  CHECK(r->header.sequence == 5);

  auto resp = bam::build_delay_resp(0, port(1), 5, bam::PtpTimestamp{100, 200}, port(9));
  CHECK(resp.size() == 54);
  auto d = bam::parse_ptp(resp);
  REQUIRE(d);
  CHECK(d->header.type == bam::PtpMsgType::DelayResp);
  CHECK(d->requesting == port(9));
  CHECK(d->timestamp.to_ns() == 100'000'000'200);
}

TEST_CASE("announce") {
  bam::AnnounceBody body;
  body.priority1 = 100;
  body.clock_class = 6;
  body.gm_identity = port(1).clock;
  auto a = bam::build_announce(0, port(1), 9, body);
  CHECK(a.size() == 64);
  auto m = bam::parse_ptp(a);
  REQUIRE(m);
  CHECK(m->header.type == bam::PtpMsgType::Announce);
  CHECK(m->announce.priority1 == 100);
  CHECK(m->announce.clock_class == 6);
  CHECK(m->announce.gm_identity == port(1).clock);
  CHECK(m->announce.utc_offset == 37);
}

TEST_CASE("correction field") {
  auto sync = bam::build_sync(0, port(1), 1, false, bam::PtpTimestamp{});
  sync[8 + 5] = 0x01;  // correction = 0x0000'0000'0001'0000 scaled = 1 ns
  auto m = bam::parse_ptp(sync);
  REQUIRE(m);
  CHECK(m->header.correction_ns() == 1);
}

TEST_CASE("invalid messages rejected") {
  auto sync = bam::build_sync(0, port(1), 1, false, bam::PtpTimestamp{});
  auto bad = sync;
  bad[1] = 1;  // PTP v1
  CHECK_FALSE(bam::parse_ptp(bad));
  CHECK_FALSE(bam::parse_ptp(std::span<const uint8_t>(sync.data(), 20)));
}

TEST_CASE("announce comparison") {
  bam::AnnounceBody a, b;
  a.gm_identity = port(1).clock;
  b.gm_identity = port(2).clock;
  CHECK(bam::compare_announce(a, b) < 0);  // identity tie-break
  b.priority1 = 10;
  CHECK(bam::compare_announce(a, b) > 0);
  b.priority1 = 128;
  a.clock_class = 248;
  b.clock_class = 6;
  CHECK(bam::compare_announce(a, b) > 0);
  CHECK(bam::compare_announce(a, a) == 0);
}
