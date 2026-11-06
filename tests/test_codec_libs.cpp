// SPDX-License-Identifier: GPL-3.0-or-later
// The codec libraries built from the pinned sources (cmake/codecs.cmake): LAME and fdk-aac are
// shared libraries loaded at run time (licences, spec §5.3), libFLAC is linked in.
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <FLAC/format.h>

#include <cstring>
#include <string>

#include "bam/platform/dynlib.hpp"

namespace bp = bam::platform;

TEST_CASE("codec libs: mp3lame loads from the build directory and exports the encoder and decoder") {
  std::string err;
  auto lib = bp::DynamicLibrary::open({BAM_MP3LAME_PATH}, &err);
  REQUIRE_MESSAGE(lib, err);
  CHECK(lib->symbol("lame_init") != nullptr);
  CHECK(lib->symbol("lame_encode_buffer_interleaved") != nullptr);
  CHECK(lib->symbol("hip_decode_init") != nullptr);  // mpglib, used by the encoder tests
  void* version = lib->symbol("get_lame_version");
  REQUIRE(version != nullptr);
  using VersionFn = const char* (*)();
  CHECK(std::string(reinterpret_cast<VersionFn>(version)()) == "3.100");
}

TEST_CASE("codec libs: fdk-aac loads from the build directory and exports the encoder and decoder") {
  std::string err;
  auto lib = bp::DynamicLibrary::open({BAM_FDKAAC_PATH}, &err);
  REQUIRE_MESSAGE(lib, err);
  CHECK(lib->symbol("aacEncOpen") != nullptr);
  CHECK(lib->symbol("aacEncEncode") != nullptr);
  CHECK(lib->symbol("aacDecoder_Open") != nullptr);
}

TEST_CASE("codec libs: libFLAC is linked in") {
  REQUIRE(FLAC__VERSION_STRING != nullptr);
  CHECK(std::strlen(FLAC__VERSION_STRING) > 0);
  CHECK(std::string(FLAC__VERSION_STRING) == "1.4.3");
}

#endif  // BAM_WITH_CODECS
