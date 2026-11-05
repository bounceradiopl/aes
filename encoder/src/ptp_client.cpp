// SPDX-License-Identifier: GPL-3.0-or-later
// PTP for the Bounce Encoder: slave only (spec §4.1), modelled on the BounceAES Master PTP worker
// (engine/src/ptp_worker.cpp) without the master role. Until it follows a master the media clock
// keeps free-running on the local clock.
#include <random>

#include "bam/constants.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "engine_internal.hpp"

namespace bam::encoder::detail {

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
      return "master";
  }
  return "?";
}

ClockIdentity random_identity() {
  std::random_device rd;
  ClockIdentity id;
  for (auto& b : id.bytes) b = static_cast<uint8_t>(rd());
  id.bytes[3] = 0xFF;  // EUI-64 style marker, like identities derived from a MAC
  id.bytes[4] = 0xFE;
  return id;
}

bool sleep_while(const std::atomic<bool>& running, int64_t ns) {
  const int64_t end = platform::monotonic_ns() + ns;
  while (running && platform::monotonic_ns() < end) platform::sleep_ns(50'000'000);
  return running;
}
}  // namespace

PtpClient::PtpClient(ClockSource& clock, int domain, std::string iface)
    : clock_(clock), domain_(domain), iface_(std::move(iface)), self_(random_identity()) {
  status_.domain = static_cast<uint8_t>(domain);
  thread_ = std::thread([this] { run(); });
}

PtpClient::~PtpClient() { stop(); }

void PtpClient::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

PtpStatus PtpClient::status() const {
  std::lock_guard lock(m_);
  return status_;
}

std::string PtpClient::error() const {
  std::lock_guard lock(m_);
  return error_;
}

void PtpClient::run() {
  std::vector<uint8_t> buf(1500);
  bool warned = false;
  while (running_) {
    std::string err;
    auto ev = platform::UdpSocket::open(kPtpEventPort, &err);
    auto gen = ev ? platform::UdpSocket::open(kPtpGeneralPort, &err) : std::nullopt;
    if (ev && gen) {
      if (!ev->join(kPtpGroup, iface_, "", &err) || !gen->join(kPtpGroup, iface_, "", &err)) {
        ev.reset();
      } else {
        // loopback: a master on the same host must see our Delay_Req
        ev->set_multicast_out(iface_, 1, true);
        gen->set_multicast_out(iface_, 1, true);
      }
    }
    if (!ev || !gen) {
      {
        std::lock_guard lock(m_);
        error_ = "PTP niedostępny: " + err;
      }
      if (!warned) log_error("PTP niedostępny: " + err + ". Wejścia działają na zegarze lokalnym.");
      warned = true;
      sleep_while(running_, 5'000'000'000);
      continue;
    }
    {
      std::lock_guard lock(m_);
      error_.clear();
    }
    warned = false;
    log_info("PTP (slave): domena " + std::to_string(domain_) + " na " + (iface_.empty() ? "domyślnej karcie" : iface_));

    PtpPort port(static_cast<uint8_t>(domain_), self_, clock_.get());  // default role: slave, never a master
    PtpState last_state = PtpState::Listening;
    bool last_master = false;
    std::vector<platform::UdpSocket*> socks = {&*ev, &*gen};
    std::vector<uint8_t> ready;
    while (running_) {
      platform::wait_readable(socks, 10, ready);
      for (size_t i = 0; i < socks.size(); ++i) {
        if (!ready[i]) continue;
        int n;
        while ((n = socks[i]->recv(buf, nullptr)) >= 0) {
          const int64_t t = platform::monotonic_ns();  // software receive timestamp
          port.on_message(std::span<const uint8_t>(buf.data(), static_cast<size_t>(n)), t);
        }
      }
      const int64_t now = platform::monotonic_ns();
      if (auto req = port.poll_delay_req(now)) {
        const int64_t t3 = platform::monotonic_ns();
        ev->send_to(*req, kPtpGroup, kPtpEventPort);
        port.on_delay_req_sent(t3);
      }
      port.tick(now);

      const PtpStatus st = port.status();
      const bool stepped = port.consume_step();
      if (st.steps > 0) {
        clock_.set(port.clock(), stepped);
        ever_stepped_ = true;
      }
      if (stepped) log_info("PTP: korekta skokowa zegara (" + std::to_string(st.steps) + ")");
      {
        std::lock_guard lock(m_);
        status_ = st;
      }
      if (st.has_master != last_master) {
        if (st.has_master) {
          log_info("PTP: master " + st.master.clock.to_string() + ", grandmaster " + st.grandmaster.to_string());
        } else {
          log_warning("PTP: brak mastera w sieci");
        }
        last_master = st.has_master;
      }
      if (st.state != last_state) {
        (st.state == PtpState::Holdover ? log_warning : log_info)(std::string("PTP: ") + state_name(st.state));
        last_state = st.state;
      }
    }
  }
}

}  // namespace bam::encoder::detail
