// SPDX-License-Identifier: GPL-3.0-or-later
// ptp-sim: minimal two-step PTPv2 master for testing BounceAES Master without network hardware.
//   ptp-sim [--domain 0] [--iface 127.0.0.1] [--seconds 0]
// Sends Sync/Follow_Up 8x/s and Announce 1x/s, answers Delay_Req. Uses the system clock (TAI).
// Safety: runs on the loopback interface unless --iface is given, and announces the worst possible
// dataset (priority1 255, class 248), so it never wins master selection against a real grandmaster.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bam/auth.hpp"
#include "bam/constants.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "bam/ptp_messages.hpp"

namespace bp = bam::platform;

int main(int argc, char** argv) {
  int domain = 0;
  int seconds = 0;
  std::string iface = "127.0.0.1";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--domain" && i + 1 < argc) domain = std::atoi(argv[++i]);
    else if (a == "--iface" && i + 1 < argc) iface = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    else {
      std::printf("ptp-sim [--domain 0] [--iface 127.0.0.1] [--seconds 0]\n");
      return 2;
    }
  }
  bp::net_init();
  std::string err;
  auto ev = bp::UdpSocket::open(bam::kPtpEventPort, &err);
  auto gen = ev ? bp::UdpSocket::open(bam::kPtpGeneralPort, &err) : std::nullopt;
  if (!ev || !gen || !ev->join(bam::kPtpGroup, iface, "", &err) || !gen->join(bam::kPtpGroup, iface, "", &err)) {
    std::fprintf(stderr, "Nie można otworzyć gniazd PTP: %s\n", err.c_str());
    return 1;
  }
  ev->set_multicast_out(iface, 1, true);
  gen->set_multicast_out(iface, 1, true);

  bam::PortIdentity self;
  const std::string id = bam::random_hex(8);
  for (int i = 0; i < 8; ++i) self.clock.bytes[i] = static_cast<uint8_t>(std::stoi(id.substr(i * 2, 2), nullptr, 16));
  self.clock.bytes[3] = 0xFF;
  self.clock.bytes[4] = 0xFE;
  self.port = 1;
  bam::AnnounceBody announce;
  announce.priority1 = 255;
  announce.clock_class = 248;
  announce.priority2 = 255;
  announce.gm_identity = self.clock;
  std::printf("ptp-sim: grandmaster %s, domena %d, karta %s\n", self.clock.to_string().c_str(), domain, iface.c_str());

  auto tai_now = [] { return bp::realtime_ns() + 37LL * 1'000'000'000; };
  const auto d = static_cast<uint8_t>(domain);
  uint16_t seq = 0;
  const int64_t start = bp::monotonic_ns();
  int64_t next_sync = start;
  std::vector<uint8_t> buf(1500);
  std::vector<bp::UdpSocket*> socks = {&*ev};
  std::vector<uint8_t> ready;
  uint64_t answered = 0;

  while (seconds == 0 || bp::monotonic_ns() - start < int64_t(seconds) * 1'000'000'000) {
    const int64_t now = bp::monotonic_ns();
    if (now >= next_sync) {
      next_sync += 125'000'000;
      ++seq;
      const int64_t t1 = tai_now();
      ev->send_to(bam::build_sync(d, self, seq, true, bam::PtpTimestamp{}), bam::kPtpGroup, bam::kPtpEventPort);
      gen->send_to(bam::build_follow_up(d, self, seq, bam::PtpTimestamp::from_ns(t1)), bam::kPtpGroup, bam::kPtpGeneralPort);
      if (seq % 8 == 0) gen->send_to(bam::build_announce(d, self, seq, announce), bam::kPtpGroup, bam::kPtpGeneralPort);
      if (seq % 80 == 0) {
        std::printf("  %lld s: odpowiedzi Delay_Resp: %llu\n", static_cast<long long>((now - start) / 1'000'000'000),
                    static_cast<unsigned long long>(answered));
        std::fflush(stdout);
      }
    }
    bp::wait_readable(socks, 2, ready);
    int n;
    while ((n = ev->recv(buf, nullptr)) >= 0) {
      const int64_t t4 = tai_now();
      auto m = bam::parse_ptp(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)));
      if (!m || m->header.domain != d || m->header.type != bam::PtpMsgType::DelayReq) continue;
      gen->send_to(bam::build_delay_resp(d, self, m->header.sequence, bam::PtpTimestamp::from_ns(t4), m->header.source),
                   bam::kPtpGroup, bam::kPtpGeneralPort);
      ++answered;
    }
  }
  return 0;
}
