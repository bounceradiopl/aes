// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <random>

#include "bam/ptp_messages.hpp"
#include "bam/ptp_slave.hpp"

namespace {

bam::PortIdentity make_port(uint8_t id) {
  bam::PortIdentity p;
  p.clock.bytes = {0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x00, 0x00, id};
  p.port = 1;
  return p;
}

// Discrete-event simulation of one master and one software-timestamping slave.
struct Sim {
  int64_t t0 = 1'700'000'000'000'000'000;  // true (master) time at start
  int64_t true_ns = t0;
  double drift;          // slave oscillator error: local runs (1 + drift) times faster
  int64_t local0 = 5'000'000'000;
  std::mt19937 rng{1234};
  uint8_t domain = 0;
  bam::PortIdentity master = make_port(1);
  bam::AnnounceBody announce;
  uint16_t seq = 0;
  bool master_on = true;
  bam::PtpSlave slave;
  std::optional<std::pair<uint16_t, int64_t>> pending_delay;  // seq, true send time

  explicit Sim(double drift_ppm, uint8_t slave_domain = 0)
      : drift(drift_ppm * 1e-6), slave(slave_domain, make_port(99).clock, bam::MediaClock{}) {
    announce.priority1 = 128;
    announce.clock_class = 6;
    announce.gm_identity = master.clock;
  }

  int64_t local(int64_t t) const { return local0 + std::llround(static_cast<double>(t - t0) * (1.0 + drift)); }
  int64_t delay() { return 100'000 + std::uniform_int_distribution<int64_t>(0, 50'000)(rng); }
  int64_t error() const { return slave.clock().to_ptp(local(true_ns)) - true_ns; }

  // Advance 125 ms: sync + follow-up, announce each second, one delay exchange per second.
  void step() {
    true_ns += 125'000'000;
    const int64_t now = true_ns;
    if (master_on) {
      ++seq;
      const int64_t d = delay();
      slave.on_message(bam::build_sync(domain, master, seq, true, bam::PtpTimestamp{}), local(now + d));
      slave.on_message(bam::build_follow_up(domain, master, seq, bam::PtpTimestamp::from_ns(now)), local(now + d + 20'000));
      if (seq % 8 == 0) slave.on_message(bam::build_announce(domain, master, seq, announce), local(now + d + 30'000));
    }
    const int64_t req_time = now + 40'000'000;
    if (auto req = slave.poll_delay_req(local(req_time))) {
      slave.on_delay_req_sent(local(req_time));
      auto parsed = bam::parse_ptp(*req);
      if (master_on && parsed) {
        const int64_t arrive = req_time + delay();
        auto resp = bam::build_delay_resp(domain, master, parsed->header.sequence, bam::PtpTimestamp::from_ns(arrive),
                                          parsed->header.source);
        slave.on_message(resp, local(arrive + delay()));
      }
    }
    slave.tick(local(now + 100'000'000));
  }

  void run_seconds(double s) {
    for (int i = 0; i < static_cast<int>(s * 8); ++i) step();
  }
};

}  // namespace

TEST_CASE("PTP slave locks with +100 ppm drift and 3 s initial offset") {
  Sim sim(100.0);
  sim.local0 = 3'000'000'000;
  CHECK(sim.slave.status().state == bam::PtpState::Listening);
  sim.run_seconds(20);
  CHECK(sim.slave.status().state == bam::PtpState::Locked);
  CHECK(sim.slave.status().has_master);
  CHECK(sim.slave.status().grandmaster == sim.master.clock);
  int64_t worst = 0;
  for (int i = 0; i < 60 * 8; ++i) {
    sim.step();
    worst = std::max<int64_t>(worst, std::llabs(sim.error()));
  }
  MESSAGE("worst error after lock: " << worst << " ns, rate ppm " << sim.slave.status().rate_ppm);
  CHECK(worst < 50'000);
  CHECK(sim.slave.status().steps == 1);
  CHECK(sim.slave.status().path_delay_ns > 80'000);
  CHECK(sim.slave.status().path_delay_ns < 170'000);
  CHECK(sim.slave.consume_step());
  CHECK_FALSE(sim.slave.consume_step());
}

TEST_CASE("PTP slave converges with -80 ppm drift") {
  Sim sim(-80.0);
  sim.run_seconds(30);
  CHECK(sim.slave.status().state == bam::PtpState::Locked);
  CHECK(std::llabs(sim.error()) < 50'000);
}

TEST_CASE("holdover when the master disappears, relock without step") {
  Sim sim(60.0);
  sim.run_seconds(40);
  REQUIRE(sim.slave.status().state == bam::PtpState::Locked);
  sim.master_on = false;
  sim.run_seconds(5);
  CHECK(sim.slave.status().state == bam::PtpState::Holdover);
  CHECK(sim.slave.status().sync_lost == 1);
  CHECK(std::llabs(sim.error()) < 100'000);  // frequency estimate alone keeps 5 s of holdover well under 1 ms
  sim.master_on = true;
  sim.run_seconds(10);
  CHECK(sim.slave.status().state == bam::PtpState::Locked);
  CHECK(sim.slave.status().steps == 1);
}

TEST_CASE("better master is selected") {
  Sim sim(0.0);
  sim.run_seconds(5);
  REQUIRE(sim.slave.status().master == sim.master);
  bam::AnnounceBody better = sim.announce;
  better.priority1 = 10;
  better.gm_identity = make_port(2).clock;
  sim.slave.on_message(bam::build_announce(0, make_port(2), 1, better), sim.local(sim.true_ns));
  CHECK(sim.slave.status().master == make_port(2));
  CHECK(sim.slave.status().grandmaster == make_port(2).clock);
}

TEST_CASE("messages from another domain are ignored") {
  Sim sim(0.0, 5);
  sim.run_seconds(5);
  CHECK(sim.slave.status().state == bam::PtpState::Listening);
  CHECK_FALSE(sim.slave.status().has_master);
  CHECK(sim.slave.status().steps == 0);
}
