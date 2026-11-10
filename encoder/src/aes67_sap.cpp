// SPDX-License-Identifier: GPL-3.0-or-later
#include "aes67_sap.hpp"

#include <optional>
#include <sstream>
#include <vector>

#include "bam/constants.hpp"
#include "bam/ipv4.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"

namespace bam::encoder::detail {

namespace {
constexpr int64_t kAnnounceNs = 30LL * 1'000'000'000;
constexpr int64_t kExpireCheckNs = 10LL * 1'000'000'000;
constexpr int64_t kRetryNs = 5LL * 1'000'000'000;

// The session id of the SDP's o= line.
std::string session_id_of(const std::string& sdp) {
  std::istringstream in(sdp);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("o=", 0) != 0) continue;
    std::istringstream tok(line.substr(2));
    std::string user, id;
    tok >> user >> id;
    return id;
  }
  return {};
}
}  // namespace

Aes67Sap::Aes67Sap() : thread_([this] { run(); }) {}

Aes67Sap::~Aes67Sap() {
  running_ = false;
  thread_.join();  // run() withdraws what it announced
}

void Aes67Sap::set_announcements(const std::string& iface_ip, std::map<std::string, std::string> sdps) {
  std::lock_guard lock(m_);
  if (iface_ == iface_ip && sdps_ == sdps) return;
  iface_ = iface_ip;
  sdps_ = std::move(sdps);
  if (sdps_.empty()) own_session_ids_.clear();
  for (const auto& [id, sdp] : sdps_) own_session_ids_.insert(session_id_of(sdp));
  ++generation_;
}

std::string Aes67Sap::conflict(const std::string& address, const std::string& own_origin) const {
  std::lock_guard lock(m_);
  for (const DiscoveredStream& s : directory_.list()) {
    if (s.parsed.multicast_address != address) continue;
    if (s.origin == own_origin && own_session_ids_.count(session_id_of(s.sdp))) continue;
    return s.name + " (" + s.origin + ")";
  }
  return {};
}

void Aes67Sap::run() {
  std::vector<uint8_t> buf(4096);
  std::vector<uint8_t> ready;
  std::map<std::string, std::string> announced;  // station -> SDP last announced
  std::optional<platform::UdpSocket> sock;
  std::string sock_iface;
  bool warned = false;
  uint64_t seen = 0;
  int64_t last_announce = 0, last_expire = 0, retry_at = 0;
  Ipv4 origin{0, 0, 0, 0};

  auto send = [&](bool deletion, const std::string& sdp) {
    if (sock) sock->send_to(build_sap(deletion, sap_hash(sdp), origin, sdp), kSapGroup, kSapPort);
  };
  auto withdraw_all = [&] {
    for (const auto& [id, sdp] : announced) send(true, sdp);
    announced.clear();
  };

  while (running_) {
    std::string iface;
    std::map<std::string, std::string> sdps;
    uint64_t generation;
    {
      std::lock_guard lock(m_);
      iface = iface_;
      sdps = sdps_;
      generation = generation_;
    }
    if (sdps.empty()) {
      withdraw_all();
      sock.reset();
      warned = false;
      {
        std::lock_guard lock(m_);
        directory_ = SapDirectory();
      }
      platform::sleep_ns(50'000'000);
      continue;
    }
    const int64_t now = platform::monotonic_ns();
    if (!sock || sock_iface != iface) {
      if (sock) announced.clear();  // the old card is not reachable any more: receivers expire it
      sock.reset();
      {
        // our earlier announcements carry the old origin: they would read as another device
        std::lock_guard lock(m_);
        directory_ = SapDirectory();
      }
      if (now < retry_at) {
        platform::sleep_ns(50'000'000);
        continue;
      }
      std::string err;
      sock = platform::UdpSocket::open(kSapPort, &err);
      if (sock && !sock->join(kSapGroup, iface, "", &err)) sock.reset();
      if (!sock) {
        if (!warned) log_warning("SAP niedostępny: " + err);
        warned = true;
        retry_at = now + kRetryNs;
        continue;
      }
      warned = false;
      sock->set_multicast_out(iface, kMulticastTtl, true);
      sock_iface = iface;
      const auto ip = parse_ipv4(iface);
      origin = ip ? *ip : Ipv4{0, 0, 0, 0};
      seen = 0;  // announce on the new interface
    }

    std::vector<platform::UdpSocket*> socks = {&*sock};
    platform::wait_readable(socks, 50, ready);
    const int64_t t = platform::monotonic_ns();
    if (ready[0]) {
      int n;
      std::lock_guard lock(m_);
      while ((n = sock->recv(buf, nullptr)) >= 0) directory_.on_packet(std::span<const uint8_t>(buf.data(), size_t(n)), t);
    }
    if (t - last_expire > kExpireCheckNs) {
      std::lock_guard lock(m_);
      directory_.expire(t);
      last_expire = t;
    }
    if (generation != seen || t - last_announce > kAnnounceNs) {
      seen = generation;
      last_announce = t;
      for (auto it = announced.begin(); it != announced.end();) {
        if (sdps.count(it->first)) {
          ++it;
        } else {
          send(true, it->second);
          it = announced.erase(it);
        }
      }
      for (const auto& [id, sdp] : sdps) {
        auto old = announced.find(id);
        if (old != announced.end() && old->second != sdp) send(true, old->second);
        send(false, sdp);
        announced[id] = sdp;
      }
    }
  }
  withdraw_all();
}

}  // namespace bam::encoder::detail
