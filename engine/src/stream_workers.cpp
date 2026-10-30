// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "bam/meters.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam {

namespace {
constexpr int64_t kTickNs = 5'000'000;
constexpr int64_t kMeterNs = 50'000'000;
constexpr uint32_t kMeterFrames = 2400;  // 50 ms window
constexpr int64_t kTxPeriodNs = 250'000;
constexpr int64_t kClientGraceNs = 1'000'000'000;
}  // namespace

void Engine::Impl::rx_loop() {
  platform::set_realtime_priority("Pro Audio");
  std::vector<uint8_t> buf(2048);
  std::vector<int32_t> scratch(kMeterFrames);
  std::vector<uint8_t> ready;
  int64_t last_tick = 0, last_meter = 0;

  while (running) {
    const auto snap = snapshot();
    std::vector<platform::UdpSocket*> socks;
    std::vector<RxEntry*> entries;
    for (const auto& e : snap->rx) {
      if (!e->stream) continue;
      socks.push_back(&e->socket);
      entries.push_back(e.get());
    }
    platform::wait_readable(socks, 2, ready);

    const MediaClock clk = clock.get();
    for (size_t i = 0; i < socks.size(); ++i) {
      if (!ready[i]) continue;
      int n;
      while ((n = socks[i]->recv(buf, nullptr)) >= 0) {
        const int64_t now = platform::monotonic_ns();
        entries[i]->stream->on_packet(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), clk.sample_at(now), now);
      }
    }

    const int64_t now = platform::monotonic_ns();
    const uint64_t now_s = clk.sample_at(now);
    if (now - last_tick >= kTickNs) {
      for (auto* e : entries) e->stream->on_tick(now_s);
      last_tick = now;
    }
    if (now - last_meter >= kMeterNs && view) {
      last_meter = now;
      auto& h = view->header();
      for (int ch = 0; ch < kChannelCount; ++ch) {
        const uint64_t lat = h.rx_latency_frames[ch / 2].load(std::memory_order_relaxed);
        if (now_s > lat + kMeterFrames) {
          view->input(ch).read(now_s - lat - kMeterFrames, scratch.data(), kMeterFrames);
          in_peak[ch].store(peak_dbfs(scratch.data(), kMeterFrames), std::memory_order_relaxed);
          view->output(ch).read(now_s - kMeterFrames, scratch.data(), kMeterFrames);
          out_peak[ch].store(peak_dbfs(scratch.data(), kMeterFrames), std::memory_order_relaxed);
        }
      }
    }
  }
}

void Engine::Impl::tx_loop() {
  platform::set_realtime_priority("Pro Audio");
  uint64_t generation = clock.step_generation();
  int64_t next_wake = platform::monotonic_ns();

  while (running) {
    next_wake += kTxPeriodNs;
    const int64_t before = platform::monotonic_ns();
    if (before - next_wake > 5'000'000) next_wake = before;  // fell far behind: don't burst
    platform::sleep_until_ns(next_wake);

    const auto snap = snapshot();
    const uint64_t g = clock.step_generation();
    if (g != generation) {
      for (const auto& e : snap->tx) e->stream->reset();
      generation = g;
    }

    const int64_t now = platform::monotonic_ns();
    auto& h = view->header();
    if (h.client_pid.load(std::memory_order_relaxed) != 0 && h.client_running.load(std::memory_order_relaxed) != 0) {
      last_client_active_ns.store(now, std::memory_order_relaxed);
    }
    const bool client_recent = now - last_client_active_ns.load(std::memory_order_relaxed) < kClientGraceNs;
    const uint64_t now_s = clock.get().sample_at(now);

    std::lock_guard lock(tx_socket_mutex);
    if (!tx_socket) continue;
    for (const auto& e : snap->tx) {
      if (e->keep_active || client_recent) {
        TxEntry* entry = e.get();
        entry->stream->pump(now_s, [&](std::span<const uint8_t> p) {
          if (!tx_socket->send_to(p, entry->address, kRtpPort)) entry->stream->count_send_error();
        });
      } else {
        e->stream->reset();
      }
    }
  }
}

}  // namespace bam
