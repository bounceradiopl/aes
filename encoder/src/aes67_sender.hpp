// SPDX-License-Identifier: GPL-3.0-or-later
// The sender thread of the AES67 outputs (spec §3): one UDP socket on the AoIP card, woken every
// 0.25 ms at "Pro Audio" priority like BounceAES Master's tx_loop, pumps every output on the
// media clock. The methods may be called from any thread (the engine calls them under its lock).
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aes67_output.hpp"
#include "bam/clock.hpp"
#include "bam/platform/udp.hpp"

namespace bam::encoder::detail {

namespace testing {
void fail_next_aes67_interfaces(int n);  // the next n set_interface() calls fail (0 = off)
}  // namespace testing

class Aes67Sender {
 public:
  using Outputs = std::vector<std::pair<std::string, std::shared_ptr<Aes67Output>>>;  // multicast address, output

  explicit Aes67Sender(const ClockSource& clock);
  ~Aes67Sender();  // stop()
  Aes67Sender(const Aes67Sender&) = delete;
  Aes67Sender& operator=(const Aes67Sender&) = delete;

  // The outputs to send. An output left out is never pumped again once this returns, and the
  // list let go of is released here, never on the sender thread.
  void set_outputs(Outputs outputs_with_address);
  // (Re)sets the multicast interface of the socket (opening it again if it failed); nothing is
  // sent while that fails, so packets never leave by another card.
  void set_interface(const std::string& iface_ip);
  // false: no packets at all (waiting for PTP); the outputs are not pumped, so nothing queues up.
  void set_allowed(bool send);
  std::string error() const;  // the socket's error, Polish, else empty
  void stop();

 private:
  void run();
  void set_error(std::string text);

  const ClockSource& clock_;
  // Held by the thread through each pass: outputs_, socket_, socket_ok_.
  mutable std::mutex m_;
  Outputs outputs_;
  std::optional<platform::UdpSocket> socket_;
  bool socket_ok_ = false;  // open, with the multicast interface set
  // the error text has its own lock, so error() never waits for a pass
  mutable std::mutex err_m_;
  std::string error_;
  std::atomic<bool> has_error_{false};
  std::atomic<bool> allowed_{false};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace bam::encoder::detail
