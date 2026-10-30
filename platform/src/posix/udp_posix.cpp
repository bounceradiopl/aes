// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "bam/platform/udp.hpp"

namespace bam::platform {

namespace {
in_addr to_addr(const std::string& ip) {
  in_addr a{};
  a.s_addr = ip.empty() ? htonl(INADDR_ANY) : inet_addr(ip.c_str());
  return a;
}
}  // namespace

bool net_init() { return true; }

std::vector<NetInterface> list_interfaces() {
  std::vector<NetInterface> out;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    NetInterface n;
    n.name = i->ifa_name;
    n.description = i->ifa_name;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof(buf));
    n.ipv4 = buf;
    n.up = (i->ifa_flags & IFF_UP) != 0;
    n.loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
    out.push_back(n);
  }
  freeifaddrs(list);
  return out;
}

std::string default_interface_ip() {
  for (const auto& i : list_interfaces())
    if (i.up && !i.loopback) return i.ipv4;
  return "";
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_), joined_(std::move(o.joined_)) { o.fd_ = kInvalid; }

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    joined_ = std::move(o.joined_);
    o.fd_ = kInvalid;
  }
  return *this;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
  if (fd_ != kInvalid) {
    leave_all();
    ::close(static_cast<int>(fd_));
    fd_ = kInvalid;
  }
}

std::optional<UdpSocket> UdpSocket::open(uint16_t port, std::string* error) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return std::nullopt;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
#ifdef IP_MULTICAST_ALL
  int zero = 0;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero));  // Linux: only joined groups
#endif
  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) *error = "bind na porcie " + std::to_string(port) + ": " + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  UdpSocket s;
  s.fd_ = fd;
  return s;
}

bool UdpSocket::join(const std::string& group, const std::string& iface, const std::string& source, std::string* error) {
  int rc;
  if (!source.empty()) {
    ip_mreq_source m{};
    m.imr_multiaddr = to_addr(group);
    m.imr_interface = to_addr(iface);
    m.imr_sourceaddr = to_addr(source);
    rc = setsockopt(static_cast<int>(fd_), IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &m, sizeof(m));
  } else {
    ip_mreq m{};
    m.imr_multiaddr = to_addr(group);
    m.imr_interface = to_addr(iface);
    rc = setsockopt(static_cast<int>(fd_), IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
  }
  if (rc != 0) {
    if (error) *error = "dołączenie do " + group + ": " + std::strerror(errno);
    return false;
  }
  joined_.push_back({group, iface, source});
  return true;
}

void UdpSocket::leave_all() {
  for (const auto& j : joined_) {
    if (!j.source.empty()) {
      ip_mreq_source m{};
      m.imr_multiaddr = to_addr(j.group);
      m.imr_interface = to_addr(j.iface);
      m.imr_sourceaddr = to_addr(j.source);
      setsockopt(static_cast<int>(fd_), IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, &m, sizeof(m));
    } else {
      ip_mreq m{};
      m.imr_multiaddr = to_addr(j.group);
      m.imr_interface = to_addr(j.iface);
      setsockopt(static_cast<int>(fd_), IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m));
    }
  }
  joined_.clear();
}

bool UdpSocket::set_multicast_out(const std::string& iface, int ttl, bool loopback) {
  const int fd = static_cast<int>(fd_);
  in_addr a = to_addr(iface);
  unsigned char t = static_cast<unsigned char>(ttl);
  unsigned char loop = loopback ? 1 : 0;
  bool ok = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a)) == 0;
  ok &= setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof(t)) == 0;
  ok &= setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) == 0;
  int tos = 46 << 2;  // DSCP EF, as recommended for AES67 audio
  setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
  return ok;
}

bool UdpSocket::send_to(std::span<const uint8_t> data, const std::string& ip, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = to_addr(ip);
  return ::sendto(static_cast<int>(fd_), data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
         static_cast<ssize_t>(data.size());
}

int UdpSocket::recv(std::span<uint8_t> buf, std::string* from_ip) {
  sockaddr_in from{};
  socklen_t len = sizeof(from);
  ssize_t n = ::recvfrom(static_cast<int>(fd_), buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
  if (n < 0) return -1;
  if (from_ip) {
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, b, sizeof(b));
    *from_ip = b;
  }
  return static_cast<int>(n);
}

int wait_readable(std::span<UdpSocket* const> sockets, int timeout_ms, std::vector<uint8_t>& ready) {
  std::vector<pollfd> fds(sockets.size());
  for (size_t i = 0; i < sockets.size(); ++i) {
    fds[i].fd = sockets[i] ? static_cast<int>(sockets[i]->native()) : -1;
    fds[i].events = POLLIN;
  }
  ready.assign(sockets.size(), 0);
  int n = ::poll(fds.data(), fds.size(), timeout_ms);
  if (n <= 0) return 0;
  for (size_t i = 0; i < fds.size(); ++i) ready[i] = (fds[i].revents & POLLIN) ? 1 : 0;
  return n;
}

}  // namespace bam::platform
