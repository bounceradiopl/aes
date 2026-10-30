// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bam {

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> msg);
std::vector<uint8_t> pbkdf2_sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations, size_t len);
std::string to_hex(std::span<const uint8_t> data);
std::string random_hex(size_t bytes);

// "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>"
std::string make_password_hash(std::string_view password);
bool verify_password(std::string_view password, std::string_view stored);

// Panel login sessions with a sliding 12 h lifetime. Thread-safe.
class SessionStore {
 public:
  std::string create(int64_t now_ns);
  bool valid(std::string_view token, int64_t now_ns);
  void remove(std::string_view token);

 private:
  std::mutex mutex_;
  std::map<std::string, int64_t, std::less<>> expiry_;
};

}  // namespace bam
