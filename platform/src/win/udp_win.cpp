// SPDX-License-Identifier: GPL-3.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <mswsock.h>

#include <cstring>
#include <mutex>

#include "bam/platform/udp.hpp"
#include "win_util.hpp"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace bam::platform {

namespace {
in_addr to_addr(const std::string& ip) {
  in_addr a{};
  if (ip.empty()) {
    a.s_addr = htonl(INADDR_ANY);
  } else {
    inet_pton(AF_INET, ip.c_str(), &a);
  }
  return a;
}

std::string wsa_error(const char* what) { return std::string(what) + " (WSA " + std::to_string(WSAGetLastError()) + ")"; }

SOCKET sock(intptr_t fd) { return static_cast<SOCKET>(fd); }

LPFN_WSARECVMSG recvmsg_fn(SOCKET s) {
  static LPFN_WSARECVMSG fn = nullptr;
  if (!fn) {
    GUID guid = WSAID_WSARECVMSG;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn, sizeof(fn), &bytes, nullptr, nullptr);
  }
  return fn;
}
}  // namespace

bool net_init() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
  });
  return ok;
}

std::vector<NetInterface> list_interfaces() {
  net_init();
  std::vector<NetInterface> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf(size);
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size);
  }
  if (rc != NO_ERROR) return out;
  for (auto* a = addrs; a; a = a->Next) {
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
      NetInterface n;
      n.name = narrow(a->FriendlyName);
      n.description = narrow(a->Description);
      n.ipv4 = ip;
      n.up = a->OperStatus == IfOperStatusUp;
      n.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      out.push_back(n);
    }
  }
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
    closesocket(sock(fd_));
    fd_ = kInvalid;
  }
}

std::optional<UdpSocket> UdpSocket::open(uint16_t port, std::string* error) {
  net_init();
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    if (error) *error = wsa_error("socket");
    return std::nullopt;
  }
  BOOL one = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
  BOOL no = FALSE;
  DWORD ret = 0;
  WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &ret, nullptr, nullptr);
  DWORD pktinfo = 1;
  setsockopt(s, IPPROTO_IP, IP_PKTINFO, reinterpret_cast<const char*>(&pktinfo), sizeof(pktinfo));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) *error = wsa_error(("bind na porcie " + std::to_string(port)).c_str());
    closesocket(s);
    return std::nullopt;
  }
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
  UdpSocket u;
  u.fd_ = static_cast<intptr_t>(s);
  return u;
}

bool UdpSocket::join(const std::string& group, const std::string& iface, const std::string& source, std::string* error) {
  int rc;
  if (!source.empty()) {
    ip_mreq_source m{};
    m.imr_multiaddr = to_addr(group);
    m.imr_interface = to_addr(iface);
    m.imr_sourceaddr = to_addr(source);
    rc = setsockopt(sock(fd_), IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m));
  } else {
    ip_mreq m{};
    m.imr_multiaddr = to_addr(group);
    m.imr_interface = to_addr(iface);
    rc = setsockopt(sock(fd_), IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m));
  }
  if (rc != 0) {
    if (error) *error = wsa_error(("dołączenie do " + group).c_str());
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
      setsockopt(sock(fd_), IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m));
    } else {
      ip_mreq m{};
      m.imr_multiaddr = to_addr(j.group);
      m.imr_interface = to_addr(j.iface);
      setsockopt(sock(fd_), IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m));
    }
  }
  joined_.clear();
}

bool UdpSocket::set_multicast_out(const std::string& iface, int ttl, bool loopback) {
  in_addr a = to_addr(iface);
  DWORD t = static_cast<DWORD>(ttl);
  DWORD loop = loopback ? 1 : 0;
  bool ok = setsockopt(sock(fd_), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&a), sizeof(a)) == 0;
  ok &= setsockopt(sock(fd_), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&t), sizeof(t)) == 0;
  ok &= setsockopt(sock(fd_), IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop)) == 0;
  return ok;
}

bool UdpSocket::send_to(std::span<const uint8_t> data, const std::string& ip, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = to_addr(ip);
  return ::sendto(sock(fd_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == static_cast<int>(data.size());
}

int UdpSocket::recv(std::span<uint8_t> buf, std::string* from_ip, std::string* to_ip) {
  sockaddr_in from{};
  WSABUF wbuf{static_cast<ULONG>(buf.size()), reinterpret_cast<char*>(buf.data())};
  alignas(WSACMSGHDR) char control[128];
  WSAMSG msg{};
  msg.name = reinterpret_cast<sockaddr*>(&from);
  msg.namelen = sizeof(from);
  msg.lpBuffers = &wbuf;
  msg.dwBufferCount = 1;
  msg.Control.buf = control;
  msg.Control.len = sizeof(control);
  DWORD n = 0;
  LPFN_WSARECVMSG fn = recvmsg_fn(sock(fd_));
  if (!fn || fn(sock(fd_), &msg, &n, nullptr, nullptr) == SOCKET_ERROR) return -1;
  char b[INET_ADDRSTRLEN];
  if (from_ip) {
    inet_ntop(AF_INET, &from.sin_addr, b, sizeof(b));
    *from_ip = b;
  }
  if (to_ip) {
    to_ip->clear();
    for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level != IPPROTO_IP || c->cmsg_type != IP_PKTINFO) continue;
      IN_PKTINFO pi{};
      std::memcpy(&pi, WSA_CMSG_DATA(c), sizeof(pi));
      inet_ntop(AF_INET, &pi.ipi_addr, b, sizeof(b));
      *to_ip = b;
    }
  }
  return static_cast<int>(n);
}

int wait_readable(std::span<UdpSocket* const> sockets, int timeout_ms, std::vector<uint8_t>& ready) {
  std::vector<WSAPOLLFD> fds(sockets.size());
  for (size_t i = 0; i < sockets.size(); ++i) {
    fds[i].fd = sockets[i] ? sock(sockets[i]->native()) : INVALID_SOCKET;
    fds[i].events = POLLRDNORM;
  }
  ready.assign(sockets.size(), 0);
  if (fds.empty()) {
    Sleep(static_cast<DWORD>(timeout_ms));
    return 0;
  }
  int n = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
  if (n <= 0) return 0;
  for (size_t i = 0; i < fds.size(); ++i) ready[i] = (fds[i].revents & POLLRDNORM) ? 1 : 0;
  return n;
}

}  // namespace bam::platform
