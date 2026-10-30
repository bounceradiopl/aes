// SPDX-License-Identifier: GPL-3.0-or-later
// bounceaes-test: end-to-end loopback tester for a running BounceAESService.
//
//   bounceaes-test --configure-loopback --devices 32 --password HASLO [--port 8067] [--interface 127.0.0.1]
//       enables devices 1..N and sets each input to the device's own output stream (via the HTTP API)
//   bounceaes-test --loopback --seconds 60 [--devices 32] [--buffer 128]
//       attaches to the shared timeline like the ASIO driver, plays a sample counter on every output
//       and verifies, sample by sample, that it comes back on the matching input through the network
#include <httplib.h>

#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "bam/asio_bridge.hpp"
#include "bam/constants.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"

namespace bp = bam::platform;
using json = nlohmann::json;

namespace {

struct Options {
  bool configure = false;
  bool loopback = false;
  int devices = 32;
  int seconds = 30;
  uint32_t buffer = 128;
  std::string password;
  std::string interface_ip;
  uint16_t port = bam::kDefaultHttpPort;
  std::string shm = bam::kShmName;
  std::string event = bam::kPeriodEventName;
};

int32_t pattern(uint64_t n, int ch) { return static_cast<int32_t>(((n * 31 + static_cast<uint64_t>(ch) * 7919) & 0xFFFFFF) << 8); }

int configure(const Options& o) {
  httplib::Client c("127.0.0.1", o.port);
  c.set_read_timeout(10, 0);
  auto login = c.Post("/api/login", json{{"password", o.password}}.dump(), "application/json");
  if (!login || login->status != 200) {
    std::fprintf(stderr, "Logowanie nieudane (sprawdź --password i czy usługa działa)\n");
    return 1;
  }
  const std::string cookie = login->get_header_value("Set-Cookie").substr(0, login->get_header_value("Set-Cookie").find(';'));
  httplib::Headers h = {{"Cookie", cookie}};

  json devices = json::array();
  for (int d = 1; d <= bam::kDeviceCount; ++d) devices.push_back({{"enabled", d <= o.devices}});
  json patch = {{"devices", devices}};
  if (!o.interface_ip.empty()) patch["interface_ip"] = o.interface_ip;
  auto r = c.Put("/api/config", h, patch.dump(), "application/json");
  if (!r || r->status != 200) {
    std::fprintf(stderr, "Nie można włączyć urządzeń: %s\n", r ? r->body.c_str() : "brak odpowiedzi");
    return 1;
  }
  devices = json::array();
  for (int d = 1; d <= o.devices; ++d) {
    auto sdp = c.Get("/api/devices/" + std::to_string(d) + "/sdp", h);
    if (!sdp || sdp->status != 200) return 1;
    devices.push_back({{"rx_sdp", sdp->body}});
  }
  r = c.Put("/api/config", h, json{{"devices", devices}}.dump(), "application/json");
  if (!r || r->status != 200) {
    std::fprintf(stderr, "Nie można ustawić wejść: %s\n", r ? r->body.c_str() : "brak odpowiedzi");
    return 1;
  }
  std::printf("Skonfigurowano pętlę na %d urządzeniach (wejście N = wyjście N)\n", o.devices);
  return 0;
}

int loopback(const Options& o) {
  std::string err;
  auto shm = bp::SharedMemory::open(o.shm, bam::ShmLayout::total_bytes(), &err);
  if (!shm) {
    std::fprintf(stderr, "Usługa nie działa: %s\n", err.c_str());
    return 1;
  }
  auto ev = bp::NamedEvent::open(o.event, &err);
  if (!ev) {
    std::fprintf(stderr, "Brak zdarzenia okresu: %s\n", err.c_str());
    return 1;
  }
  bam::ShmView view(shm->data());
  if (!view.compatible()) {
    std::fprintf(stderr, "Niezgodna wersja pamięci współdzielonej\n");
    return 1;
  }
  auto& h = view.header();
  uint32_t expected = 0;
  if (!h.client_pid.compare_exchange_strong(expected, bp::current_pid())) {
    std::fprintf(stderr, "Urządzenie zajęte przez PID %u\n", expected);
    return 1;
  }
  bp::set_realtime_priority("Pro Audio");
  h.buffer_frames = o.buffer;
  h.client_running = 1;

  const int channels = o.devices * 2;
  const uint32_t b = o.buffer;
  std::vector<std::vector<int32_t>> out(bam::kChannelCount, std::vector<int32_t>(b));
  std::vector<std::vector<int32_t>> in(bam::kChannelCount, std::vector<int32_t>(b));
  std::vector<const int32_t*> out_ptrs(bam::kChannelCount, nullptr);
  std::vector<int32_t*> in_ptrs(bam::kChannelCount, nullptr);
  for (int ch = 0; ch < channels; ++ch) {
    out_ptrs[ch] = out[ch].data();
    in_ptrs[ch] = in[ch].data();
  }
  bam::AsioBridge bridge(view);
  std::vector<uint64_t> errors(channels, 0), checked(channels, 0);
  uint64_t periods = 0, missed = 0, last_start = 0, last_counter = h.period_counter.load();
  const int64_t start = bp::monotonic_ns();
  const int64_t warmup_end = start + 1'000'000'000;
  const int64_t end = start + int64_t(o.seconds) * 1'000'000'000 + 1'000'000'000;
  int64_t next_report = start + 5'000'000'000;

  while (bp::monotonic_ns() < end) {
    if (!ev->wait(50)) continue;
    const uint64_t c = h.period_counter.load();
    if (c == last_counter) continue;
    last_counter = c;
    const uint64_t s = h.period_start_sample.load();
    uint64_t first = s;
    if (last_start) {
      if (s - last_start > b) ++missed;
      first = last_start + b;
    }
    for (uint64_t p = first; p <= s; p += b) {
      for (int ch = 0; ch < channels; ++ch)
        for (uint32_t i = 0; i < b; ++i) out[ch][i] = pattern(p + b + i, ch);
      bridge.write_outputs(p, b, out_ptrs.data());
    }
    last_start = s;
    bridge.read_inputs(s, b, in_ptrs.data());
    h.client_heartbeat.fetch_add(1);
    ++periods;

    if (bp::monotonic_ns() > warmup_end) {
      for (int ch = 0; ch < channels; ++ch) {
        const uint64_t lat = h.rx_latency_frames[ch / 2].load();
        const uint64_t from = s - b - lat;
        for (uint32_t i = 0; i < b; ++i) {
          ++checked[ch];
          if (in[ch][i] != pattern(from + i, ch)) ++errors[ch];
        }
      }
    }
    const int64_t now = bp::monotonic_ns();
    if (now > next_report) {
      uint64_t total = 0;
      for (auto e : errors) total += e;
      std::printf("  %3lld s: okresy %llu, pominięte %llu, błędne próbki %llu\n", static_cast<long long>((now - start) / 1'000'000'000),
                  static_cast<unsigned long long>(periods), static_cast<unsigned long long>(missed), static_cast<unsigned long long>(total));
      std::fflush(stdout);
      next_report += 5'000'000'000;
    }
  }
  h.client_running = 0;
  uint32_t me = bp::current_pid();
  h.client_pid.compare_exchange_strong(me, 0);

  bool ok = true;
  for (int ch = 0; ch < channels; ++ch) {
    const bool good = errors[ch] == 0 && checked[ch] > 0;
    ok &= good;
    if (!good)
      std::printf("Kanał %2d (%s): %llu błędnych próbek z %llu\n", ch, bam::channel_label(ch, "").c_str(),
                  static_cast<unsigned long long>(errors[ch]), static_cast<unsigned long long>(checked[ch]));
  }
  std::printf("%s: %d kanałów, %llu okresów po %u próbek, %llu sprawdzonych próbek na kanał, pominięte okresy klienta: %llu\n",
              ok ? "OK" : "BŁĄD", channels, static_cast<unsigned long long>(periods), b,
              static_cast<unsigned long long>(checked.empty() ? 0 : checked[0]), static_cast<unsigned long long>(missed));
  return ok ? 0 : 1;
}

void usage() {
  std::printf(
      "bounceaes-test --configure-loopback --password HASLO [--devices 32] [--port 8067] [--interface 127.0.0.1]\n"
      "bounceaes-test --loopback [--seconds 30] [--devices 32] [--buffer 128]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--configure-loopback") o.configure = true;
    else if (a == "--loopback") o.loopback = true;
    else if (a == "--devices") o.devices = std::atoi(next().c_str());
    else if (a == "--seconds") o.seconds = std::atoi(next().c_str());
    else if (a == "--buffer") o.buffer = static_cast<uint32_t>(std::atoi(next().c_str()));
    else if (a == "--password") o.password = next();
    else if (a == "--port") o.port = static_cast<uint16_t>(std::atoi(next().c_str()));
    else if (a == "--interface") o.interface_ip = next();
    else if (a == "--shm") o.shm = next();
    else if (a == "--event") o.event = next();
    else {
      usage();
      return 2;
    }
  }
  if (o.devices < 1 || o.devices > bam::kDeviceCount || o.buffer < 16 || o.buffer > 4096) {
    usage();
    return 2;
  }
  if (o.configure) return configure(o);
  if (o.loopback) return loopback(o);
  usage();
  return 2;
}
