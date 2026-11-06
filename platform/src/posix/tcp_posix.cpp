// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"

namespace bam::platform {

namespace {
void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

// A send() on a socket whose peer already reset the connection raises SIGPIPE by default and
// kills the process. Linux has MSG_NOSIGNAL per-call (used in send_all() below); macOS/BSD have
// neither MSG_NOSIGNAL nor a way to ignore SIGPIPE just for one send(), only this per-socket flag,
// so every TcpStream fd (connect()-created and accept()-returned) gets it where it exists.
void set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

// Remaining time until `deadline_ns`, clamped to >= 0; monotonic_ns() comes from platform/time.
int remaining_ms(int64_t deadline_ns) {
  const int64_t left = deadline_ns - monotonic_ns();
  return left > 0 ? static_cast<int>(left / 1'000'000) : 0;
}

// poll() a single fd, retrying when interrupted by a signal, until `deadline_ns` (monotonic).
// Otherwise behaves exactly like a plain ::poll() call: callers still check the return value and
// p.revents themselves.
int poll_fd(pollfd& p, int64_t deadline_ns) {
  for (;;) {
    p.revents = 0;
    const int pr = ::poll(&p, 1, remaining_ms(deadline_ns));
    if (pr < 0 && errno == EINTR && monotonic_ns() < deadline_ns) continue;
    return pr;
  }
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
    ::close(static_cast<int>(fd_));
    fd_ = kInvalid;
  }
}

std::optional<TcpStream> TcpStream::connect(const std::string& host, uint16_t port, int timeout_ms, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const int gai_rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (gai_rc != 0) {
    if (error) *error = "nie można rozwiązać nazwy hosta " + host + ": " + gai_strerror(gai_rc);
    return std::nullopt;
  }
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  std::string last_error = "connect: brak adresów";
  for (addrinfo* a = res; a; a = a->ai_next) {
    const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) {
      last_error = std::string("socket: ") + std::strerror(errno);
      continue;
    }
    set_nonblocking(fd);
    set_no_sigpipe(fd);
    const int cr = ::connect(fd, a->ai_addr, a->ai_addrlen);
    if (cr == 0) {
      freeaddrinfo(res);
      return TcpStream(fd);
    }
    if (errno != EINPROGRESS) {
      last_error = "connect: " + std::string(std::strerror(errno));
      ::close(fd);
      continue;
    }
    pollfd p{fd, POLLOUT, 0};
    const int pr = poll_fd(p, deadline);
    if (pr <= 0) {
      last_error = "connect: upłynął limit czasu";
      ::close(fd);
      continue;
    }
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
    if (soerr != 0) {
      last_error = "connect: " + std::string(std::strerror(soerr));
      ::close(fd);
      continue;
    }
    freeaddrinfo(res);
    return TcpStream(fd);
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
    pollfd p{static_cast<int>(fd_), POLLOUT, 0};
    const int pr = poll_fd(p, deadline);
    if (pr <= 0 || !(p.revents & POLLOUT)) return false;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const ssize_t n = ::send(static_cast<int>(fd_), data.data() + sent, data.size() - sent, flags);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

int TcpStream::recv(std::span<uint8_t> buf, int timeout_ms) {
  if (fd_ == kInvalid) return -1;
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  pollfd p{static_cast<int>(fd_), POLLIN, 0};
  const int pr = poll_fd(p, deadline);
  // a close or reset without POLLIN still goes through recv(): it reports the end (0) or the error
  if (pr <= 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR))) return -1;
  const ssize_t n = ::recv(static_cast<int>(fd_), buf.data(), buf.size(), 0);
  if (n < 0) return -1;
  return static_cast<int>(n);
}

int TcpStream::send_some(std::span<const uint8_t> data, int timeout_ms) {
  if (fd_ == kInvalid) return -1;
  if (data.empty()) return 0;
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  for (;;) {
    pollfd p{static_cast<int>(fd_), POLLOUT, 0};
    const int pr = poll_fd(p, deadline);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    if (!(p.revents & POLLOUT)) return 0;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    // At most 1 GiB per call so the byte count always fits the int result (as on Windows).
    const size_t len = std::min<size_t>(data.size(), size_t(1) << 30);
    const ssize_t n = ::send(static_cast<int>(fd_), data.data(), len, flags);
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
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
    pollfd p{static_cast<int>(fd_), POLLIN, 0};
    const int pr = poll_fd(p, deadline);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    if (!(p.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) return 0;
    // POLLHUP/POLLERR without POLLIN still goes through recv(): it reports EOF or the error.
    const size_t len = std::min<size_t>(buf.size(), size_t(1) << 30);  // fits the int result
    const ssize_t n = ::recv(static_cast<int>(fd_), buf.data(), len, 0);
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
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
    ::close(static_cast<int>(fd_));
    fd_ = kInvalid;
  }
}

std::optional<TcpListener> TcpListener::listen(const std::string& ip, uint16_t port, std::string* error) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return std::nullopt;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = ip.empty() ? htonl(INADDR_ANY) : inet_addr(ip.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) *error = "bind na porcie " + std::to_string(port) + ": " + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  if (::listen(fd, 16) != 0) {
    if (error) *error = std::string("listen: ") + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  set_nonblocking(fd);
  TcpListener l;
  l.fd_ = fd;
  l.port_ = ntohs(addr.sin_port);
  return l;
}

std::optional<TcpStream> TcpListener::accept(int timeout_ms) {
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  pollfd p{static_cast<int>(fd_), POLLIN, 0};
  const int pr = poll_fd(p, deadline);
  if (pr <= 0 || !(p.revents & POLLIN)) return std::nullopt;
  const int cfd = ::accept(static_cast<int>(fd_), nullptr, nullptr);
  if (cfd < 0) return std::nullopt;
  set_nonblocking(cfd);
  set_no_sigpipe(cfd);
  return TcpStream(cfd);
}

}  // namespace bam::platform
