// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdio>
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
  int subscribe_to_output(int n) {
    auto r = engine->add_subscription(engine->output_sdp(n));
    REQUIRE(r.ok);
    return r.created;
  }
  void route(const std::string& dst, const std::string& src) { REQUIRE(engine->set_route(dst, src).ok); }
};

// A fake ASIO host: attaches to the shared timeline and plays ramp_value() on ASIO output pair 1.
struct FakeAsioHost {
  std::optional<bp::SharedMemory> shm;
  std::optional<bp::NamedEvent> ev;
  std::unique_ptr<bam::ShmView> view;
  uint64_t last_start = 0;
  int periods = 0;
  int skipped = 0;

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
    std::vector<std::vector<int32_t>> out(2, std::vector<int32_t>(128));
    std::vector<const int32_t*> out_ptrs(bam::kChannelCount, nullptr);
    out_ptrs[0] = out[0].data();
    out_ptrs[1] = out[1].data();
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
        if (s - last_start > 128) ++skipped;
        first = last_start + 128;
      }
      for (uint64_t p = first; p <= s; p += 128) {
        for (int ch = 0; ch < 2; ++ch)
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
