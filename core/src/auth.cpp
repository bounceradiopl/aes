// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/auth.hpp"

#include <cstring>
#include <random>

namespace bam {

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t kPbkdf2Iterations = 100000;
constexpr int64_t kSessionLifetime = 12LL * 3600 * 1'000'000'000;

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

class Sha256 {
 public:
  void update(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      block_[block_len_++] = data[i];
      if (block_len_ == 64) {
        compress();
        block_len_ = 0;
      }
    }
    total_ += len;
  }

  std::array<uint8_t, 32> finish() {
    const uint64_t bits = total_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (block_len_ != 56) update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len_be, 8);
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 8; ++i)
      for (int b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<uint8_t>(h_[i] >> (24 - 8 * b));
    return out;
  }

 private:
  void compress() {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t(block_[i * 4]) << 24) | (uint32_t(block_[i * 4 + 1]) << 16) | (uint32_t(block_[i * 4 + 2]) << 8) |
             block_[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block_[64] = {};
  size_t block_len_ = 0;
  uint64_t total_ = 0;
};

std::vector<uint8_t> from_hex(std::string_view hex) {
  std::vector<uint8_t> out;
  if (hex.size() % 2) return out;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return {};
    out.push_back(static_cast<uint8_t>(hi * 16 + lo));
  }
  return out;
}

std::span<const uint8_t> as_bytes(std::string_view s) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
  Sha256 s;
  s.update(data.data(), data.size());
  return s.finish();
}

std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> msg) {
  uint8_t k[64] = {};
  if (key.size() > 64) {
    auto hk = sha256(key);
    std::memcpy(k, hk.data(), hk.size());
  } else {
    std::memcpy(k, key.data(), key.size());
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = k[i] ^ 0x36;
    opad[i] = k[i] ^ 0x5c;
  }
  Sha256 inner;
  inner.update(ipad, 64);
  inner.update(msg.data(), msg.size());
  auto ih = inner.finish();
  Sha256 outer;
  outer.update(opad, 64);
  outer.update(ih.data(), ih.size());
  return outer.finish();
}

std::vector<uint8_t> pbkdf2_sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations, size_t len) {
  std::vector<uint8_t> out;
  // HMAC uses a key longer than its 64-byte block as its SHA-256 (RFC 2104): done once here, not
  // in each of the 2 x iterations HMACs (a 1 MB password would otherwise cost 200 GB of hashing
  // per login attempt, before any session). The result is the same.
  std::span<const uint8_t> key = as_bytes(password);
  std::array<uint8_t, 32> hashed_key{};
  if (key.size() > 64) {
    hashed_key = sha256(key);
    key = hashed_key;
  }
  for (uint32_t block = 1; out.size() < len; ++block) {
    std::vector<uint8_t> msg(salt.begin(), salt.end());
    for (int i = 3; i >= 0; --i) msg.push_back(static_cast<uint8_t>(block >> (8 * i)));
    auto u = hmac_sha256(key, msg);
    auto t = u;
    for (uint32_t it = 1; it < iterations; ++it) {
      u = hmac_sha256(key, u);
      for (size_t i = 0; i < t.size(); ++i) t[i] ^= u[i];
    }
    for (size_t i = 0; i < t.size() && out.size() < len; ++i) out.push_back(t[i]);
  }
  return out;
}

std::string to_hex(std::span<const uint8_t> data) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (uint8_t b : data) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 15]);
  }
  return s;
}

std::string random_hex(size_t bytes) {
  static std::mutex m;
  static std::random_device rd;  // OS CSPRNG on Windows (rand_s) and POSIX (/dev/urandom)
  std::lock_guard lock(m);
  std::vector<uint8_t> buf(bytes);
  for (auto& b : buf) b = static_cast<uint8_t>(rd());
  return to_hex(buf);
}

std::string make_password_hash(std::string_view password) {
  const std::string salt_hex = random_hex(16);
  const auto salt = from_hex(salt_hex);
  const auto hash = pbkdf2_sha256(password, salt, kPbkdf2Iterations, 32);
  return "pbkdf2-sha256$" + std::to_string(kPbkdf2Iterations) + "$" + salt_hex + "$" + to_hex(hash);
}

bool verify_password(std::string_view password, std::string_view stored) {
  constexpr std::string_view prefix = "pbkdf2-sha256$";
  if (stored.substr(0, prefix.size()) != prefix) return false;
  stored.remove_prefix(prefix.size());
  const size_t a = stored.find('$');
  if (a == std::string_view::npos) return false;
  const size_t b = stored.find('$', a + 1);
  if (b == std::string_view::npos) return false;
  uint32_t iterations = 0;
  for (char c : stored.substr(0, a)) {
    if (c < '0' || c > '9') return false;
    iterations = iterations * 10 + static_cast<uint32_t>(c - '0');
    if (iterations > 10'000'000) return false;
  }
  if (iterations == 0) return false;
  const auto salt = from_hex(stored.substr(a + 1, b - a - 1));
  const auto expected = from_hex(stored.substr(b + 1));
  if (salt.empty() || expected.empty()) return false;
  const auto actual = pbkdf2_sha256(password, salt, iterations, expected.size());
  uint8_t diff = 0;
  for (size_t i = 0; i < expected.size(); ++i) diff |= static_cast<uint8_t>(actual[i] ^ expected[i]);
  return diff == 0;
}

std::string SessionStore::create(int64_t now_ns) {
  std::string token = random_hex(32);
  std::lock_guard lock(mutex_);
  for (auto it = expiry_.begin(); it != expiry_.end();) it = it->second < now_ns ? expiry_.erase(it) : std::next(it);
  expiry_[token] = now_ns + kSessionLifetime;
  return token;
}

bool SessionStore::valid(std::string_view token, int64_t now_ns) {
  std::lock_guard lock(mutex_);
  auto it = expiry_.find(token);
  if (it == expiry_.end()) return false;
  if (it->second < now_ns) {
    expiry_.erase(it);
    return false;
  }
  it->second = now_ns + kSessionLifetime;
  return true;
}

bool SessionStore::alive(std::string_view token, int64_t now_ns) {
  std::lock_guard lock(mutex_);
  auto it = expiry_.find(token);
  return it != expiry_.end() && it->second >= now_ns;
}

void SessionStore::remove(std::string_view token) {
  std::lock_guard lock(mutex_);
  auto it = expiry_.find(token);
  if (it != expiry_.end()) expiry_.erase(it);
}

}  // namespace bam
