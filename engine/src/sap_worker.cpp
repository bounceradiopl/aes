// SPDX-License-Identifier: GPL-3.0-or-later
#include <map>

#include "bam/ipv4.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam {

namespace {
constexpr int64_t kAnnounceNs = 30LL * 1'000'000'000;
constexpr int64_t kExpireCheckNs = 10LL * 1'000'000'000;
}  // namespace

void Engine::Impl::sap_loop() {
  std::vector<uint8_t> buf(4096);
  std::vector<uint8_t> ready;
  std::map<int, std::string> announced;  // device -> SDP last announced
  std::optional<platform::UdpSocket> sock;
  std::string sock_iface;
  uint64_t seen_dirty = 0;
  int64_t last_announce = 0, last_expire = 0;

  auto origin_bytes = [&]() {
    auto ip = parse_ipv4(current_interface());
    return ip ? *ip : Ipv4{0, 0, 0, 0};
  };
  auto send = [&](bool deletion, const std::string& sdp) {
    if (sock) sock->send_to(build_sap(deletion, sap_hash(sdp), origin_bytes(), sdp), kSapGroup, kSapPort);
  };

  while (running) {
    if (!sock || sock_iface != current_interface()) {
      sock.reset();
      sock_iface = current_interface();
      std::string err;
      sock = platform::UdpSocket::open(kSapPort, &err);
      if (sock && !sock->join(kSapGroup, sock_iface, "", &err)) sock.reset();
      if (!sock) {
        log_warning("SAP niedostępny: " + err);
        for (int i = 0; i < 50 && running; ++i) platform::sleep_ns(100'000'000);
        continue;
      }
      sock->set_multicast_out(sock_iface, kMulticastTtl, true);
      seen_dirty = 0;  // re-announce on the new interface
    }

    std::vector<platform::UdpSocket*> socks = {&*sock};
    platform::wait_readable(socks, 100, ready);
    const int64_t now = platform::monotonic_ns();
    if (ready[0]) {
      int n;
      std::lock_guard lock(sap_mutex);
      while ((n = sock->recv(buf, nullptr)) >= 0)
        directory.on_packet(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), now);
    }
    if (now - last_expire > kExpireCheckNs) {
      std::lock_guard lock(sap_mutex);
      directory.expire(now);
      last_expire = now;
    }

    const uint64_t dirty = sap_dirty.load();
    if (dirty != seen_dirty || now - last_announce > kAnnounceNs) {
      seen_dirty = dirty;
      last_announce = now;
      Config cfg;
      {
        std::lock_guard lock(config_mutex);
        cfg = config;
      }
      for (int dev = 1; dev <= kDeviceCount; ++dev) {
        auto it = announced.find(dev);
        if (cfg.devices[dev - 1].enabled) {
          const std::string sdp = build_output_sdp(cfg, dev);
          if (it != announced.end() && it->second != sdp) send(true, it->second);
          send(false, sdp);
          announced[dev] = sdp;
        } else if (it != announced.end()) {
          send(true, it->second);
          announced.erase(it);
        }
      }
    }
  }
  for (const auto& [dev, sdp] : announced) send(true, sdp);
}

}  // namespace bam
