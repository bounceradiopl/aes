// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "bam/ptp_messages.hpp"
#include "bam/ptp_slave.hpp"

namespace {

constexpr int64_t kT0 = 1'700'000'000'000'000'000;
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kS = 1'000'000'000;

bam::ClockIdentity id(uint8_t last) {
  bam::ClockIdentity c;
  c.bytes = {0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x00, 0x00, last};
  return c;
}

bam::PtpLocal local(bam::PtpRole role, int priority1) {
  bam::PtpLocal l;
  l.role = role;
  l.announce.priority1 = static_cast<uint8_t>(priority1);
  l.announce.priority2 = 128;
  l.announce.clock_class = 248;
  return l;
}

// A computer: a PtpPort on a local oscillator with its own drift and offset.
struct Node {
  double drift;
  int64_t local0;
  std::unique_ptr<bam::PtpPort> port;
  int64_t local(int64_t t) const { return local0 + std::llround(static_cast<double>(t - kT0) * (1.0 + drift)); }
  int64_t ptp(int64_t t) const { return port->clock().to_ptp(local(t)); }
};

// A hardware master (Orban, Omnia, console): perfect clock = true time, priority1 128.
struct HardMaster {
  bam::PortIdentity port;
  bam::AnnounceBody announce;
  bool on = false;
  uint16_t seq = 0;
  int64_t next_sync = 0, next_announce = 0;
};

struct Msg {
  int64_t arrive;
  int from;  // node index, or -1 for the hard master
  std::vector<uint8_t> bytes;
};

struct Bus {
  std::vector<Node> nodes;
  HardMaster hm;
  std::deque<Msg> inflight;
  std::mt19937 rng{7};
  int64_t now = kT0;

  int64_t delay() { return 100'000 + std::uniform_int_distribution<int64_t>(0, 50'000)(rng); }
  void send(int from, std::vector<uint8_t> bytes) { inflight.push_back({now + delay(), from, std::move(bytes)}); }

  void step() {
    now += kMs;
    for (size_t i = 0; i < nodes.size(); ++i) {
      auto& n = nodes[i];
      const int64_t l = n.local(now);
      for (auto& out : n.port->poll_master(l)) {
        if (out.needs_tx_time) n.port->on_sync_sent(out.sync_seq, l);
        send(static_cast<int>(i), std::move(out.bytes));
      }
      if (auto req = n.port->poll_delay_req(l)) {
        n.port->on_delay_req_sent(l);
        send(static_cast<int>(i), std::move(*req));
      }
      n.port->tick(l);
    }
    if (hm.on) {
      if (now >= hm.next_announce) {
        send(-1, bam::build_announce(0, hm.port, hm.seq, hm.announce));
        hm.next_announce = now + kS;
      }
      if (now >= hm.next_sync) {
        ++hm.seq;
        send(-1, bam::build_sync(0, hm.port, hm.seq, true, bam::PtpTimestamp{}));
        send(-1, bam::build_follow_up(0, hm.port, hm.seq, bam::PtpTimestamp::from_ns(now)));
        hm.next_sync = now + 125 * kMs;
      }
    }
    while (!inflight.empty() && inflight.front().arrive <= now) {
      Msg m = std::move(inflight.front());
      inflight.pop_front();
      // the hard master answers Delay_Req with its true receive time
      if (hm.on && m.from >= 0) {
        auto parsed = bam::parse_ptp(m.bytes);
        if (parsed && parsed->header.type == bam::PtpMsgType::DelayReq)
          send(-1, bam::build_delay_resp(0, hm.port, parsed->header.sequence, bam::PtpTimestamp::from_ns(m.arrive),
                                         parsed->header.source));
      }
      for (size_t j = 0; j < nodes.size(); ++j) {
        if (static_cast<int>(j) == m.from) continue;
        nodes[j].port->on_message(m.bytes, nodes[j].local(m.arrive));
      }
    }
  }
  void run(int64_t ns) {
    const int64_t end = now + ns;
    while (now < end) step();
  }
  int masters() const {
    int c = 0;
    for (const auto& n : nodes) c += n.port->status().state == bam::PtpState::Master;
    return c;
  }
};

Bus two_computers(bam::PtpRole role) {
  Bus b;
  b.nodes.push_back({40e-6, 5 * kS, nullptr});
  b.nodes.push_back({-30e-6, 9 * kS, nullptr});
  b.nodes[0].port = std::make_unique<bam::PtpPort>(0, id(1), bam::MediaClock{}, local(role, 250));
  b.nodes[1].port = std::make_unique<bam::PtpPort>(0, id(2), bam::MediaClock{}, local(role, 250));
  b.hm.port.clock = id(0x90);
  b.hm.port.port = 1;
  b.hm.announce.priority1 = 128;
  b.hm.announce.clock_class = 6;
  b.hm.announce.gm_identity = b.hm.port.clock;
  return b;
}

}  // namespace

TEST_CASE("two computers in auto mode elect exactly one master and stay in sync") {
  Bus b = two_computers(bam::PtpRole::Auto);
  b.run(5 * kS);
  CHECK(b.masters() == 1);
  CHECK(b.nodes[0].port->status().state == bam::PtpState::Master);  // lower clock identity wins the tie
  b.run(55 * kS);
  CHECK(b.masters() == 1);
  CHECK(b.nodes[1].port->status().state == bam::PtpState::Locked);
  CHECK(b.nodes[1].port->status().master.clock == id(1));
  const int64_t diff = std::llabs(b.nodes[0].ptp(b.now) - b.nodes[1].ptp(b.now));
  MESSAGE("A/B difference after 60 s: " << diff << " ns");
  CHECK(diff < 50'000);
}

TEST_CASE("slave-only computers never transmit a clock") {
  Bus b = two_computers(bam::PtpRole::Slave);
  b.run(10 * kS);
  CHECK(b.masters() == 0);
}

TEST_CASE("a hardware master takes over and the computer resumes when it disappears") {
  Bus b = two_computers(bam::PtpRole::Auto);
  b.run(10 * kS);
  REQUIRE(b.nodes[0].port->status().state == bam::PtpState::Master);

  b.hm.on = true;
  b.hm.next_announce = b.now;
  b.hm.next_sync = b.now;
  b.run(1 * kS);
  CHECK(b.masters() == 0);  // computer A stepped down within 1 s

  b.run(20 * kS);
  for (const auto& n : b.nodes) {
    CHECK(n.port->status().state == bam::PtpState::Locked);
    CHECK(n.port->status().master.clock == id(0x90));
    CHECK(std::llabs(n.ptp(b.now) - b.now) < 50'000);  // following the hardware clock (true time)
  }

  b.hm.on = false;
  b.run(12 * kS);  // foreign master timeout (6 s) + qualification (3 s)
  CHECK(b.masters() == 1);
  CHECK(b.nodes[0].port->status().state == bam::PtpState::Master);
}

TEST_CASE("switching a master to slave-only stops it") {
  Bus b = two_computers(bam::PtpRole::Auto);
  b.run(5 * kS);
  REQUIRE(b.nodes[0].port->status().state == bam::PtpState::Master);
  b.nodes[0].port->set_local(local(bam::PtpRole::Slave, 250));
  b.run(100 * kMs);
  CHECK(b.nodes[0].port->status().state != bam::PtpState::Master);
}
