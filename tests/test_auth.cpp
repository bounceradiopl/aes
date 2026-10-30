// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

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
