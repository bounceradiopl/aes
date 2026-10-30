// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam {

namespace {
const char* state_name(PtpState s) {
  switch (s) {
    case PtpState::Locked:
      return "zsynchronizowany";
    case PtpState::Holdover:
      return "holdover (brak mastera)";
    case PtpState::Listening:
      return "nasłuch";
  }
  return "?";
}
}  // namespace

void Engine::Impl::ptp_loop() {
  std::vector<uint8_t> buf(1500);
  bool warned = false;
  while (running) {
    ptp_running = true;
    const std::string iface = current_interface();
    int domain;
    {
      std::lock_guard lock(config_mutex);
      domain = config.ptp_domain;
    }

    std::string err;
    auto ev = platform::UdpSocket::open(kPtpEventPort, &err);
    auto gen = ev ? platform::UdpSocket::open(kPtpGeneralPort, &err) : std::nullopt;
    if (ev && gen) {
      if (!ev->join(kPtpGroup, iface, "", &err) || !gen->join(kPtpGroup, iface, "", &err)) {
        ev.reset();
      } else {
        ev->set_multicast_out(iface, 1, true);  // loopback: a master on the same host must see our Delay_Req
      }
    }
    if (!ev || !gen) {
      if (!warned) log_error("PTP niedostępny: " + err + ". Audio działa na zegarze lokalnym.");
      warned = true;
      for (int i = 0; i < 50 && running && ptp_running; ++i) platform::sleep_ns(100'000'000);
      continue;
    }
    warned = false;
    log_info("PTP: nasłuch w domenie " + std::to_string(domain) + " na " + (iface.empty() ? "domyślnej karcie" : iface));

    PtpSlave slave(static_cast<uint8_t>(domain), self_identity, clock.get());
    PtpState last_state = PtpState::Listening;
    bool last_master = false;
    std::vector<platform::UdpSocket*> socks = {&*ev, &*gen};
    std::vector<uint8_t> ready;

    while (running && ptp_running) {
      platform::wait_readable(socks, 10, ready);
      for (size_t i = 0; i < socks.size(); ++i) {
        if (!ready[i]) continue;
        int n;
        while ((n = socks[i]->recv(buf, nullptr)) >= 0) {
          const int64_t t = platform::monotonic_ns();  // software receive timestamp
          slave.on_message(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), t);
        }
      }
      const int64_t now = platform::monotonic_ns();
      if (auto req = slave.poll_delay_req(now)) {
        const int64_t t3 = platform::monotonic_ns();
        ev->send_to(*req, kPtpGroup, kPtpEventPort);
        slave.on_delay_req_sent(t3);
      }
      slave.tick(now);

      const PtpStatus st = slave.status();
      const bool stepped = slave.consume_step();
      if (st.steps > 0) clock.set(slave.clock(), stepped);
      if (stepped) log_info("PTP: korekta skokowa zegara (" + std::to_string(st.steps) + ")");
      if (st.state == PtpState::Locked) ptp_ever_locked = true;
      {
        std::lock_guard lock(ptp_mutex);
        ptp_status = st;
      }
      ShmPtpState shm_state = ShmPtpState::FreeRun;
      if (st.state == PtpState::Locked) {
        shm_state = ShmPtpState::Locked;
      } else if (st.state == PtpState::Holdover) {
        shm_state = ShmPtpState::Holdover;
      } else if (st.steps > 0) {
        shm_state = ShmPtpState::Listening;
      }
      view->header().ptp_state.store(static_cast<uint32_t>(shm_state));

      if (st.has_master != last_master) {
        if (st.has_master) {
          log_info("PTP: wybrano mastera " + st.master.clock.to_string() + ", grandmaster " + st.grandmaster.to_string());
        } else {
          log_warning("PTP: brak mastera w sieci");
        }
        last_master = st.has_master;
        sap_dirty.fetch_add(1);  // SDP carries the grandmaster identity
      }
      if (st.state != last_state) {
        (st.state == PtpState::Holdover ? log_warning : log_info)(std::string("PTP: ") + state_name(st.state));
        last_state = st.state;
      }
    }
  }
}

}  // namespace bam
