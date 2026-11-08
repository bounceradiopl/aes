// SPDX-License-Identifier: GPL-3.0-or-later
// Test side of the fake libStereoTool (fake_stereotool.cpp): its control functions, opened from
// the same library file the binding loads (so they share its state), and UseFake.
#pragma once
#ifdef BAM_FAKE_STEREOTOOL_PATH

#include <doctest/doctest.h>

#include <optional>
#include <string>

#include "bam/encoder/stereotool.hpp"
#include "bam/platform/dynlib.hpp"

namespace fake_st {

namespace be = bam::encoder;
namespace bp = bam::platform;

// The fake's control functions, from the same library file the binding loads (shared state).
struct Fake {
  bp::DynamicLibrary lib;
  void (*reset)() = nullptr;
  void (*set)(const char*, double) = nullptr;
  long long (*value)(const char*) = nullptr;
  int (*text_fn)(const char*, char*, int) = nullptr;
  int (*sts_fn)(const char*, int, char*, int) = nullptr;
  long long (*drop_live)() = nullptr;  // deletes the instances left live (the end of the process)

  std::optional<std::string> text(const char* what) const {
    char buf[1024];
    if (!text_fn(what, buf, sizeof buf)) return std::nullopt;
    return std::string(buf);
  }
  std::optional<std::string> sts(const char* st_name, int index) const {
    char buf[256];
    if (!sts_fn(st_name, index, buf, sizeof buf)) return std::nullopt;
    return std::string(buf);
  }
};

inline Fake& fake() {
  static Fake f = [] {
    Fake x;
    std::string err;
    auto lib = bp::DynamicLibrary::open({BAM_FAKE_STEREOTOOL_PATH}, &err);
    REQUIRE_MESSAGE(lib, err);
    x.lib = std::move(*lib);
    x.reset = reinterpret_cast<void (*)()>(x.lib.symbol("fake_stereotool_reset"));
    x.set = reinterpret_cast<void (*)(const char*, double)>(x.lib.symbol("fake_stereotool_set"));
    x.value = reinterpret_cast<long long (*)(const char*)>(x.lib.symbol("fake_stereotool_value"));
    x.text_fn = reinterpret_cast<int (*)(const char*, char*, int)>(x.lib.symbol("fake_stereotool_text"));
    x.sts_fn = reinterpret_cast<int (*)(const char*, int, char*, int)>(x.lib.symbol("fake_stereotool_sts"));
    x.drop_live = reinterpret_cast<long long (*)()>(x.lib.symbol("fake_stereotool_drop_live"));
    REQUIRE(x.reset);
    REQUIRE(x.set);
    REQUIRE(x.value);
    REQUIRE(x.text_fn);
    REQUIRE(x.sts_fn);
    REQUIRE(x.drop_live);
    return x;
  }();
  return f;
}

// The binding searches the fake's folder; the fake starts from its defaults. Afterwards the
// binding is back to the program folder.
struct UseFake {
  UseFake() {
    be::set_stereotool_library_dirs({BAM_FAKE_STEREOTOOL_DIR});
    fake().reset();
  }
  ~UseFake() {
    be::set_stereotool_library_dirs({});
    be::testing::set_stereotool_abandoned(0);
  }
  UseFake(const UseFake&) = delete;
  UseFake& operator=(const UseFake&) = delete;
};

}  // namespace fake_st

#endif  // BAM_FAKE_STEREOTOOL_PATH
