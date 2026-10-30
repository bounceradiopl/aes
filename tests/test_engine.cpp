// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "bam/asio_bridge.hpp"
#include "bam/engine.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"

namespace bp = bam::platform;

namespace {
int32_t ramp_value(uint64_t n, int ch) { return static_cast<int32_t>(((n * 3 + ch) & 0xFFFFFF) << 8); }

struct TestEngine {
  std::string suffix = std::to_string(bp::current_pid()) + "." + std::to_string(bp::monotonic_ns() % 100000);
  std::string config_path = "bam_test_config_" + suffix + ".json";
  bam::EngineOptions opts;
  std::unique_ptr<bam::Engine> engine;

  TestEngine() {
    opts.config_path = config_path;
    opts.shm_name = "bamtest.e." + suffix;
    opts.event_name = "bamtest.p." + suffix;
    opts.enable_ptp = false;
    opts.enable_sap = false;
    auto cfg = bam::default_config();
    cfg.interface_ip = "127.0.0.1";  // tests never send multicast onto a real network
    std::ofstream(config_path) << bam::config_to_json(cfg);
    engine = std::make_unique<bam::Engine>(opts);
  }
  ~TestEngine() {
    engine->stop();
    std::remove(config_path.c_str());
    bp::SharedMemory::remove(opts.shm_name);
    bp::NamedEvent::remove(opts.event_name);
  }
};
}  // namespace

TEST_CASE("engine loopback: output stream comes back sample-exact on the input") {
  TestEngine t;
  std::string err;
  REQUIRE_MESSAGE(t.engine->start(&err), err);

  auto cfg = t.engine->config();
  CHECK(cfg.devices[0].enabled);
  cfg.devices[0].tx_address = "239.69.201.1";
  auto r = t.engine->apply_config(cfg);
  REQUIRE(r.ok);
  cfg = t.engine->config();
  cfg.devices[0].rx_sdp = t.engine->output_sdp(1);
  CHECK(cfg.devices[0].rx_sdp.find("c=IN IP4 239.69.201.1/15") != std::string::npos);
  r = t.engine->apply_config(cfg);
  REQUIRE(r.ok);

  // Fake ASIO client
  auto shm = bp::SharedMemory::open(t.opts.shm_name, bam::ShmLayout::total_bytes(), &err);
  REQUIRE_MESSAGE(shm, err);
  auto ev = bp::NamedEvent::open(t.opts.event_name, &err);
  REQUIRE_MESSAGE(ev, err);
  bam::ShmView view(shm->data());
  REQUIRE(view.compatible());
  auto& h = view.header();
  uint32_t expected = 0;
  REQUIRE(h.client_pid.compare_exchange_strong(expected, bp::current_pid()));
  h.buffer_frames = 128;
  h.client_running = 1;

  bam::AsioBridge bridge(view);
  std::vector<std::vector<int32_t>> out(bam::kChannelCount, std::vector<int32_t>(128));
  std::vector<const int32_t*> out_ptrs(bam::kChannelCount, nullptr);
  out_ptrs[0] = out[0].data();
  out_ptrs[1] = out[1].data();
  uint64_t last_counter = h.period_counter.load();
  uint64_t last_start = 0;
  int periods = 0;
  int skipped = 0;
  const int64_t end = bp::monotonic_ns() + 1'500'000'000;
  while (bp::monotonic_ns() < end) {
    if (!ev->wait(50)) continue;
    const uint64_t c = h.period_counter.load();
    if (c == last_counter) continue;
    last_counter = c;
    const uint64_t s = h.period_start_sample.load();
    // The service must advance exactly one buffer per period; this non-realtime test client may
    // occasionally miss an event, in which case it writes the skipped buffers as well.
    uint64_t first = s;
    if (last_start) {
      CHECK((s - last_start) % 128 == 0);
      if (s - last_start > 128) ++skipped;
      first = last_start + 128;
    }
    for (uint64_t p = first; p <= s; p += 128) {
      for (int ch = 0; ch < 2; ++ch)
        for (uint32_t i = 0; i < 128; ++i) out[ch][i] = ramp_value(bam::AsioBridge::output_start(p, 128) + i, ch);
      bridge.write_outputs(p, 128, out_ptrs.data());
    }
    last_start = s;
    h.client_heartbeat.fetch_add(1);
    ++periods;
  }
  MESSAGE("periods: " << periods << ", missed by the test client: " << skipped);
  CHECK(periods > 450);  // 1.5 s / 2.67 ms ≈ 562
  CHECK(skipped < periods / 20);

  // 10 000 consecutive samples, well inside the written region and past the rx latency
  const uint64_t from = last_start - 30000;
  std::vector<int32_t> l(10000), rr(10000);
  view.input(0).read(from, l.data(), l.size());
  view.input(1).read(from, rr.data(), rr.size());
  int mismatches = 0;
  for (size_t i = 0; i < l.size(); ++i) {
    if (l[i] != ramp_value(from + i, 0) || rr[i] != ramp_value(from + i, 1)) ++mismatches;
  }
  CHECK(mismatches == 0);

  auto st = t.engine->status();
  CHECK(st.asio_connected);
  CHECK(st.asio_buffer_frames == 128);
  const auto& d = st.devices[0];
  MESSAGE("rx packets " << d.rx.packets << " underruns " << d.rx.underruns << " seq " << d.rx.seq_errors
                        << " overruns " << d.rx.overruns << " jitter " << d.rx.jitter_ms << " ms, tx " << d.tx.packets
                        << " err " << d.tx.errors);
  CHECK(d.rx_signal);
  CHECK(d.rx.packets > 1000);
  CHECK(d.rx.underruns == 0);
  CHECK(d.rx.seq_errors == 0);
  CHECK(d.tx.errors == 0);
  CHECK(d.in_peak_db[0] > -60.0f);  // the test ramp is present (its level depends on the window)
  CHECK(d.out_peak_db[0] > -60.0f);
  CHECK(d.in_peak_db[1] > -60.0f);
  CHECK(st.devices[1].in_peak_db[0] == doctest::Approx(-120.0f));

  h.client_running = 0;
  h.client_pid = 0;
}

TEST_CASE("engine receives more inputs on one port than a socket may join") {
  TestEngine t;
  std::string err;
  REQUIRE_MESSAGE(t.engine->start(&err), err);
  constexpr int kDevices = 24;  // Linux allows 20 memberships per socket
  auto cfg = t.engine->config();
  for (int i = 0; i < kDevices; ++i) {
    cfg.devices[i].enabled = true;
    cfg.devices[i].tx_address = "239.69.202." + std::to_string(i + 1);
  }
  REQUIRE(t.engine->apply_config(cfg).ok);
  cfg = t.engine->config();
  for (int i = 0; i < kDevices; ++i) cfg.devices[i].rx_sdp = t.engine->output_sdp(i + 1);
  REQUIRE(t.engine->apply_config(cfg).ok);
  bp::sleep_ns(800'000'000);
  const auto st = t.engine->status();
  for (int i = 0; i < kDevices; ++i) {
    INFO("device " << i + 1 << " error: " << st.devices[i].rx_error);
    CHECK(st.devices[i].rx_error.empty());
    CHECK(st.devices[i].rx.packets > 300);
    CHECK(st.devices[i].rx.seq_errors == 0);
  }
}

TEST_CASE("engine rejects invalid configuration and keeps the running one") {
  TestEngine t;
  std::string err;
  REQUIRE_MESSAGE(t.engine->start(&err), err);
  auto cfg = t.engine->config();
  cfg.devices[1].tx_ptime_us = 999;
  auto r = t.engine->apply_config(cfg);
  CHECK_FALSE(r.ok);
  REQUIRE_FALSE(r.issues.empty());
  CHECK(r.issues[0].device == 2);
  CHECK(t.engine->config().devices[1].tx_ptime_us == 1000);
}

TEST_CASE("engine persists configuration and keeps the password hash") {
  TestEngine t;
  std::string err;
  REQUIRE_MESSAGE(t.engine->start(&err), err);
  REQUIRE(t.engine->set_password_hash("pbkdf2-sha256$1$00$00"));
  auto cfg = t.engine->config();
  cfg.devices[2].name = "Kraków";
  cfg.password_hash = "";
  REQUIRE(t.engine->apply_config(cfg).ok);
  t.engine->stop();

  bam::Engine again(t.opts);
  REQUIRE_MESSAGE(again.start(&err), err);
  CHECK(again.config().devices[2].name == "Kraków");
  CHECK(again.config().password_hash == "pbkdf2-sha256$1$00$00");
  again.stop();
}
