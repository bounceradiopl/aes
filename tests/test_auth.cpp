// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "bam/auth.hpp"

namespace {
std::span<const uint8_t> bytes(const std::string& s) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
}  // namespace

TEST_CASE("SHA-256 vectors") {
  CHECK(bam::to_hex(bam::sha256(bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(bam::to_hex(bam::sha256(bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(bam::to_hex(bam::sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("HMAC-SHA256 RFC 4231 case 2") {
  CHECK(bam::to_hex(bam::hmac_sha256(bytes("Jefe"), bytes("what do ya want for nothing?"))) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("PBKDF2-HMAC-SHA256") {
  const std::string salt = "salt";
  CHECK(bam::to_hex(bam::pbkdf2_sha256("password", bytes(salt), 1, 32)) ==
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  CHECK(bam::to_hex(bam::pbkdf2_sha256("password", bytes(salt), 2, 32)) ==
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
}

namespace {
// n printable bytes, none of them repeating its neighbour: '!' + 7·i mod 94
std::string pattern_password(size_t n) {
  std::string s(n, ' ');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(33 + (i * 7) % 94);
  return s;
}
}  // namespace

TEST_CASE("PBKDF2 with passwords around and far past the 64-byte HMAC block: known values") {
  // hashlib.pbkdf2_hmac("sha256", pattern_password(n), b"BounceAES-sol", 10, 40).hex() (Python 3):
  // 40 bytes make two blocks; a key over 64 bytes is the SHA-256 of itself (RFC 2104)
  const std::string salt = "BounceAES-sol";
  const struct {
    size_t length;
    const char* hex;
  } vectors[] = {
      {0, "2c8a21f2b0139de1309db1faf6518f7be4a5792079e5bef57518ba432f17bd9e1b57294864454908"},
      {64, "1b7d0db079420bd4610d68f4850b7fb3eeb5726021372b172a70096cd809bfe10b847e8ce052a236"},
      {65, "15cc9518e5e7fbc7e67d07b62ac4be281ef5fd50370009bde04626a24b215244120a57de3b0ea39a"},
      {1000, "67fb0aed030a72f8d7ddd4a934320766a561df3a7cf742c11f80b3a6a77936d6e0da0c331b5fb85e"},
      {100'000, "0deef54553ebdc099427ec0e829136b934b6e81dd7dd75986b814d1e31b2ef751732b8cbca18082a"},
  };
  for (const auto& v : vectors) {
    CAPTURE(v.length);
    CHECK(bam::to_hex(bam::pbkdf2_sha256(pattern_password(v.length), bytes(salt), 10, 40)) == v.hex);
  }
  // a hash stored by an earlier version (the full 100 000 iterations, a 200-byte password) still
  // verifies: hashlib.pbkdf2_hmac("sha256", b"L" * 200, bytes.fromhex("0011…eeff"), 100000, 32)
  const std::string stored =
      "pbkdf2-sha256$100000$00112233445566778899aabbccddeeff$23f71a5dd743916e8c471be97fdac1896435779fa7ac632086573ace90dae3e5";
  CHECK(bam::verify_password(std::string(200, 'L'), stored));
  CHECK_FALSE(bam::verify_password(std::string(199, 'L'), stored));
}

TEST_CASE("PBKDF2: a very long password costs what a short one does") {
  // it used to hash the whole password in each of the 2 x 100 000 HMACs (minutes for 100 KB)
  const auto h = bam::make_password_hash("tajne-haslo");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(bam::verify_password(pattern_password(100'000), h));
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
  const std::string long_password = pattern_password(1000);
  CHECK(bam::verify_password(long_password, bam::make_password_hash(long_password)));
}

TEST_CASE("password hash and verify") {
  auto h = bam::make_password_hash("tajne-haslo");
  CHECK(h.rfind("pbkdf2-sha256$", 0) == 0);
  CHECK(bam::verify_password("tajne-haslo", h));
  CHECK_FALSE(bam::verify_password("zle", h));
  CHECK_FALSE(bam::verify_password("tajne-haslo", "garbage"));
  CHECK_FALSE(bam::verify_password("tajne-haslo", ""));
  CHECK(bam::make_password_hash("a") != bam::make_password_hash("a"));  // random salt
}

TEST_CASE("random hex") {
  CHECK(bam::random_hex(16).size() == 32);
  CHECK(bam::random_hex(16) != bam::random_hex(16));
}

TEST_CASE("sessions") {
  bam::SessionStore store;
  const int64_t h = 3600LL * 1'000'000'000;
  auto t = store.create(0);
  CHECK(store.valid(t, 1 * h));
  CHECK(store.valid(t, 12 * h));        // sliding: refreshed at 1 h, so valid until 13 h
  CHECK_FALSE(store.valid(t, 25 * h));  // 13 h after last use
  CHECK_FALSE(store.valid("nope", 0));
  auto t2 = store.create(0);
  store.remove(t2);
  CHECK_FALSE(store.valid(t2, 0));
}
