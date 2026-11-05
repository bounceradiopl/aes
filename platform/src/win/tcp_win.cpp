// SPDX-License-Identifier: GPL-3.0-or-later
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"
#include "bam/platform/udp.hpp"  // net_init(): WSAStartup, shared with the UDP sockets

namespace bam::platform {

namespace {
SOCKET sock(intptr_t fd) { return static_cast<SOCKET>(fd); }

std::string wsa_error(const char* what) { return std::string(what) + " (WSA " + std::to_string(WSAGetLastError()) + ")"; }

void set_nonblocking(SOCKET s) {
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
}

// Maps a WSA connect() error to user text; a handful of common cases get a specific message, the
// rest fall back to the raw WSA code.
std::string connect_error_text(int wsa_err) {
  switch (wsa_err) {
    case WSAECONNREFUSED:
      return "connect: połączenie odrzucone (WSA " + std::to_string(wsa_err) + ")";
    case WSAETIMEDOUT:
      return "connect: upłynął limit czasu (WSA " + std::to_string(wsa_err) + ")";
    case WSAENETUNREACH:
      return "connect: sieć nieosiągalna (WSA " + std::to_string(wsa_err) + ")";
    case WSAEHOSTUNREACH:
      return "connect: host nieosiągalny (WSA " + std::to_string(wsa_err) + ")";
    default:
      return "connect: (WSA " + std::to_string(wsa_err) + ")";
  }
}

// Remaining time until `deadline_ns`, clamped to >= 0; monotonic_ns() comes from platform/time.
int remaining_ms(int64_t deadline_ns) {
  const int64_t left = deadline_ns - monotonic_ns();
  return left > 0 ? static_cast<int>(left / 1'000'000) : 0;
}
}  // namespace

TcpStream::TcpStream(TcpStream&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalid; }

TcpStream& TcpStream::operator=(TcpStream&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    o.fd_ = kInvalid;
  }
  return *this;
}

TcpStream::~TcpStream() { close(); }

void TcpStream::close() {
  if (fd_ != kInvalid) {
    closesocket(sock(fd_));
    fd_ = kInvalid;
  }
}

std::optional<TcpStream> TcpStream::connect(const std::string& host, uint16_t port, int timeout_ms, std::string* error) {
  net_init();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const int gai_rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (gai_rc != 0) {
    if (error) *error = "nie można rozwiązać nazwy hosta " + host + ": " + gai_strerrorA(gai_rc);
    return std::nullopt;
  }
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  std::string last_error = "connect: brak adresów";
  for (addrinfo* a = res; a; a = a->ai_next) {
    const SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == INVALID_SOCKET) {
      last_error = wsa_error("socket");
      continue;
    }
    set_nonblocking(s);
    const int cr = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
    if (cr == 0) {
      freeaddrinfo(res);
      return TcpStream(static_cast<intptr_t>(s));
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      last_error = connect_error_text(WSAGetLastError());
      closesocket(s);
      continue;
    }
    // WSAPoll does not reliably report a failed non-blocking connect (e.g. a refused connection)
    // via POLLWRNORM -- it can take the full timeout to notice. select() with the socket in both
    // writefds (success) and exceptfds (failure) reports a refusal promptly; the real error then
    // comes from SO_ERROR.
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(s, &wfds);
    FD_SET(s, &efds);
    const int wait_ms = remaining_ms(deadline);
    timeval tv{wait_ms / 1000, (wait_ms % 1000) * 1000};
    const int sr = ::select(0, nullptr, &wfds, &efds, &tv);  // nfds is ignored by Winsock
    if (sr <= 0) {
      last_error = "connect: upłynął limit czasu";
      closesocket(s);
      continue;
    }
    int soerr = 0;
    int len = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
    if (FD_ISSET(s, &efds) || soerr != 0) {
      last_error = connect_error_text(soerr != 0 ? soerr : WSAGetLastError());
      closesocket(s);
      continue;
    }
    freeaddrinfo(res);
    return TcpStream(static_cast<intptr_t>(s));
  }
  freeaddrinfo(res);
  if (error) *error = last_error;
  return std::nullopt;
}

bool TcpStream::send_all(std::span<const uint8_t> data, int timeout_ms) {
  if (fd_ == kInvalid) return false;
  size_t sent = 0;
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  while (sent < data.size()) {
    const int wait_ms = remaining_ms(deadline);
    if (wait_ms <= 0 && monotonic_ns() >= deadline) return false;
    WSAPOLLFD p{sock(fd_), POLLWRNORM, 0};
    const int pr = WSAPoll(&p, 1, wait_ms);
    if (pr <= 0 || !(p.revents & POLLWRNORM)) return false;
    const int n = ::send(sock(fd_), reinterpret_cast<const char*>(data.data() + sent),
                          static_cast<int>(data.size() - sent), 0);
    if (n == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

int TcpStream::recv(std::span<uint8_t> buf, int timeout_ms) {
  if (fd_ == kInvalid) return -1;
  WSAPOLLFD p{sock(fd_), POLLRDNORM, 0};
  const int pr = WSAPoll(&p, 1, timeout_ms);
  if (pr <= 0 || !(p.revents & POLLRDNORM)) return -1;
  const int n = ::recv(sock(fd_), reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
  if (n == SOCKET_ERROR) return -1;
  return n;
}

int TcpStream::send_some(std::span<const uint8_t> data, int timeout_ms) {
  if (fd_ == kInvalid) return -1;
  if (data.empty()) return 0;
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  for (;;) {
    WSAPOLLFD p{sock(fd_), POLLWRNORM, 0};
    const int pr = WSAPoll(&p, 1, remaining_ms(deadline));
    if (pr == SOCKET_ERROR) return -1;
    if (pr == 0) return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    if (!(p.revents & POLLWRNORM)) return 0;
    const int len = static_cast<int>(std::min<size_t>(data.size(), 1 << 30));
    const int n = ::send(sock(fd_), reinterpret_cast<const char*>(data.data()), len, 0);
    if (n > 0) return n;
    if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
      if (monotonic_ns() >= deadline) return 0;
      continue;
    }
    return -1;
  }
}

int TcpStream::recv_some(std::span<uint8_t> buf, int timeout_ms) {
  if (fd_ == kInvalid) return -1;
  if (buf.empty()) return 0;
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  for (;;) {
    WSAPOLLFD p{sock(fd_), POLLRDNORM, 0};
    const int pr = WSAPoll(&p, 1, remaining_ms(deadline));
    if (pr == SOCKET_ERROR) return -1;
    if (pr == 0) return 0;
    if (!(p.revents & (POLLRDNORM | POLLERR | POLLHUP | POLLNVAL))) return 0;
    // POLLHUP/POLLERR without POLLRDNORM still goes through recv(): it reports EOF or the error.
    const int len = static_cast<int>(std::min<size_t>(buf.size(), 1 << 30));
    const int n = ::recv(sock(fd_), reinterpret_cast<char*>(buf.data()), len, 0);
    if (n > 0) return n;
    if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
      if (monotonic_ns() >= deadline) return 0;
      continue;
    }
    return -1;  // 0 = closed by the peer, or an error
  }
}

TcpListener::TcpListener(TcpListener&& o) noexcept : fd_(o.fd_), port_(o.port_) { o.fd_ = kInvalid; }

TcpListener& TcpListener::operator=(TcpListener&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    port_ = o.port_;
    o.fd_ = kInvalid;
  }
  return *this;
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
  if (fd_ != kInvalid) {
    closesocket(sock(fd_));
    fd_ = kInvalid;
  }
}

std::optional<TcpListener> TcpListener::listen(const std::string& ip, uint16_t port, std::string* error) {
  net_init();
  const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    if (error) *error = wsa_error("socket");
    return std::nullopt;
  }
  BOOL one = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (ip.empty()) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) *error = wsa_error(("bind na porcie " + std::to_string(port)).c_str());
    closesocket(s);
    return std::nullopt;
  }
  if (::listen(s, 16) != 0) {
    if (error) *error = wsa_error("listen");
    closesocket(s);
    return std::nullopt;
  }
  int len = sizeof(addr);
  getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  set_nonblocking(s);
  TcpListener l;
  l.fd_ = static_cast<intptr_t>(s);
  l.port_ = ntohs(addr.sin_port);
  return l;
}

std::optional<TcpStream> TcpListener::accept(int timeout_ms) {
  WSAPOLLFD p{sock(fd_), POLLRDNORM, 0};
  const int pr = WSAPoll(&p, 1, timeout_ms);
  if (pr <= 0 || !(p.revents & POLLRDNORM)) return std::nullopt;
  const SOCKET c = ::accept(sock(fd_), nullptr, nullptr);
  if (c == INVALID_SOCKET) return std::nullopt;
  set_nonblocking(c);
  return TcpStream(static_cast<intptr_t>(c));
}

}  // namespace bam::platform
