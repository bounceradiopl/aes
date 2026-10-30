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
    case PtpState::Master:
      return "master (ten komputer nadaje zegar)";
  }
  return "?";
}
}  // namespace

static PtpLocal ptp_local_from(const Config& cfg) {
  PtpLocal l;
  l.role = cfg.ptp_role;
  l.announce.priority1 = static_cast<uint8_t>(cfg.ptp_priority1);
  l.announce.priority2 = static_cast<uint8_t>(cfg.ptp_priority2);
  l.announce.clock_class = 248;  // default, not traceable
  l.announce.clock_accuracy = 0xFE;
  l.announce.variance = 0xFFFF;
  l.announce.time_source = 0xA0;  // internal oscillator
  return l;
}

void Engine::Impl::ptp_loop() {
  std::vector<uint8_t> buf(1500);
  bool warned = false;
  while (running) {
    ptp_running = true;
    const std::string iface = current_interface();
    int domain;
    PtpLocal local;
    uint64_t local_version = ptp_local_version.load();
    {
      std::lock_guard lock(config_mutex);
      domain = config.ptp_domain;
      local = ptp_local_from(config);
    }

    std::string err;
    auto ev = platform::UdpSocket::open(kPtpEventPort, &err);
    auto gen = ev ? platform::UdpSocket::open(kPtpGeneralPort, &err) : std::nullopt;
    if (ev && gen) {
      if (!ev->join(kPtpGroup, iface, "", &err) || !gen->join(kPtpGroup, iface, "", &err)) {
        ev.reset();
      } else {
        // loopback: a master on the same host must see our Delay_Req (and our own messages are ignored)
        ev->set_multicast_out(iface, 1, true);
        gen->set_multicast_out(iface, 1, true);
      }
    }
    if (!ev || !gen) {
      if (!warned) log_error("PTP niedostępny: " + err + ". Audio działa na zegarze lokalnym.");
      warned = true;
      for (int i = 0; i < 50 && running && ptp_running; ++i) platform::sleep_ns(100'000'000);
      continue;
    }
    warned = false;
    log_info("PTP: domena " + std::to_string(domain) + ", rola " + ptp_role_name(local.role) + ", priorytet " +
             std::to_string(local.announce.priority1) + "/" + std::to_string(local.announce.priority2) + " na " +
             (iface.empty() ? "domyślnej karcie" : iface));

    PtpPort port(static_cast<uint8_t>(domain), self_identity, clock.get(), local);
    PtpState last_state = PtpState::Listening;
    bool last_master = false;
    ClockIdentity last_gm;
    std::vector<platform::UdpSocket*> socks = {&*ev, &*gen};
    std::vector<uint8_t> ready;

    while (running && ptp_running) {
      platform::wait_readable(socks, 10, ready);
      for (size_t i = 0; i < socks.size(); ++i) {
        if (!ready[i]) continue;
        int n;
        while ((n = socks[i]->recv(buf, nullptr)) >= 0) {
          const int64_t t = platform::monotonic_ns();  // software receive timestamp
          port.on_message(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), t);
        }
      }
      if (const uint64_t v = ptp_local_version.load(); v != local_version) {
        local_version = v;
        std::lock_guard lock(config_mutex);
        local = ptp_local_from(config);
        port.set_local(local);
        log_info(std::string("PTP: rola ") + ptp_role_name(local.role) + ", priorytet " + std::to_string(local.announce.priority1) +
                 "/" + std::to_string(local.announce.priority2));
      }
      const int64_t now = platform::monotonic_ns();
      for (auto& out : port.poll_master(now)) {
        auto& sock = out.event_port ? *ev : *gen;
        const int64_t t1 = platform::monotonic_ns();  // software transmit timestamp
        sock.send_to(out.bytes, kPtpGroup, out.event_port ? kPtpEventPort : kPtpGeneralPort);
        if (out.needs_tx_time) port.on_sync_sent(out.sync_seq, t1);
      }
      if (auto req = port.poll_delay_req(now)) {
        const int64_t t3 = platform::monotonic_ns();
        ev->send_to(*req, kPtpGroup, kPtpEventPort);
        port.on_delay_req_sent(t3);
      }
      port.tick(now);

      const PtpStatus st = port.status();
      const bool stepped = port.consume_step();
      if (st.steps > 0) clock.set(port.clock(), stepped);
      if (stepped) log_info("PTP: korekta skokowa zegara (" + std::to_string(st.steps) + ")");
      if (st.state == PtpState::Locked || st.state == PtpState::Master) ptp_ever_locked = true;
      {
        std::lock_guard lock(ptp_mutex);
        ptp_status = st;
      }
      ShmPtpState shm_state = ShmPtpState::FreeRun;
      if (st.state == PtpState::Master) {
        shm_state = ShmPtpState::Master;
      } else if (st.state == PtpState::Locked) {
        shm_state = ShmPtpState::Locked;
      } else if (st.state == PtpState::Holdover) {
        shm_state = ShmPtpState::Holdover;
      } else if (st.steps > 0) {
        shm_state = ShmPtpState::Listening;
      }
      view->header().ptp_state.store(static_cast<uint32_t>(shm_state));

      if (st.has_master != last_master) {
        if (st.has_master) {
          log_info("PTP: master " + st.master.clock.to_string() + ", grandmaster " + st.grandmaster.to_string());
        } else {
          log_warning("PTP: brak mastera w sieci");
        }
        last_master = st.has_master;
        sap_dirty.fetch_add(1);  // SDP carries the grandmaster identity
      }
      if (st.grandmaster != last_gm) {
        last_gm = st.grandmaster;
        sap_dirty.fetch_add(1);
      }
      if (st.state != last_state) {
        (st.state == PtpState::Holdover ? log_warning : log_info)(std::string("PTP: ") + state_name(st.state));
        last_state = st.state;
      }
    }
  }
}

}  // namespace bam
