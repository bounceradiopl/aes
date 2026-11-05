// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace bam::platform {

// Non-blocking IPv4 TCP client connection (Icecast PUT/SOURCE uploads).
class TcpStream {
 public:
  TcpStream() = default;
  TcpStream(TcpStream&& other) noexcept;
  TcpStream& operator=(TcpStream&& other) noexcept;
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;
  ~TcpStream();

  // Resolves host (getaddrinfo, IPv4) and connects within timeout_ms.
  static std::optional<TcpStream> connect(const std::string& host, uint16_t port, int timeout_ms, std::string* error);

  bool send_all(std::span<const uint8_t> data, int timeout_ms);  // false on error/timeout
  int recv(std::span<uint8_t> buf, int timeout_ms);              // bytes, 0 = closed, -1 = timeout/error
  // Partial I/O for callers that wait in short slices (to stay stoppable) and must know exactly
  // how much went through: bytes moved (> 0), 0 = nothing within timeout_ms (0 = just check),
  // -1 = error or connection closed by the peer.
  int send_some(std::span<const uint8_t> data, int timeout_ms);
  int recv_some(std::span<uint8_t> buf, int timeout_ms);
  void close();
  bool valid() const { return fd_ != kInvalid; }
  intptr_t native() const { return fd_; }

 private:
  friend class TcpListener;
  static constexpr intptr_t kInvalid = -1;
  explicit TcpStream(intptr_t fd) : fd_(fd) {}
  intptr_t fd_ = kInvalid;
};

// TCP listener used by tests and local tools (e.g. the fake Icecast server).
class TcpListener {
 public:
  TcpListener() = default;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  ~TcpListener();

  static std::optional<TcpListener> listen(const std::string& ip, uint16_t port, std::string* error);  // port 0 = any
  uint16_t port() const { return port_; }
  std::optional<TcpStream> accept(int timeout_ms);
  bool valid() const { return fd_ != kInvalid; }

 private:
  static constexpr intptr_t kInvalid = -1;
  void close();
  intptr_t fd_ = kInvalid;
  uint16_t port_ = 0;
};

}  // namespace bam::platform
