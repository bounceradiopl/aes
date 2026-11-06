// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "bam/platform/dynlib.hpp"

namespace bp = bam::platform;

namespace {
#ifdef _WIN32
const std::vector<std::string> kSystemLib = {"kernel32.dll"};
const char* const kKnownSymbol = "GetCurrentProcessId";
#elif defined(__APPLE__)
const std::vector<std::string> kSystemLib = {"libSystem.B.dylib", "/usr/lib/libSystem.B.dylib"};
const char* const kKnownSymbol = "getpid";
#else
const std::vector<std::string> kSystemLib = {"libc.so.6", "libc.so"};
const char* const kKnownSymbol = "getpid";
#endif
}  // namespace

TEST_CASE("dynlib: opens a system library and finds a known symbol") {
  std::string err;
  auto lib = bp::DynamicLibrary::open(kSystemLib, &err);
  REQUIRE_MESSAGE(lib, err);
  CHECK_FALSE(lib->path().empty());
  void* sym = lib->symbol(kKnownSymbol);
  CHECK(sym != nullptr);
  CHECK(lib->symbol("this_symbol_does_not_exist_xyz") == nullptr);
}

TEST_CASE("dynlib: missing library returns an error listing the candidates") {
  const std::vector<std::string> candidates = {"this_lib_does_not_exist_xyz.so", "neither_does_this_one_xyz.dll"};
  std::string err;
  auto lib = bp::DynamicLibrary::open(candidates, &err);
  CHECK_FALSE(lib);
  CHECK_FALSE(err.empty());
  for (const auto& c : candidates) CHECK(err.find(c) != std::string::npos);
}
