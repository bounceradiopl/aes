// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "bam/sdp.hpp"
#include "test_util.hpp"

namespace {
const char* kMinimal =
    "v=0\r\n"
    "o=- 1 1 IN IP4 10.0.0.5\r\n"
    "s=Test\r\n"
    "c=IN IP4 239.10.0.1/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 96\r\n"
    "a=rtpmap:96 L16/48000/1\r\n"
    "a=framecount:48\r\n";
}

TEST_CASE("parse Orban SDP") {
  auto r = bam::parse_sdp(read_file(BAM_TEST_DATA "/orban.sdp"));
  REQUIRE_MESSAGE(r.stream, r.error);
  const auto& s = *r.stream;
  CHECK(s.session_name == "Optimod_114969_2");
  CHECK(s.multicast_address == "239.1.14.100");
  CHECK(s.ttl == 15);
  CHECK(s.port == 5004);
  CHECK(s.payload_type == 98);
  CHECK(s.encoding == bam::Encoding::L24);
  CHECK(s.sample_rate == 48000);
  CHECK(s.channels == 2);
  CHECK(s.frames_per_packet == 64);
  CHECK(s.ptime_ms == doctest::Approx(1.3333).epsilon(0.001));
  CHECK(s.source_filter == "10.80.14.100");
  CHECK(s.origin_address == "10.80.14.100");
  CHECK(s.ptp_gmid == "30-D6-59-FF-FE-01-C1-19");
  CHECK(s.ptp_domain == 0);
  REQUIRE(s.framecount);
  CHECK(*s.framecount == 64);
}

TEST_CASE("framecount used when no ptime, CRLF accepted, L16 mono") {
  auto r = bam::parse_sdp(kMinimal);
  REQUIRE_MESSAGE(r.stream, r.error);
  CHECK(r.stream->frames_per_packet == 48);
  CHECK(r.stream->ptime_ms == doctest::Approx(1.0));
  CHECK(r.stream->encoding == bam::Encoding::L16);
  CHECK(r.stream->channels == 1);
  CHECK(r.stream->ttl == 32);
  CHECK(r.stream->source_filter.empty());
}

TEST_CASE("default ptime is 1 ms") {
  std::string sdp = kMinimal;
  sdp.erase(sdp.find("a=framecount:48\r\n"));
  auto r = bam::parse_sdp(sdp);
  REQUIRE(r.stream);
  CHECK(r.stream->frames_per_packet == 48);
}

TEST_CASE("rejects unsupported formats") {
  std::string sdp = kMinimal;
  SUBCASE("44.1 kHz") {
    sdp.replace(sdp.find("L16/48000/1"), 11, "L24/44100/2");
    auto r = bam::parse_sdp(sdp);
    CHECK_FALSE(r.stream);
    CHECK(r.error.find("44100") != std::string::npos);
  }
  SUBCASE("unknown codec") {
    sdp.replace(sdp.find("L16/48000/1"), 11, "AM824/48000/2");
    auto r = bam::parse_sdp(sdp);
    CHECK_FALSE(r.stream);
    CHECK(r.error.find("AM824") != std::string::npos);
  }
  SUBCASE("8 channels") {
    sdp.replace(sdp.find("L16/48000/1"), 11, "L24/48000/8");
    CHECK_FALSE(bam::parse_sdp(sdp).stream);
  }
  SUBCASE("unicast") {
    sdp.replace(sdp.find("239.10.0.1/32"), 13, "10.0.0.1");
    auto r = bam::parse_sdp(sdp);
    CHECK_FALSE(r.stream);
    CHECK(r.error.find("multicast") != std::string::npos);
  }
  SUBCASE("no rtpmap") {
    sdp.erase(sdp.find("a=rtpmap"), std::string("a=rtpmap:96 L16/48000/1\r\n").size());
    CHECK_FALSE(bam::parse_sdp(sdp).stream);
  }
  SUBCASE("no media") {
    CHECK_FALSE(bam::parse_sdp("v=0\ns=x\n").stream);
  }
}

TEST_CASE("generate round-trips") {
  bam::SdpStream s;
  s.session_name = "BounceAES 01";
  s.origin_address = "10.80.14.50";
  s.multicast_address = "239.69.100.1";
  s.ttl = 15;
  s.port = 5004;
  s.payload_type = 98;
  s.encoding = bam::Encoding::L24;
  s.sample_rate = 48000;
  s.channels = 2;
  s.ptime_ms = 1;
  s.frames_per_packet = 48;
  s.ptp_gmid = "00-11-22-FF-FE-33-44-55";
  s.ptp_domain = 0;
  auto text = bam::generate_sdp(s, 1234, 1);
  auto r = bam::parse_sdp(text);
  REQUIRE_MESSAGE(r.stream, r.error);
  CHECK(r.stream->multicast_address == "239.69.100.1");
  CHECK(r.stream->frames_per_packet == 48);
  CHECK(r.stream->source_filter == "10.80.14.50");
  CHECK(r.stream->session_name == "BounceAES 01");
  CHECK(text.find("a=mediaclk:direct=0") != std::string::npos);
  CHECK(text.find("a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0") != std::string::npos);
  CHECK(text.find("o=- 1234 1 IN IP4 10.80.14.50") != std::string::npos);

  s.ptime_ms = 4.0 / 3.0;
  s.frames_per_packet = 64;
  auto r2 = bam::parse_sdp(bam::generate_sdp(s, 1, 1));
  REQUIRE(r2.stream);
  CHECK(r2.stream->frames_per_packet == 64);
}
