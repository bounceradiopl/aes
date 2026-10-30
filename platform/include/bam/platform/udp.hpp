// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bam::platform {

struct NetInterface {
  std::string name;
  std::string description;
  std::string ipv4;
  bool up = false;
  bool loopback = false;
};

bool net_init();  // WSAStartup on Windows; idempotent
std::vector<NetInterface> list_interfaces();
std::string default_interface_ip();  // first up, non-loopback IPv4, or "" when none

// Non-blocking IPv4 UDP socket bound to 0.0.0.0:port with address reuse, so several sockets
// (and other AES67 software) can share port 5004/319/320/9875. Each socket only receives the
// multicast groups it joined itself.
class UdpSocket {
 public:
  UdpSocket() = default;
  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  ~UdpSocket();

  static std::optional<UdpSocket> open(uint16_t port, std::string* error);

  // source != "" uses source-specific multicast (IGMPv3).
  bool join(const std::string& group, const std::string& iface_ip, const std::string& source, std::string* error);
  void leave_all();
  bool set_multicast_out(const std::string& iface_ip, int ttl, bool loopback);
  bool send_to(std::span<const uint8_t> data, const std::string& ip, uint16_t port);
  int recv(std::span<uint8_t> buf, std::string* from_ip);  // bytes, or -1 when nothing pending / error

  intptr_t native() const { return fd_; }
  bool valid() const { return fd_ != kInvalid; }

 private:
  static constexpr intptr_t kInvalid = -1;
  struct Membership {
    std::string group, iface, source;
  };
  void close();
  intptr_t fd_ = kInvalid;
  std::vector<Membership> joined_;
};

// Waits until at least one socket is readable. ready[i] is set to 1 for readable sockets.
int wait_readable(std::span<UdpSocket* const> sockets, int timeout_ms, std::vector<uint8_t>& ready);

}  // namespace bam::platform
