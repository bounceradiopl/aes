// SPDX-License-Identifier: GPL-3.0-or-later
// SAP for the AES67 outputs (spec §5): announces the outputs' SDPs on kSapGroup:kSapPort every
// 30 s and on change, withdraws them, and keeps the directory of what other devices announce
// (for the address conflict warning). Mirrors BounceAES's sap_worker. Its own thread does all the
// network I/O; the methods only take a short lock.
#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "bam/sap.hpp"

namespace bam::encoder::detail {

class Aes67Sap {
 public:
  Aes67Sap();
  ~Aes67Sap();  // withdraws everything announced, stops
  Aes67Sap(const Aes67Sap&) = delete;
  Aes67Sap& operator=(const Aes67Sap&) = delete;

  // The SDPs to announce (key: station id); a changed SDP withdraws the old one first, an empty
  // map withdraws everything and closes the socket.
  void set_announcements(const std::string& iface_ip, std::map<std::string, std::string> sdps);
  // Another announcer's stream on `address` ("<s=> (<origin>)"), or empty. Announcements of this
  // encoder are skipped: origin == own_origin AND an SDP session id of ours (BounceAES Master on
  // the same computer has the same origin, so the origin alone would hide a real conflict).
  std::string conflict(const std::string& address, const std::string& own_origin) const;

 private:
  void run();

  mutable std::mutex m_;
  std::string iface_;
  std::map<std::string, std::string> sdps_;
  std::set<std::string> own_session_ids_;
  uint64_t generation_ = 0;
  SapDirectory directory_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

}  // namespace bam::encoder::detail
