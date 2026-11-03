// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <map>
#include <vector>

#include "bam/asio_bridge.hpp"
#include "bam/bridge_protocol.hpp"
#include "bam/engine.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"
#include "test_util.hpp"

namespace bp = bam::platform;

namespace {
int32_t ramp_value(uint64_t n, int ch) { return static_cast<int32_t>(((n * 3 + ch) & 0xFFFFFF) << 8); }

// What a 0 dB mix of two ramp channels carries: their sum, saturated like the mixer does.
// The ramps span the whole int32 range, so about half of the ramp cycle clips.
int32_t mix_sum(uint64_t n, int ch_a, int ch_b) {
  const int64_t s = static_cast<int64_t>(ramp_value(n, ch_a)) + ramp_value(n, ch_b);
  return static_cast<int32_t>(std::clamp<int64_t>(s, INT32_MIN, INT32_MAX));
}

// A sample after an AES67 L24 hop: the low byte is gone.
int32_t l24(int32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v) & 0xFFFFFF00u); }

constexpr uint32_t kRxDelay = 240;  // default subscription latency 5 ms

struct TestEngine {
  std::string suffix = std::to_string(bp::current_pid()) + "." + std::to_string(bp::monotonic_ns() % 100000);
  std::string config_path = "bam_test_config_" + suffix + ".json";
  bam::EngineOptions opts;
  std::unique_ptr<bam::Engine> engine;

  explicit TestEngine(const std::string& config_text = "") {
    opts.config_path = config_path;
    opts.shm_name = "bamtest.e." + suffix;
    opts.event_name = "bamtest.p." + suffix;
    opts.enable_ptp = false;
    opts.enable_sap = false;
    if (config_text.empty()) {
      auto cfg = bam::default_config();
      cfg.interface_ip = "127.0.0.1";  // tests never send multicast onto a real network
      cfg.station_name = "TEST";
      std::ofstream(config_path) << bam::config_to_json(cfg);
    } else {
      std::ofstream(config_path) << config_text;
    }
    engine = std::make_unique<bam::Engine>(opts);
  }
  ~TestEngine() {
    engine->stop();
    std::remove(config_path.c_str());
    std::remove((config_path + ".v1.bak").c_str());
    bp::SharedMemory::remove(opts.shm_name);
    bp::NamedEvent::remove(opts.event_name);
  }

  void start() {
    std::string err;
    REQUIRE_MESSAGE(engine->start(&err), err);
  }
  void set_output(int n, const std::string& address) {
    auto cfg = engine->config();
    cfg.outputs[n - 1].enabled = true;
    cfg.outputs[n - 1].address = address;
    REQUIRE(engine->apply_config(cfg).ok);
  }
  int subscribe_to_output_of(const bam::Engine& sender, int n) {
    auto r = engine->add_subscription(sender.output_sdp(n));
    REQUIRE(r.ok);
    return r.created;
  }
  int subscribe_to_output(int n) {
    auto r = engine->add_subscription(engine->output_sdp(n));
    REQUIRE(r.ok);
    return r.created;
  }
  void route(const std::string& dst, const std::string& src) { REQUIRE(engine->set_route(dst, src).ok); }
};

// A fake ASIO host: attaches to the shared timeline and plays ramp_value() on ASIO output pairs 1 and 2.
struct FakeAsioHost {
  std::optional<bp::SharedMemory> shm;
  std::optional<bp::NamedEvent> ev;
  std::unique_ptr<bam::ShmView> view;
  uint64_t last_start = 0;
  int periods = 0;
  int skipped = 0;
  std::vector<std::pair<uint64_t, uint64_t>> late;  // output sample ranges written after their period

  explicit FakeAsioHost(const TestEngine& t) {
    std::string err;
    shm = bp::SharedMemory::open(t.opts.shm_name, bam::ShmLayout::total_bytes(), &err);
    REQUIRE_MESSAGE(shm, err);
    ev = bp::NamedEvent::open(t.opts.event_name, &err);
    REQUIRE_MESSAGE(ev, err);
    view = std::make_unique<bam::ShmView>(shm->data());
    REQUIRE(view->compatible());
    auto& h = view->header();
    uint32_t expected = 0;
    REQUIRE(h.client_pid.compare_exchange_strong(expected, bp::current_pid()));
    h.buffer_frames = 128;
    h.client_running = 1;
  }
  ~FakeAsioHost() {
    view->header().client_running = 0;
    view->header().client_pid = 0;
  }

  // per_buffer(p): called for every buffer start p the host plays, e.g. to feed other rings in step
  void run(int64_t ns, const std::function<void(uint64_t)>& per_buffer = {}) {
    auto& h = view->header();
    bam::AsioBridge bridge(*view);
    std::vector<std::vector<int32_t>> out(4, std::vector<int32_t>(128));
    std::vector<const int32_t*> out_ptrs(bam::kChannelCount, nullptr);
    for (int ch = 0; ch < 4; ++ch) out_ptrs[ch] = out[ch].data();  // pairs 1 and 2
    uint64_t last_counter = h.period_counter.load();
    const int64_t end = bp::monotonic_ns() + ns;
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
        if (s - last_start > 128) {
          ++skipped;
          late.emplace_back(bam::AsioBridge::output_start(last_start + 128, 128), bam::AsioBridge::output_start(s, 128) + 128);
        }
        first = last_start + 128;
      }
      for (uint64_t p = first; p <= s; p += 128) {
        for (int ch = 0; ch < 4; ++ch)
          for (uint32_t i = 0; i < 128; ++i) out[ch][i] = ramp_value(bam::AsioBridge::output_start(p, 128) + i, ch);
        bridge.write_outputs(p, 128, out_ptrs.data());
        if (per_buffer) per_buffer(p);
      }
      last_start = s;
      h.client_heartbeat.fetch_add(1);
      ++periods;
    }
  }

  // Mismatches of `ring pair` against the ramp delayed by `delay`, over 10 000 samples well inside the written region.
  int mismatches_net_in(int pair, uint64_t delay) const {
    const uint64_t from = last_start - 30000;
    std::vector<int32_t> l(10000), r(10000);
    view->net_in(pair * 2).read(from, l.data(), l.size());
    view->net_in(pair * 2 + 1).read(from, r.data(), r.size());
    int bad = 0;
    for (size_t i = 0; i < l.size(); ++i)
      if (l[i] != ramp_value(from + i - delay, 0) || r[i] != ramp_value(from + i - delay, 1)) ++bad;
    return bad;
  }

  // What an ASIO host recording from input pair `pair` (0-based) gets for the period at last_start.
  std::vector<int32_t> asio_input(int pair, int side) const {
    bam::AsioBridge bridge(*view);
    std::vector<int32_t> buf(128);
    std::vector<int32_t*> ins(bam::kChannelCount, nullptr);
    ins[pair * 2 + side] = buf.data();
    bridge.read_inputs(last_start, 128, ins.data());
    return buf;
  }
};
}  // namespace

TEST_CASE("engine loopback: output 1 <- asio-out:1, subscription 1 = output 1, asio-in:1 <- net-in:1") {
  TestEngine t;
  t.start();
  CHECK(t.engine->config().outputs[0].enabled);
  CHECK(t.engine->config().routes.at("net-out:1") == "asio-out:1");
  t.set_output(1, "239.69.201.1");
  const std::string sdp = t.engine->output_sdp(1);
  CHECK(sdp.find("c=IN IP4 239.69.201.1/15") != std::string::npos);
  CHECK(sdp.find("s=TEST / BounceAES 01") != std::string::npos);
  REQUIRE(t.subscribe_to_output(1) == 1);
  t.route("asio-in:1", "net-in:1");

  FakeAsioHost host(t);
  host.run(1'500'000'000);
  MESSAGE("periods: " << host.periods << ", missed by the test client: " << host.skipped);
  CHECK(host.periods > 450);  // 1.5 s / 2.67 ms ≈ 562
  CHECK(host.skipped < host.periods / 20);

  CHECK(host.mismatches_net_in(0, 0) == 0);  // the subscription lands at its RTP timestamps

  // ASIO input pair 1 reads net-in:1 at S - B - D_src
  auto rec = host.asio_input(0, 1);
  int bad = 0;
  for (uint32_t i = 0; i < 128; ++i) bad += rec[i] != ramp_value(host.last_start - 128 - kRxDelay + i, 1);
  CHECK(bad == 0);

  auto st = t.engine->status();
  CHECK(st.asio_connected);
  CHECK(st.asio_buffer_frames == 128);
  CHECK(st.station_name == "TEST");
  const auto& o = st.outputs[0];
  const auto& s = st.subscriptions[0];
  MESSAGE("rx packets " << s.rx.packets << " underruns " << s.rx.underruns << " seq " << s.rx.seq_errors << " overruns "
                        << s.rx.overruns << " jitter " << s.rx.jitter_ms << " ms, tx " << o.tx.packets << " err " << o.tx.errors);
  CHECK(o.source == "asio-out:1");
  CHECK(o.tx_active);
  CHECK(s.signal);
  CHECK(s.rx.packets > 1000);
  CHECK(s.rx.underruns == 0);
  CHECK(s.rx.seq_errors == 0);
  CHECK(o.tx.errors == 0);
  CHECK(s.peak_db[0] > -60.0f);  // the test ramp is present (its level depends on the window)
  CHECK(o.peak_db[0] > -60.0f);
  CHECK(st.subscriptions[1].peak_db[0] == doctest::Approx(-120.0f));
}

TEST_CASE("engine forwards a subscription to another output: net-out:2 <- net-in:1") {
  TestEngine t;
  t.start();
  t.set_output(1, "239.69.203.1");
  t.set_output(2, "239.69.203.2");
  REQUIRE(t.subscribe_to_output(1) == 1);
  t.route("net-out:2", "net-in:1");
  REQUIRE(t.subscribe_to_output(2) == 2);

  FakeAsioHost host(t);
  host.run(1'500'000'000);
  CHECK(host.mismatches_net_in(0, 0) == 0);
  // output 2 carries subscription 1 delayed by its D_src
  CHECK(host.mismatches_net_in(1, kRxDelay) == 0);
  auto st = t.engine->status();
  CHECK(st.outputs[1].source == "net-in:1");
  CHECK(st.outputs[1].tx_active);
  CHECK(st.subscriptions[1].rx.seq_errors == 0);
}

TEST_CASE("engine local ASIO loopback: asio-in:3 <- asio-out:1 at no delay, rerouted while running") {
  TestEngine t;
  t.start();
  FakeAsioHost host(t);
  host.run(300'000'000);
  t.route("asio-in:3", "asio-out:1");
  host.run(300'000'000);
  auto rec = host.asio_input(2, 0);
  int bad = 0;
  for (uint32_t i = 0; i < 128; ++i) bad += rec[i] != ramp_value(host.last_start - 128 + i, 0);
  CHECK(bad == 0);

  REQUIRE(t.engine->set_route("asio-in:3", std::nullopt).ok);
  rec = host.asio_input(2, 0);
  for (auto v : rec) CHECK(v == 0);
}

TEST_CASE("engine with a (fake) audio bridge: win-app:test.exe -> net-out:1, request and report") {
  TestEngine t;
  t.start();
  {
    auto v = t.engine->routing();
    for (const auto& n : v.sources) CHECK(n.group != "win");  // no bridge: nothing from Windows
  }
  t.set_output(1, "239.69.205.1");
  t.route("net-out:1", "win-app:Test.exe");
  REQUIRE(t.subscribe_to_output(1) == 1);

  FakeAsioHost host(t);
  auto& h = host.view->header();
  h.bridge_pid = bp::current_pid();
  bam::BridgeReport report;
  report.generation = 1;
  report.endpoints.push_back({"{0.0.0.00000000}.{spk}", "Głośniki", true, true});
  report.endpoints.push_back({"{0.0.1.00000000}.{mic}", "Mikrofon", false, true});
  report.apps.push_back({"test.exe", "Test", 4242});
  report.slots.push_back({"win-app:test.exe", true, "", {-6, -6}});
  host.view->write_report(bam::to_json(report));

  uint32_t req_version = 0;
  auto req = bam::request_from_json(host.view->read_request(&req_version));
  REQUIRE(req);
  REQUIRE(req->sources.size() == 1);
  CHECK(req->sources[0].slot == 0);
  CHECK(req->sources[0].id == "win-app:test.exe");
  const uint64_t first_generation = req->generation;
  CHECK(h.win_src_delay_frames.load() == 960);

  // The bridge writes the application's audio into win_src pair 0 on the media clock; here it
  // is fed in step with the fake ASIO host's buffers and heartbeats like the real bridge.
  std::vector<int32_t> l(128), r(128);
  host.run(1'500'000'000, [&](uint64_t p) {
    for (uint32_t i = 0; i < 128; ++i) {
      l[i] = ramp_value(p + i, 0);
      r[i] = ramp_value(p + i, 1);
    }
    host.view->win_src(0).write(p, l.data(), 128);
    host.view->win_src(1).write(p, r.data(), 128);
    h.bridge_heartbeat.fetch_add(1);
  });
  CHECK(host.mismatches_net_in(0, 960) == 0);  // output 1 carries the app D_src (20 ms) behind

  auto v = t.engine->routing();
  std::map<std::string, bam::RouteNode> nodes;
  for (const auto& n : v.sources) nodes[n.id] = n;
  for (const auto& n : v.dests) nodes["dst " + n.id] = n;
  REQUIRE(nodes.count("win-app:test.exe"));
  CHECK(nodes["win-app:test.exe"].available);
  CHECK(nodes["win-app:test.exe"].name == "Test");
  CHECK(nodes.count("win-loop:{0.0.0.00000000}.{spk}"));
  CHECK(nodes.count("win-in:{0.0.1.00000000}.{mic}"));
  CHECK(nodes.count("dst win-out:{0.0.0.00000000}.{spk}"));
  CHECK(t.engine->status().bridge_connected);

  // a Windows destination is announced to the bridge with its source ring
  t.route("win-out:{0.0.0.00000000}.{spk}", "net-in:1");
  req = bam::request_from_json(host.view->read_request(nullptr));
  REQUIRE(req);
  REQUIRE(req->dests.size() == 1);
  CHECK(req->dests[0].kind == bam::ShmSource::Net);
  CHECK(req->dests[0].pair == 0);
  CHECK(req->dests[0].delay_frames == kRxDelay);
  CHECK(req->generation == first_generation + 1);
  h.bridge_pid = 0;
}

TEST_CASE("engine routing view: virtual sound cards are labelled, listed first and paired") {
  TestEngine t;
  t.start();
  FakeAsioHost host(t);
  auto& h = host.view->header();
  h.bridge_pid = bp::current_pid();
  const std::string cable_in = "{0.0.0.00000000}.{cin}", cable_out = "{0.0.1.00000000}.{cout}";
  bam::BridgeReport report;
  report.generation = 1;
  report.endpoints.push_back({"{0.0.0.00000000}.{spk}", "Głośniki (Realtek(R) Audio)", true, true});
  report.endpoints.push_back({cable_in, "CABLE Input (VB-Audio Virtual Cable)", true, false});
  report.endpoints.push_back({cable_out, "CABLE Output (VB-Audio Virtual Cable)", false, false});
  report.endpoints.push_back({"{0.0.1.00000000}.{mic}", "Mikrofon (USB Audio CODEC)", false, true});
  report.endpoints.push_back({"{0.0.0.00000000}.{vad}", "Speakers (Virtual Audio Driver)", true, false});
  host.view->write_report(bam::to_json(report));
  host.run(300'000'000, [&](uint64_t) { h.bridge_heartbeat.fetch_add(1); });

  auto win_nodes = [](const std::vector<bam::RouteNode>& list) {
    std::vector<bam::RouteNode> out;
    for (const auto& n : list)
      if (n.group == "win") out.push_back(n);
    return out;
  };
  auto find = [](const std::vector<bam::RouteNode>& list, const std::string& id) -> const bam::RouteNode* {
    for (const auto& n : list)
      if (n.id == id) return &n;
    return nullptr;
  };
  auto v = t.engine->routing();
  auto src = win_nodes(v.sources), dst = win_nodes(v.dests);
  REQUIRE(src.size() == 4);  // two loopbacks of render ends, the VAD loopback, the USB microphone
  CHECK(src[0].id == "win-loop:" + cable_in);
  CHECK(src[0].name == "CABLE: z aplikacji");
  CHECK(src[0].virtual_device == "VB-CABLE");
  CHECK(src[1].name == "Speakers (Virtual Audio Driver): z aplikacji");
  CHECK(src[1].virtual_device == "Virtual Audio Driver");
  CHECK(find(src, "win-in:" + cable_out) == nullptr);  // the cable's other end is the same audio
  REQUIRE(find(src, "win-in:{0.0.1.00000000}.{mic}"));
  CHECK(find(src, "win-in:{0.0.1.00000000}.{mic}")->virtual_device.empty());
  REQUIRE(find(src, "win-loop:{0.0.0.00000000}.{spk}"));
  CHECK(find(src, "win-loop:{0.0.0.00000000}.{spk}")->name == "Odsłuch: Głośniki (Realtek(R) Audio)");
  REQUIRE(!dst.empty());
  CHECK(dst[0].id == "win-out:" + cable_in);
  CHECK(dst[0].name == "CABLE: do aplikacji");
  CHECK(dst[0].virtual_device == "VB-CABLE");

  // something already routed from the capture end stays visible and usable
  t.set_output(1, "239.69.205.2");
  t.route("net-out:1", "win-in:" + cable_out);
  v = t.engine->routing();
  const auto* routed = find(v.sources, "win-in:" + cable_out);
  REQUIRE(routed);
  CHECK(routed->name == "CABLE: wyjście kabla");
  CHECK(routed->available);
  h.bridge_pid = 0;
}

TEST_CASE("engine adds an output for a receiver (Orban, Omnia) and removes it") {
  TestEngine t;
  t.start();
  // output 1 is enabled by default; the new one takes the first free slot
  bam::NewOutput req;
  req.name = "Orban Warszawa";
  req.source = "asio-out:3";
  auto r = t.engine->add_output(req);
  REQUIRE_MESSAGE(r.ok, (r.issues.empty() ? "" : r.issues[0].message));
  CHECK(r.created == 2);
  auto cfg = t.engine->config();
  CHECK(cfg.outputs[1].enabled);
  CHECK(cfg.outputs[1].name == "Orban Warszawa");
  CHECK(cfg.outputs[1].ptime_us == 1000);
  CHECK(cfg.outputs[1].address != cfg.outputs[0].address);
  CHECK(cfg.routes["net-out:2"] == "asio-out:3");
  const std::string sdp = t.engine->output_sdp(2);
  CHECK(sdp.find("s=TEST / Orban Warszawa") != std::string::npos);
  CHECK(sdp.find("c=IN IP4 " + cfg.outputs[1].address + "/") != std::string::npos);

  // an address already used by another output is moved to a free one
  req = {};
  req.name = "Omnia";
  req.source = "asio-out:4";
  req.address = cfg.outputs[0].address;
  r = t.engine->add_output(req);
  CHECK_FALSE(r.ok);
  req.address = "239.69.250.7";
  req.ptime_us = 4000;
  r = t.engine->add_output(req);
  REQUIRE(r.ok);
  CHECK(r.created == 3);
  cfg = t.engine->config();
  CHECK(cfg.outputs[2].address == "239.69.250.7");
  CHECK(cfg.outputs[2].ptime_us == 4000);

  // the default address of a slot taken by an enabled output is skipped
  t.set_output(5, "239.69.100.4");
  req = {};
  req.name = "Test";
  r = t.engine->add_output(req);  // no source yet: allowed, routed later in the matrix
  REQUIRE(r.ok);
  CHECK(r.created == 4);
  cfg = t.engine->config();
  CHECK(cfg.outputs[3].address != "239.69.100.4");
  CHECK(cfg.routes.count("net-out:4") == 0);

  req = {};
  req.name = "Zły";
  req.source = "nie:istnieje";
  CHECK_FALSE(t.engine->add_output(req).ok);
  req.source = "asio-out:1";
  req.name = std::string(60, 'x');
  CHECK_FALSE(t.engine->add_output(req).ok);

  // removing frees the slot: disabled, default name and address, its route gone
  r = t.engine->remove_output(2);
  REQUIRE(r.ok);
  cfg = t.engine->config();
  CHECK_FALSE(cfg.outputs[1].enabled);
  CHECK(cfg.outputs[1].name == "BounceAES 02");
  CHECK(cfg.outputs[1].address == "239.69.100.2");
  CHECK(cfg.routes.count("net-out:2") == 0);
  CHECK_FALSE(t.engine->remove_output(40).ok);
}

// Two BounceAES computers (here: two engines on loopback): A is the PTP master and sends ASIO
// pair 1, B follows A's clock and receives the stream. B's receive ring must carry A's samples at
// their own timeline positions, without gaps, for the whole run. BAM_TWO_ENGINES_SECONDS
// lengthens the run for investigations.
TEST_CASE("two engines: A is the PTP master and sends, B follows and receives without gaps") {
  const char* env = std::getenv("BAM_TWO_ENGINES_SECONDS");
  const int seconds = env ? std::atoi(env) : 8;
  auto make = [](const char* station, const char* role, bool output) {
    auto cfg = bam::default_config();
    cfg.interface_ip = "127.0.0.1";  // tests never send multicast onto a real network
    cfg.station_name = station;
    cfg.ptp_role = role == std::string("preferred") ? bam::PtpRole::Preferred : bam::PtpRole::Slave;
    cfg.outputs[0].enabled = output;
    cfg.outputs[0].address = "239.69.77.1";
    return bam::config_to_json(cfg);
  };
  TestEngine a(make("A", "preferred", true));
  TestEngine b(make("B", "slave", false));
  a.opts.enable_ptp = true;
  b.opts.enable_ptp = true;
  a.engine = std::make_unique<bam::Engine>(a.opts);
  b.engine = std::make_unique<bam::Engine>(b.opts);
  a.start();
  b.start();
  REQUIRE(b.subscribe_to_output_of(*a.engine, 1) == 1);

  std::string err;
  auto b_shm = bp::SharedMemory::open(b.opts.shm_name, bam::ShmLayout::total_bytes(), &err);
  REQUIRE_MESSAGE(b_shm, err);
  bam::ShmView bview(b_shm->data());

  // B steps its clock while it locks to A; checks start once it follows A
  const int64_t lock_deadline = bp::monotonic_ns() + 20'000'000'000;
  while (b.engine->status().ptp.state != bam::PtpState::Locked && bp::monotonic_ns() < lock_deadline) bp::sleep_ns(100'000'000);
  REQUIRE(b.engine->status().ptp.state == bam::PtpState::Locked);
  const uint64_t steps_at_lock = b.engine->status().ptp.steps;
  // packets received before B followed A's clock are rejected by design; count from the lock on
  const bam::RxStats rx_at_lock = b.engine->status().subscriptions[0].rx;

  FakeAsioHost host(a);  // mAirList on A plays the ramp on pair 1
  constexpr uint32_t kWindow = 4800;
  std::vector<int32_t> l(kWindow), r(kWindow);
  uint64_t checked = 0, bad = 0, bad_windows = 0, windows = 0;
  uint64_t first_bad_at = 0, bad_outside_late = 0;
  auto written_late = [&](uint64_t x) {
    for (const auto& [lo, hi] : host.late)
      if (x >= lo && x < hi) return true;
    return false;
  };
  int64_t next_check = bp::monotonic_ns() + 1'000'000'000;  // stream start
  const int64_t start = bp::monotonic_ns();
  host.run(int64_t(seconds + 1) * 1'000'000'000, [&](uint64_t) {
    const int64_t now = bp::monotonic_ns();
    if (now < next_check) return;
    next_check = now + 250'000'000;
    uint64_t gen = 0;
    const bam::MediaClock clk = bview.read_clock(&gen);
    const uint64_t b_now = clk.sample_at(now);
    const uint64_t from = b_now - kRxDelay - 96 - kWindow;  // fully played-out region on B
    bview.net_in(0).read(from, l.data(), kWindow);
    bview.net_in(1).read(from, r.data(), kWindow);
    uint64_t w = 0;
    for (uint32_t i = 0; i < kWindow; ++i) {
      const int e = (l[i] != ramp_value(from + i, 0)) + (r[i] != ramp_value(from + i, 1));
      w += e;
      if (e && !written_late(from + i)) bad_outside_late += e;
    }
    checked += kWindow;
    ++windows;
    if (w) {
      ++bad_windows;
      bad += w;
      if (!first_bad_at) first_bad_at = static_cast<uint64_t>((now - start) / 1'000'000);
    }
  });
  const auto st = b.engine->status();
  const auto& rx = st.subscriptions[0].rx;
  MESSAGE("windows " << windows << ", bad windows " << bad_windows << ", bad samples " << bad << " (outside client-late ranges: "
          << bad_outside_late << "), first bad at "
          << first_bad_at << " ms; B ptp offset " << st.ptp.offset_ns << " ns, steps " << st.ptp.steps << ", sync lost "
          << st.ptp.sync_lost << ", rate " << st.ptp.rate_ppm << " ppm; rx underruns " << rx.underruns << ", overruns "
          << rx.overruns << " (" << rx_at_lock.overruns << " before lock), seq " << rx.seq_errors << ", jitter " << rx.jitter_ms << " ms; A client skipped " << host.skipped);
  CHECK(windows > 0);
  CHECK(st.ptp.has_master);
  // Samples this non-realtime test client wrote after their period may be missing on the
  // network, as with any late ASIO host; everything else must arrive bit for bit.
  CHECK(bad_outside_late == 0);
  CHECK(rx.underruns == rx_at_lock.underruns);
  CHECK(rx.overruns == rx_at_lock.overruns);
  CHECK(st.ptp.steps == steps_at_lock);  // no clock step while streaming
}

TEST_CASE("engine mixes two ASIO pairs onto an AoIP output and onto an ASIO input") {
  TestEngine t;
  t.start();
  t.set_output(1, "239.69.206.1");
  REQUIRE(t.engine->set_mix("net-out:1", true, bam::MixConfig{{{"asio-out:1", 0, false}, {"asio-out:2", 0, false}}}, std::nullopt).ok);
  REQUIRE(t.subscribe_to_output(1) == 1);
  REQUIRE(t.engine->set_mix("asio-in:4", true, bam::MixConfig{{{"asio-out:1", 0, false}, {"asio-out:2", 0, false}}}, std::nullopt).ok);
  FakeAsioHost host(t);
  host.run(1'500'000'000);
  // the subscription carries ramp(pair 1) + ramp(pair 2), D_mix (96) behind
  const uint64_t from = host.last_start - 30000;
  std::vector<int32_t> l(10000);
  host.view->net_in(0).read(from, l.data(), l.size());
  int bad = 0;
  for (size_t i = 0; i < l.size(); ++i) bad += l[i] != l24(mix_sum(from + i - 96, 0, 2));
  CHECK(bad == 0);
  auto rec = host.asio_input(3, 0);
  bad = 0;
  for (uint32_t i = 0; i < 128; ++i) bad += rec[i] != mix_sum(host.last_start - 128 - 96 + i, 0, 2);
  CHECK(bad == 0);
  CHECK(t.engine->status().mixer_late == 0);
  auto v = t.engine->routing();
  CHECK(v.mixes.count("net-out:1"));
  CHECK(v.mix_delay_ms.at("net-out:1") == doctest::Approx(2.0));
}

TEST_CASE("engine: a one-source 0 dB mix is a direct route, mix mode toggles membership") {
  TestEngine t;
  t.start();
  REQUIRE(t.engine->set_route("asio-in:3", std::string("asio-out:1")).ok);
  REQUIRE(t.engine->set_mix("asio-in:3", true, std::nullopt, std::nullopt).ok);  // route becomes the first member
  auto cfg = t.engine->config();
  CHECK(cfg.routes.count("asio-in:3") == 0);
  REQUIRE(cfg.mixes.at("asio-in:3").sources.size() == 1);
  FakeAsioHost host(t);
  host.run(300'000'000);
  auto rec = host.asio_input(2, 0);
  int bad = 0;
  for (uint32_t i = 0; i < 128; ++i) bad += rec[i] != ramp_value(host.last_start - 128 + i, 0);  // no D_mix: direct
  CHECK(bad == 0);
  REQUIRE(t.engine->set_route("asio-in:3", std::string("asio-out:2")).ok);  // mix mode: adds
  CHECK(t.engine->config().mixes.at("asio-in:3").sources.size() == 2);
  REQUIRE(t.engine->set_route("asio-in:3", std::string("asio-out:1")).ok);  // removes
  CHECK(t.engine->config().mixes.at("asio-in:3").sources.size() == 1);
  CHECK_FALSE(t.engine->set_mix("asio-in:3", false, std::nullopt, std::string("asio-out:9")).ok);  // keep must be a member
  REQUIRE(t.engine->set_mix("asio-in:3", false, std::nullopt, std::string("asio-out:2")).ok);
  cfg = t.engine->config();
  CHECK(cfg.mixes.count("asio-in:3") == 0);
  CHECK(cfg.routes.at("asio-in:3") == "asio-out:2");
}

TEST_CASE("engine sends a mix to a Windows device as a mix bus") {
  TestEngine t;
  t.start();
  const std::string dst = "win-out:{0.0.0.00000000}.{spk}#2";
  REQUIRE(t.engine->set_mix(dst, true, bam::MixConfig{{{"asio-out:1", 0, false}, {"asio-out:2", -6, false}}}, std::nullopt).ok);
  FakeAsioHost host(t);
  auto req = bam::request_from_json(host.view->read_request(nullptr));
  REQUIRE(req);
  REQUIRE(req->dests.size() == 1);
  CHECK(req->dests[0].id == dst);
  CHECK(req->dests[0].kind == bam::ShmSource::Mix);
  CHECK(req->dests[0].delay_frames == 96);
  CHECK(req->dests[0].channel_pair == 2);
  const int bus = req->dests[0].pair;
  // muting a member keeps the destination on its bus at the same D_mix (no reroute, no gap)
  REQUIRE(t.engine->set_mix(dst, true, bam::MixConfig{{{"asio-out:1", 0, false}, {"asio-out:2", -6, true}}}, std::nullopt).ok);
  req = bam::request_from_json(host.view->read_request(nullptr));
  REQUIRE(req);
  REQUIRE(req->dests.size() == 1);
  CHECK(req->dests[0].kind == bam::ShmSource::Mix);
  CHECK(req->dests[0].pair == bus);
  CHECK(req->dests[0].delay_frames == 96);
  // another mix and a gain change elsewhere keep this destination on its bus (no interruption)
  REQUIRE(t.engine->set_mix("asio-in:2", true, bam::MixConfig{{{"asio-out:3", 0, false}, {"asio-out:4", 0, false}}}, std::nullopt).ok);
  REQUIRE(t.engine->set_mix(dst, true, bam::MixConfig{{{"asio-out:1", -3, false}, {"asio-out:2", -6, false}}}, std::nullopt).ok);
  req = bam::request_from_json(host.view->read_request(nullptr));
  REQUIRE(req);
  REQUIRE(req->dests.size() == 1);
  CHECK(req->dests[0].pair == bus);
}

TEST_CASE("engine routing view: a multichannel Windows render device gets one destination per channel pair") {
  TestEngine t;
  t.start();
  FakeAsioHost host(t);
  auto& h = host.view->header();
  h.bridge_pid = bp::current_pid();
  const std::string usb = "{0.0.0.00000000}.{usb}";
  bam::BridgeReport report;
  report.generation = 1;
  report.endpoints.push_back({usb, "Speakers (USB Audio)", true, false, 6});
  host.view->write_report(bam::to_json(report));
  host.run(300'000'000, [&](uint64_t) { h.bridge_heartbeat.fetch_add(1); });

  auto v = t.engine->routing();
  std::map<std::string, bam::RouteNode> dests;
  for (const auto& n : v.dests) dests[n.id] = n;
  REQUIRE(dests.count("win-out:" + usb));
  REQUIRE(dests.count("win-out:" + usb + "#2"));
  REQUIRE(dests.count("win-out:" + usb + "#3"));
  CHECK(dests["win-out:" + usb].name == "Speakers (USB Audio) 1-2");
  CHECK(dests["win-out:" + usb + "#2"].name == "Speakers (USB Audio) 3-4");
  CHECK(dests["win-out:" + usb + "#3"].name == "Speakers (USB Audio) 5-6");

  t.route("win-out:" + usb + "#3", "asio-out:1");
  auto req = bam::request_from_json(host.view->read_request(nullptr));
  REQUIRE(req);
  REQUIRE(req->dests.size() == 1);
  CHECK(req->dests[0].id == "win-out:" + usb + "#3");
  CHECK(req->dests[0].channel_pair == 3);
  h.bridge_pid = 0;
}

TEST_CASE("engine receives more subscriptions on one port than a socket may join") {
  TestEngine t;
  t.start();
  constexpr int kStreams = 24;  // Linux allows 20 memberships per socket
  auto cfg = t.engine->config();
  for (int i = 0; i < kStreams; ++i) {
    cfg.outputs[i].enabled = true;
    cfg.outputs[i].address = "239.69.202." + std::to_string(i + 1);
  }
  REQUIRE(t.engine->apply_config(cfg).ok);
  for (int i = 0; i < kStreams; ++i) REQUIRE(t.subscribe_to_output(i + 1) == i + 1);
  // Windows activates multicast memberships with some delay per group, so wait for every stream
  // instead of assuming a fixed start-up time.
  bam::EngineStatus st;
  const int64_t deadline = bp::monotonic_ns() + 5'000'000'000;
  for (;;) {
    st = t.engine->status();
    bool all = true;
    for (int i = 0; i < kStreams; ++i) all = all && st.subscriptions[i].rx.packets > 300;
    if (all || bp::monotonic_ns() > deadline) break;
    bp::sleep_ns(100'000'000);
  }
  for (int i = 0; i < kStreams; ++i) {
    INFO("subscription " << i + 1 << " error: " << st.subscriptions[i].error);
    CHECK(st.subscriptions[i].error.empty());
    CHECK(st.subscriptions[i].rx.packets > 300);
    CHECK(st.subscriptions[i].rx.seq_errors == 0);
  }
}

TEST_CASE("engine routing view, invalid routes and subscription removal") {
  TestEngine t;
  t.start();
  REQUIRE(t.subscribe_to_output(1) == 1);
  t.route("asio-in:1", "net-in:1");
  t.route("net-out:1", "net-in:1");

  auto v = t.engine->routing();
  auto has = [](const std::vector<bam::RouteNode>& list, const std::string& id) {
    for (const auto& n : list)
      if (n.id == id) return true;
    return false;
  };
  CHECK(has(v.sources, "net-in:1"));
  CHECK(has(v.sources, "asio-out:32"));
  CHECK(has(v.dests, "net-out:1"));
  CHECK_FALSE(has(v.dests, "net-out:2"));  // disabled output
  CHECK(has(v.dests, "asio-in:1"));
  CHECK(v.routes.at("asio-in:1") == "net-in:1");

  CHECK_FALSE(t.engine->set_route("bogus:1", std::string("net-in:1")).ok);
  CHECK_FALSE(t.engine->set_route("asio-in:1", std::string("net-in:99")).ok);
  CHECK_FALSE(t.engine->add_subscription("v=0\r\nnonsense").ok);
  CHECK_FALSE(t.engine->add_subscription("239.1.2.3/abcd").ok);  // unknown SAP id

  // Windows routes without a bridge: listed, unavailable
  t.route("net-out:1", "win-app:Chrome.exe");
  v = t.engine->routing();
  CHECK(v.routes.at("net-out:1") == "win-app:chrome.exe");
  bool found = false;
  for (const auto& n : v.sources) {
    if (n.id != "win-app:chrome.exe") continue;
    found = true;
    CHECK_FALSE(n.available);
  }
  CHECK(found);

  REQUIRE(t.engine->remove_subscription(1).ok);
  const auto cfg = t.engine->config();
  CHECK_FALSE(cfg.subscriptions[0].enabled);
  CHECK(cfg.routes.count("asio-in:1") == 0);
  CHECK(cfg.routes.at("net-out:1") == "win-app:chrome.exe");
}

TEST_CASE("engine rejects invalid configuration and keeps the running one") {
  TestEngine t;
  t.start();
  auto cfg = t.engine->config();
  cfg.outputs[1].ptime_us = 999;
  auto r = t.engine->apply_config(cfg);
  CHECK_FALSE(r.ok);
  REQUIRE_FALSE(r.issues.empty());
  CHECK(r.issues[0].device == 2);
  CHECK(t.engine->config().outputs[1].ptime_us == 1000);
}

TEST_CASE("engine persists configuration and keeps the password hash") {
  TestEngine t;
  t.start();
  REQUIRE(t.engine->set_password_hash("pbkdf2-sha256$1$00$00"));
  auto cfg = t.engine->config();
  cfg.outputs[2].name = "Kraków";
  cfg.password_hash = "";
  REQUIRE(t.engine->apply_config(cfg).ok);
  t.engine->stop();

  bam::Engine again(t.opts);
  std::string err;
  REQUIRE_MESSAGE(again.start(&err), err);
  CHECK(again.config().outputs[2].name == "Kraków");
  CHECK(again.config().password_hash == "pbkdf2-sha256$1$00$00");
  again.stop();
}

TEST_CASE("engine migrates a 1.0 configuration file on start and keeps a backup") {
  const std::string v1 =
      R"({"interface_ip":"127.0.0.1","ptp_domain":0,"pc_nominal_level_dbfs":-20,"http_bind":"127.0.0.1","http_port":8067,)"
      R"("password_hash":"pbkdf2-sha256$1$00$00","devices":[)"
      R"({"enabled":true,"name":"Warszawa","tx_address":"239.69.204.1","tx_ptime_us":1000,"keep_active":true,"rx_sdp":"","rx_latency_us":5000}]})";
  TestEngine t(v1);
  t.start();
  const auto cfg = t.engine->config();
  CHECK(cfg.outputs[0].enabled);
  CHECK(cfg.outputs[0].name == "Warszawa");
  CHECK(cfg.asio_names[0] == "Warszawa");
  CHECK(cfg.routes.at("net-out:1") == "asio-out:1");
  CHECK(cfg.password_hash == "pbkdf2-sha256$1$00$00");
  CHECK(read_file(t.config_path + ".v1.bak") == v1);
  CHECK(read_file(t.config_path).find("\"version\": 2") != std::string::npos);
}
