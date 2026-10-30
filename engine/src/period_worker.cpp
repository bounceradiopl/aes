// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam {

namespace {
constexpr int64_t kIdleNs = 5'000'000;
constexpr int64_t kMaxSleepNs = 10'000'000;      // heartbeat at least every 10 ms
constexpr int64_t kLivenessCheckNs = 500'000'000;
constexpr int64_t kClientStaleNs = 2'000'000'000;
}  // namespace

// Generates the ASIO period clock from the PTP media clock: one event per buffer of B samples,
// at the exact local time the media clock crosses k * B.
void Engine::Impl::period_loop() {
  platform::set_realtime_priority("Pro Audio");
  auto& h = view->header();
  uint64_t last_published = 0;
  uint64_t generation = clock.step_generation();
  uint64_t seen_client_hb = 0;
  int64_t client_hb_changed = platform::monotonic_ns();
  int64_t last_liveness = 0;
  uint32_t last_pid = 0;

  MediaClock published;
  uint64_t published_generation = ~0ull;

  while (running) {
    h.heartbeat.fetch_add(1, std::memory_order_release);
    const int64_t now = platform::monotonic_ns();
    {
      const MediaClock c = clock.get();
      const uint64_t g = clock.step_generation();
      if (g != published_generation || c.base_local() != published.base_local() || c.base_ptp() != published.base_ptp() ||
          c.rate() != published.rate()) {
        view->publish_clock(c, g);
        published = c;
        published_generation = g;
      }
    }
    const uint32_t pid = h.client_pid.load(std::memory_order_acquire);

    if (pid != last_pid) {
      if (pid) log_info("Aplikacja ASIO podłączona (PID " + std::to_string(pid) + ")");
      if (!pid && last_pid) log_info("Aplikacja ASIO odłączona");
      last_pid = pid;
      client_hb_changed = now;
    }
    if (pid && now - last_liveness > kLivenessCheckNs) {
      last_liveness = now;
      const uint64_t hb = h.client_heartbeat.load(std::memory_order_relaxed);
      if (hb != seen_client_hb) {
        seen_client_hb = hb;
        client_hb_changed = now;
      }
      const bool stale = h.client_running.load() && now - client_hb_changed > kClientStaleNs;
      if (!platform::process_alive(pid) || stale) {
        log_warning("Aplikacja ASIO (PID " + std::to_string(pid) + ") nie odpowiada, zwalniam urządzenie");
        h.client_running.store(0);
        uint32_t expected = pid;
        h.client_pid.compare_exchange_strong(expected, 0);
        continue;
      }
    }

    const uint32_t frames = h.buffer_frames.load(std::memory_order_acquire);
    if (!pid || !h.client_running.load(std::memory_order_acquire) || frames < 16 || frames > 4096) {
      last_published = 0;
      platform::sleep_ns(kIdleNs);
      continue;
    }

    const uint64_t g = clock.step_generation();
    if (g != generation) {
      generation = g;
      last_published = 0;
    }
    const MediaClock clk = clock.get();
    const uint64_t now_s = clk.sample_at(now);
    uint64_t next = (now_s / frames + 1) * frames;
    if (last_published && next <= last_published) next = last_published + frames;
    const int64_t target = clk.local_for_sample(next);
    platform::sleep_until_ns(std::min(target, now + kMaxSleepNs));
    if (platform::monotonic_ns() + 20'000 >= target) {
      h.period_start_sample.store(next, std::memory_order_release);
      h.period_counter.fetch_add(1, std::memory_order_acq_rel);
      period_event->signal();
      last_published = next;
    }
  }
}

void Engine::Impl::monitor_loop() {
  while (running) {
    for (int i = 0; i < 20 && running; ++i) platform::sleep_ns(100'000'000);
    if (!running) break;
    bridge_alive();  // keeps the bridge heartbeat observation fresh
    std::string configured;
    {
      std::lock_guard lock(config_mutex);
      configured = config.interface_ip;
    }
    if (!configured.empty()) continue;
    const std::string detected = platform::default_interface_ip();
    if (detected.empty() || detected == current_interface()) continue;
    log_warning("Zmiana adresu karty sieciowej: " + (current_interface().empty() ? "(brak)" : current_interface()) + " → " +
                detected + ", ponowne dołączanie do grup multicast");
    {
      std::lock_guard lock(iface_mutex);
      interface_ip = detected;
    }
    {
      std::lock_guard lock(tx_socket_mutex);
      if (tx_socket) tx_socket->set_multicast_out(detected, kMulticastTtl, true);
    }
    Config cfg;
    {
      std::lock_guard lock(config_mutex);
      cfg = config;
    }
    rebuild_runtime(cfg, true);
    ptp_running = false;
  }
}

}  // namespace bam
