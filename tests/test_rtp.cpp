// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <vector>

#include "bam/rtp.hpp"

TEST_CASE("RTP header round trip") {
  bam::RtpHeader h;
  h.payload_type = 98;
  h.marker = true;
  h.sequence = 0xABCD;
  h.timestamp = 0x12345678;
  h.ssrc = 0xDEADBEEF;
  std::vector<uint8_t> pkt(bam::kRtpHeaderSize + 6, 0);
  bam::write_rtp_header(h, pkt.data());
  CHECK(pkt[0] == 0x80);
  CHECK(pkt[1] == (0x80 | 98));
  auto v = bam::parse_rtp(pkt);
  REQUIRE(v);
  CHECK(v->header.payload_type == 98);
  CHECK(v->header.marker);
  CHECK(v->header.sequence == 0xABCD);
  CHECK(v->header.timestamp == 0x12345678);
  CHECK(v->header.ssrc == 0xDEADBEEF);
  CHECK(v->payload.size() == 6);
}

TEST_CASE("RTP parse rejects bad packets") {
  std::vector<uint8_t> short_pkt(11, 0x80);
  CHECK_FALSE(bam::parse_rtp(short_pkt));
  std::vector<uint8_t> v1(20, 0);
  v1[0] = 0x40;
  CHECK_FALSE(bam::parse_rtp(v1));
}

TEST_CASE("RTP CSRC, extension and padding are skipped") {
  std::vector<uint8_t> pkt(12 + 8 + 4 + 4 + 6 + 2, 0);
  pkt[0] = 0x80 | 0x20 | 0x10 | 2;  // padding, extension, CC=2
  pkt[1] = 98;
  size_t ext = 12 + 8;
  pkt[ext + 2] = 0;
  pkt[ext + 3] = 1;  // extension length 1 word
  size_t payload = ext + 4 + 4;
  pkt[payload] = 0x11;
  pkt.back() = 2;  // 2 bytes of padding
  auto v = bam::parse_rtp(pkt);
  REQUIRE(v);
  CHECK(v->payload.size() == 6);
  CHECK(v->payload[0] == 0x11);
}

TEST_CASE("decode L24 and L16") {
  const uint8_t l24[] = {0x7F, 0xFF, 0xFF, 0x80, 0x00, 0x00};
  int32_t mono[2];
  int32_t* out1[] = {mono};
  bam::decode_pcm(bam::Encoding::L24, l24, 2, 1, out1);
  CHECK(mono[0] == 0x7FFFFF00);
  CHECK(mono[1] == static_cast<int32_t>(0x80000000u));

  const uint8_t l16[] = {0x12, 0x34, 0xFF, 0xFE};
  int32_t l[1], r[1];
  int32_t* out2[] = {l, r};
  bam::decode_pcm(bam::Encoding::L16, l16, 1, 2, out2);
  CHECK(l[0] == 0x12340000);
  CHECK(r[0] == static_cast<int32_t>(0xFFFE0000u));
}

TEST_CASE("encode L24 round trip") {
  int32_t l[3] = {0x12345678, -1, static_cast<int32_t>(0x80000000u)};
  int32_t r[3] = {0, 0x7FFFFFFF, 256};
  const int32_t* in[] = {l, r};
  uint8_t buf[18];
  bam::encode_l24(in, 2, 3, buf);
  CHECK(buf[0] == 0x12);
  CHECK(buf[1] == 0x34);
  CHECK(buf[2] == 0x56);
  int32_t dl[3], dr[3];
  int32_t* out[] = {dl, dr};
  bam::decode_pcm(bam::Encoding::L24, buf, 3, 2, out);
  for (int i = 0; i < 3; ++i) {
    CHECK(dl[i] == static_cast<int32_t>(static_cast<uint32_t>(l[i]) & 0xFFFFFF00u));
    CHECK(dr[i] == static_cast<int32_t>(static_cast<uint32_t>(r[i]) & 0xFFFFFF00u));
  }
}

TEST_CASE("unwrap RTP timestamp") {
  CHECK(bam::unwrap_rtp_timestamp(0xFFFFFFF0u, 0x100000010ull) == 0x0FFFFFFF0ull);
  CHECK(bam::unwrap_rtp_timestamp(0x10u, 0x0FFFFFFF0ull) == 0x100000010ull);
  CHECK(bam::unwrap_rtp_timestamp(900u, 1000ull) == 900ull);
  CHECK(bam::unwrap_rtp_timestamp(5u, 0x500000000ull) == 0x500000005ull);
  // reference near zero must not underflow
  CHECK(bam::unwrap_rtp_timestamp(0xFFFFFFF0u, 5ull) == 0xFFFFFFF0ull);
}
