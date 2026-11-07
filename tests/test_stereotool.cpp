// SPDX-License-Identifier: GPL-3.0-or-later
// The Stereo Tool binding (bam/encoder/stereotool.hpp) against the fake library of
// tests/fake_stereotool (gain + delay, counters, failure modes). The real libStereoTool is used
// only when BAM_TEST_REAL_STEREOTOOL names a folder holding it (no licence key, Stereo Tool's defaults).
#include <doctest/doctest.h>

#ifdef BAM_FAKE_STEREOTOOL_PATH

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../encoder/src/stereotool_api.hpp"  // the parameter indices the binding uses (11.07)
#include "bam/encoder/preset_store.hpp"
#include "bam/encoder/stereotool.hpp"
#include "bam/platform/dynlib.hpp"
#include "bam/platform/process.hpp"
#include "fake_stereotool/fake_control.hpp"
#include "test_util.hpp"

namespace be = bam::encoder;
namespace bp = bam::platform;
namespace fs = std::filesystem;

namespace {

using fake_st::fake;
using fake_st::UseFake;
namespace st = be::detail::st;
constexpr int kWebserverEnable = st::kParamWebserverEnable;
constexpr int kWebserverPort = st::kParamWebserverPort;
constexpr long long kSaveAllSettings = st::kSaveAllSettings;
const std::string kUnlicensed = "Fake: Declipper, Fake: Multiband";

struct TempDir {
  std::string path;
  explicit TempDir(const std::string& tag) {
    path = (fs::temp_directory_path() / ("bam_st_" + tag + "_" + std::to_string(bp::current_pid()))).string();
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

struct CwdGuard {
  fs::path saved = fs::current_path();
  explicit CwdGuard(const fs::path& to) { fs::current_path(to); }
  ~CwdGuard() {
    std::error_code ec;
    fs::current_path(saved, ec);
  }
  CwdGuard(const CwdGuard&) = delete;
  CwdGuard& operator=(const CwdGuard&) = delete;
};

bool same_file(const std::string& a, const std::string& b) {
  std::error_code ec;
  return fs::equivalent(a, b, ec);
}

// Interleaved stereo, every frame different on both channels.
std::vector<float> ramp(size_t frames) {
  std::vector<float> v(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    v[2 * i] = float(i + 1) / 1024.0f;
    v[2 * i + 1] = -float(i + 1) / 2048.0f;
  }
  return v;
}

std::unique_ptr<be::StereoToolInstance> make(const char* name, const std::string& key = "",
                                             const std::string& home = "") {
  std::string err;
  auto st = be::StereoToolInstance::create(key, name, home, &err);
  REQUIRE_MESSAGE(st, err);
  return st;
}

}  // namespace

TEST_CASE("stereotool: the library is loaded from the configured folder only, never by its bare name") {
  UseFake use;
  const std::string name = fs::path(BAM_FAKE_STEREOTOOL_PATH).filename().string();
  const auto names = be::stereotool_library_names();
  REQUIRE_FALSE(names.empty());
  CHECK(names.front() == name);  // the fake carries the real library's file name

  {
    // the working directory holds the library: a search by bare name would find it there
    CwdGuard cwd(BAM_FAKE_STEREOTOOL_DIR);
    TempDir empty("empty");
    be::set_stereotool_library_dirs({empty.path});
    const auto info = be::stereotool_info();
    CHECK_FALSE(info.available);
    CHECK(info.path.empty());
    CHECK(info.version == 0);
    CHECK_MESSAGE(info.error.find("brak biblioteki Stereo Tool") != std::string::npos, info.error);
    CHECK_MESSAGE(info.error.find(name) != std::string::npos, info.error);
    CHECK_MESSAGE(info.error.find(empty.path) != std::string::npos, info.error);
    std::string err;
    CHECK(be::StereoToolInstance::create("", "station-x", "", &err) == nullptr);
    CHECK(err == info.error);
  }

  // the configured folder: loaded from there, by its full path
  be::set_stereotool_library_dirs({BAM_FAKE_STEREOTOOL_DIR});
  const auto info = be::stereotool_info();
  REQUIRE_MESSAGE(info.available, info.error);
  CHECK(info.error.empty());
  CHECK(fs::path(info.path).is_absolute());
  CHECK_MESSAGE(same_file(info.path, BAM_FAKE_STEREOTOOL_PATH), info.path);
  CHECK(info.version == 11070);
}

TEST_CASE("stereotool: by default the library is searched in the program folder") {
  be::set_stereotool_library_dirs({});
  const auto info = be::stereotool_info();
  const std::string exe_dir = bp::executable_dir();
  REQUIRE_FALSE(exe_dir.empty());
  if (info.available) {
    // a build that bundles the real library (Windows x64) puts it next to the executables
    CHECK_MESSAGE(same_file(fs::path(info.path).parent_path().string(), exe_dir), info.path);
  } else {
    CHECK_MESSAGE(info.error.find("brak biblioteki Stereo Tool") != std::string::npos, info.error);
    CHECK_MESSAGE(info.error.find(exe_dir) != std::string::npos, info.error);
  }
}

TEST_CASE("stereotool: web interface and UECP are bound to 127.0.0.1 before the first instance") {
  UseFake use;
  auto st = make("station-bind");
  CHECK(fake().value("bind_calls") == 1);
  CHECK(fake().value("bind_create_calls") == 0);  // no Create3 before the bind
  CHECK(fake().text("bind_web") == std::optional<std::string>("127.0.0.1"));
  CHECK(fake().text("bind_uecp") == std::optional<std::string>("127.0.0.1"));
  auto second = make("station-bind-2");
  CHECK(fake().value("bind_calls") == 1);  // once per loaded library
}

TEST_CASE("stereotool: instances are created with the key, name and home folder, and deleted") {
  UseFake use;
  const long long live0 = fake().value("live");
  TempDir home("home");
  {
    auto a = make("station-1", "KEY-A", home.path);
    CHECK(fake().text("create_key") == std::optional<std::string>("KEY-A"));
    CHECK(fake().text("create_name") == std::optional<std::string>("station-1"));
    CHECK(fake().text("create_home") == std::optional<std::string>(home.path));
    CHECK(fake().value("create_has_gui") == 0);
    CHECK(fake().value("create_load_ini") == 0);
    // empty key and home folder: NULL (no licence, the library's own home)
    auto b = make("station-2");
    CHECK_FALSE(fake().text("create_key"));
    CHECK_FALSE(fake().text("create_home"));
    CHECK(fake().text("create_name") == std::optional<std::string>("station-2"));
    CHECK(fake().value("created") == 2);
    CHECK(fake().value("live") == live0 + 2);
    CHECK(fake().value("deleted") == 0);
    a.reset();
    CHECK(fake().value("deleted") == 1);
    CHECK(fake().value("live") == live0 + 1);
  }
  CHECK(fake().value("deleted") == 2);
  CHECK(fake().value("live") == live0);
  CHECK(fake().value("delete_unknown") == 0);
}

TEST_CASE("stereotool: a failed Create3 is reported in Polish, without the key") {
  UseFake use;
  fake().set("create_fails", 1);
  std::string err;
  CHECK(be::StereoToolInstance::create("SECRET-KEY-123", "station-fail", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("nie udało się utworzyć instancji Stereo Tool") != std::string::npos, err);
  CHECK(err.find("SECRET-KEY-123") == std::string::npos);
  CHECK(fake().value("created") == 0);
}

TEST_CASE("stereotool: process applies the library in place at 48 kHz stereo, bit-exact") {
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 3);
  auto st = make("station-exact");
  const auto in = ramp(520);
  auto out = in;
  size_t done = 0;
  for (int32_t block : {7, 1, 480, 32}) {
    CHECK(st->process(out.data() + 2 * done, block));
    done += size_t(block);
  }
  REQUIRE(done == 520);
  size_t mismatches = 0;
  for (size_t k = 0; k < out.size(); ++k) {
    const float expected = k >= 6 ? in[k - 6] * 0.5f : 0.0f;
    if (out[k] != expected) ++mismatches;
  }
  CHECK(mismatches == 0);
  CHECK(fake().value("process_calls") == 4);
  CHECK(fake().value("process_frames") == 520);
  CHECK(fake().value("process_channels") == 2);
  CHECK(fake().value("process_rate") == 48000);
  CHECK_FALSE(st->failed());
  CHECK(st->failure().empty());
}

TEST_CASE("stereotool: latency is GetLatency2 at 48 kHz with silence fed") {
  UseFake use;
  fake().set("latency", 512);
  auto st = make("station-latency");
  CHECK(st->latency_frames() == 512);
  CHECK(fake().value("latency_rate") == 48000);
  CHECK(fake().value("latency_feed_silence") == 1);
}

TEST_CASE("stereotool: presets are saved and loaded with all settings, failures in Polish") {
  UseFake use;
  TempDir dir("presets");
  const std::string path = (fs::path(dir.path) / "a.sts").string();
  fake().set("gain", 0.25);
  fake().set("latency", 100);
  auto a = make("station-a");
  std::string err;
  REQUIRE_MESSAGE(a->save_preset(path, &err), err);
  CHECK(fake().value("save_type") == kSaveAllSettings);
  CHECK(fake().text("save_path") == std::optional<std::string>(path));
  CHECK(read_file(path).rfind("[FakeStereoTool]", 0) == 0);

  fake().set("gain", 1);
  fake().set("latency", 0);
  auto b = make("station-b");
  CHECK(b->latency_frames() == 0);
  REQUIRE_MESSAGE(b->load_preset(path, &err), err);
  CHECK(fake().value("load_type") == kSaveAllSettings);
  CHECK(fake().text("load_path") == std::optional<std::string>(path));
  CHECK(b->latency_frames() == 100);
  const auto in = ramp(200);
  auto out = in;
  REQUIRE(b->process(out.data(), 200));
  CHECK(out[0] == 0.0f);
  CHECK(out[2 * 100] == in[0] * 0.25f);
  CHECK(out[2 * 199 + 1] == in[2 * 99 + 1] * 0.25f);

  const std::string missing = (fs::path(dir.path) / "missing.sts").string();
  err.clear();
  CHECK_FALSE(b->load_preset(missing, &err));
  CHECK_MESSAGE(err.find("nie wczytał presetu") != std::string::npos, err);
  CHECK_MESSAGE(err.find(missing) != std::string::npos, err);
  CHECK_FALSE(b->failed());  // a refused preset is not a failed instance

  fake().set("save_fails", 1);
  err.clear();
  CHECK_FALSE(a->save_preset(path, &err));
  CHECK_MESSAGE(err.find("nie zapisał presetu") != std::string::npos, err);
  CHECK_FALSE(a->failed());
}

TEST_CASE("stereotool: licence validity and the unlicensed features") {
  UseFake use;
  auto st = make("station-licence", "KEY-L");
  CHECK(st->license_valid());
  CHECK(st->unlicensed_features().empty());
  fake().set("license_valid", 0);
  CHECK_FALSE(st->license_valid());
  CHECK(st->unlicensed_features() == kUnlicensed);
  CHECK_FALSE(st->failed());
}

TEST_CASE("stereotool: a C++ exception in process is caught, the instance is failed and then deleted") {
  UseFake use;
  fake().set("latency", 0);
  fake().set("gain", 2);
  auto st = make("station-throw");
  fake().set("process_throws", 1);
  const auto in = ramp(480);
  auto buf = in;
  CHECK_FALSE(st->process(buf.data(), 480));
  CHECK(fake().value("process_calls") == 1);
  CHECK(st->failed());
  CHECK_MESSAGE(st->failure().find("wyjątek") != std::string::npos, st->failure());
  CHECK_MESSAGE(st->failure().find("stereoTool_Process") != std::string::npos, st->failure());
  CHECK_MESSAGE(st->failure().find("fake Stereo Tool process failure") != std::string::npos, st->failure());

  // never called again, even when the library would work now
  fake().set("process_throws", 0);
  buf = in;
  CHECK_FALSE(st->process(buf.data(), 480));
  CHECK(buf == in);
  CHECK(fake().value("process_calls") == 1);
  CHECK(st->latency_frames() == 0);
  CHECK_FALSE(st->license_valid());
  std::string err;
  CHECK_FALSE(st->load_preset("whatever.sts", &err));
  CHECK_FALSE(err.empty());
  CHECK(fake().value("latency_calls") == 0);
  CHECK(fake().value("license_calls") == 0);
  CHECK(fake().value("load_calls") == 0);
  // a C++ exception unwinds in order: the instance is deleted (its ports and memory freed), not abandoned
  CHECK(be::stereotool_info().abandoned == 0);
  st.reset();
  CHECK(fake().value("deleted") == 1);
  CHECK(fake().value("delete_unknown") == 0);
  CHECK(be::stereotool_info().abandoned == 0);
  auto next = make("station-throw-2");  // and the next instance is allowed
}

TEST_CASE("stereotool: an exception from Create3 is reported without the licence key") {
  UseFake use;
  fake().set("create_throws", 1);  // the fake's exception text contains the key
  std::string err;
  CHECK(be::StereoToolInstance::create("SECRET-KEY-456", "station-ct", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("nie udało się utworzyć instancji Stereo Tool") != std::string::npos, err);
  CHECK_MESSAGE(err.find("wyjątek") != std::string::npos, err);
  CHECK_MESSAGE(err.find("fake Create3 failure") != std::string::npos, err);
  CHECK_MESSAGE(err.find("SECRET-KEY-456") == std::string::npos, err);
  CHECK(fake().value("create_calls") == 1);
  CHECK(fake().value("created") == 0);
}

TEST_CASE("stereotool: the search candidates are absolute paths, a relative folder is never searched") {
  UseFake use;
  const auto names = be::stereotool_library_names();
  be::set_stereotool_library_dirs({BAM_FAKE_STEREOTOOL_DIR});
  auto candidates = be::stereotool_library_candidates();
  REQUIRE(candidates.size() == names.size());
  for (const auto& c : candidates) CHECK_MESSAGE(fs::path(c).is_absolute(), c);
  CHECK(same_file(candidates.front(), BAM_FAKE_STEREOTOOL_PATH));
  // the default: the program folder, absolute as well
  be::set_stereotool_library_dirs({});
  candidates = be::stereotool_library_candidates();
  REQUIRE_FALSE(candidates.empty());
  for (const auto& c : candidates) CHECK_MESSAGE(fs::path(c).is_absolute(), c);
  // a relative folder (resolved against the working directory) is left out: nothing to search
  CwdGuard cwd(fs::path(BAM_FAKE_STEREOTOOL_DIR).parent_path());
  const std::string relative = fs::path(BAM_FAKE_STEREOTOOL_DIR).filename().string();
  be::set_stereotool_library_dirs({relative});
  CHECK(be::stereotool_library_candidates().empty());
  const auto info = be::stereotool_info();
  CHECK_FALSE(info.available);
  CHECK_MESSAGE(info.error.find("brak biblioteki Stereo Tool") != std::string::npos, info.error);
}

TEST_CASE("stereotool: after an abandoned instance no further instance is created") {
  UseFake use;
  CHECK(be::stereotool_info().abandoned == 0);
  be::testing::set_stereotool_abandoned(1);  // what a crash in the library leaves behind (MSVC test below)
  CHECK(be::stereotool_info().abandoned == 1);
  CHECK(be::stereotool_info().available);  // the library itself is still there
  std::string err;
  CHECK(be::StereoToolInstance::create("", "station-after-crash", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("Stereo Tool uległ awarii") != std::string::npos, err);
  CHECK_MESSAGE(err.find("uruchom ponownie usługę Bounce Encoder") != std::string::npos, err);
  CHECK(fake().value("create_calls") == 0);  // refused before the library is asked
  // no reload either: the abandoned instance still lives in this library
  CHECK(be::stereotool_reload().abandoned == 1);
  be::testing::set_stereotool_abandoned(0);
  auto st = make("station-after-restart");
}

TEST_CASE("stereotool: a create waiting for another one re-checks the abandoned cap before Create3") {
  UseFake use;
  fake().set("create_sleep_us", 300000);  // the first Create3 holds the API mutex for 0.3 s
  std::unique_ptr<be::StereoToolInstance> a, b;
  std::string err_a, err_b;
  std::thread first([&] { a = be::StereoToolInstance::create("", "station-race-a", "", &err_a); });
  for (int i = 0; i < 400 && fake().value("create_calls") < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(fake().value("create_calls") == 1);
  // the second create passes the early check (nothing abandoned yet) and waits for the mutex
  std::thread second([&] { b = be::StereoToolInstance::create("", "station-race-b", "", &err_b); });
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  be::testing::set_stereotool_abandoned(1);  // meanwhile another station's instance crashed
  first.join();
  second.join();
  CHECK_MESSAGE(a, err_a);  // already inside Create3 when the crash happened
  CHECK(b == nullptr);
  CHECK_MESSAGE(err_b.find("Stereo Tool uległ awarii") != std::string::npos, err_b);
  CHECK(fake().value("create_calls") == 1);  // the waiting create never reached the library
  a.reset();
}

TEST_CASE("stereotool: a library that failed to load after a crash says the service needs a restart") {
  UseFake use;
  be::testing::set_stereotool_abandoned(1);  // e.g. a crash while it was loaded
  fake().set("version", 11080);              // and it is not available
  std::string err;
  CHECK(be::StereoToolInstance::create("", "station-hint", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("11080") != std::string::npos, err);
  CHECK_MESSAGE(err.find("uruchom ponownie usługę Bounce Encoder") != std::string::npos, err);
  be::testing::set_stereotool_abandoned(0);
  CHECK(be::StereoToolInstance::create("", "station-hint", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("uruchom ponownie") == std::string::npos, err);  // no crash: no such hint
}

#ifdef _MSC_VER
TEST_CASE("stereotool: a crash while loading the library counts as an abandoned instance") {
  for (const char* option : {"version_crashes", "bind_crashes"}) {
    CAPTURE(option);
    UseFake use;
    fake().set(option, 1);
    auto info = be::stereotool_info();
    CHECK_FALSE(info.available);
    CHECK_MESSAGE(info.error.find("wyjątek systemowy 0xC0000005") != std::string::npos, info.error);
    CHECK(info.abandoned == 1);
    // no reload into a library that just crashed
    fake().set(option, 0);
    const long long versions = fake().value("version_calls");
    info = be::stereotool_reload();
    CHECK_FALSE(info.available);
    CHECK(fake().value("version_calls") == versions);
    std::string err;
    CHECK(be::StereoToolInstance::create("", "station-load-crash", "", &err) == nullptr);
    CHECK_MESSAGE(err.find("uruchom ponownie usługę Bounce Encoder") != std::string::npos, err);
    CHECK(fake().value("create_calls") == 0);
  }
}
#endif

TEST_CASE("stereotool: only version 11.07 is used (the parameter indices are those of 11.07)") {
  UseFake use;
  fake().set("version", 11080);
  auto info = be::stereotool_info();
  CHECK_FALSE(info.available);
  CHECK(info.version == 11080);
  CHECK_MESSAGE(info.error.find("11080") != std::string::npos, info.error);
  CHECK_MESSAGE(info.error.find("11070") != std::string::npos, info.error);
  std::string err;
  CHECK(be::StereoToolInstance::create("", "station-ver", "", &err) == nullptr);
  CHECK(err == info.error);
  CHECK(fake().value("create_calls") == 0);
  CHECK(fake().value("bind_calls") == 0);  // nothing but the version asked of a wrong library

  // a failed load stays cached until a reload; here the same (fake) file answers 11070 now. A real
  // DLL of another version stays loaded in the process, so it needs a service restart instead
  fake().set("version", 11070);
  CHECK_FALSE(be::stereotool_info().available);
  info = be::stereotool_reload();
  REQUIRE_MESSAGE(info.available, info.error);
  CHECK(info.version == 11070);
  CHECK(be::stereotool_info().available);
  CHECK(fake().value("bind_calls") == 1);
  auto st = make("station-ver-ok");
  // a reload of a library that is loaded changes nothing
  CHECK(be::stereotool_reload().available);
  CHECK(fake().value("bind_calls") == 1);
}

#ifdef _MSC_VER
// Only MSVC builds catch structured exceptions (__try/__except); a crash inside the library
// elsewhere ends the process, so there is nothing to test there.
TEST_CASE("stereotool: an access violation in process is caught, the instance is abandoned and the next refused") {
  UseFake use;
  auto st = make("station-crash");
  fake().set("process_crashes", 1);
  auto buf = ramp(480);
  CHECK_FALSE(st->process(buf.data(), 480));
  CHECK(st->failed());
  CHECK_MESSAGE(st->failure().find("wyjątek systemowy 0xC0000005") != std::string::npos, st->failure());
  CHECK(be::stereotool_info().abandoned == 1);
  fake().set("process_crashes", 0);
  CHECK_FALSE(st->process(buf.data(), 480));
  CHECK(fake().value("process_calls") == 1);
  CHECK(st->latency_frames() == 0);
  CHECK(fake().value("latency_calls") == 0);
  st.reset();
  CHECK(fake().value("deleted") == 0);  // abandoned: its state is unknown
  CHECK(be::stereotool_info().abandoned == 1);
  std::string err;
  CHECK(be::StereoToolInstance::create("", "station-crash-2", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("uruchom ponownie usługę Bounce Encoder") != std::string::npos, err);
}
#endif

TEST_CASE("stereotool: the web interface port is set per instance and kept across preset loads") {
  UseFake use;
  auto st = make("station-web");
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("0"));
  REQUIRE(st->set_web_interface(true, 8105));
  CHECK(fake().sts("station-web", kWebserverPort) == std::optional<std::string>("8105"));
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("1"));
  // a port out of range changes nothing
  CHECK_FALSE(st->set_web_interface(true, 0));
  CHECK_FALSE(st->set_web_interface(true, 65536));
  CHECK(fake().sts("station-web", kWebserverPort) == std::optional<std::string>("8105"));
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("1"));

  // a preset may carry web interface settings of its own: ours are applied again
  TempDir dir("web");
  const std::string path = (fs::path(dir.path) / "web.sts").string();
  std::string err;
  REQUIRE_MESSAGE(st->save_preset(path, &err), err);
  REQUIRE_MESSAGE(st->load_preset(path, &err), err);
  CHECK(fake().sts("station-web", kWebserverPort) == std::optional<std::string>("8105"));
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("1"));

  REQUIRE(st->set_web_interface(false, 0));
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("0"));
  // a port the library refuses: the web interface is not switched on (it would use another port)
  fake().set("sts_rejects", kWebserverPort);
  CHECK_FALSE(st->set_web_interface(true, 8106));
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("0"));
  fake().set("sts_rejects", -1);
  REQUIRE_MESSAGE(st->load_preset(path, &err), err);
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("0"));
  // still only on 127.0.0.1
  CHECK(fake().text("bind_web") == std::optional<std::string>("127.0.0.1"));
}

namespace {

// Times of a call into the real library, in ms.
struct CallTimes {
  std::vector<double> ms;
  template <typename F>
  auto time(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = f();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return r;
  }
  double at(double q) const {  // quantile q (0..1) of the times
    auto v = ms;
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
  }
  std::string text() const {
    char buf[160];
    std::snprintf(buf, sizeof buf, "n %llu, median %.3f ms, p99 %.3f ms, max %.3f ms", (unsigned long long)ms.size(), at(0.5),
                  at(0.99), at(1.0));
    return buf;
  }
};

}  // namespace

// Windows CI runs this against the DLL next to the encoder (BAM_TEST_REAL_STEREOTOOL = that folder)
// in a run of its own: never with a licence key, Stereo Tool's default settings, no web interface
// (no socket). It checks that a preset saved by the library under the preset store's temporary
// name loads again, and prints the times the processing worker's rules depend on: a Process call
// (the worker bypasses when a call is out for more than 200 ms), the licence check (every 250 ms
// of audio, on the same thread as Process), SavePreset/LoadPreset (under the API mutex, like
// Create3 and Delete).
TEST_CASE("stereotool: the real library (BAM_TEST_REAL_STEREOTOOL) saves and loads a preset and processes audio without a licence key") {
  const char* dir = std::getenv("BAM_TEST_REAL_STEREOTOOL");
  if (!dir || !*dir) {
    MESSAGE("BAM_TEST_REAL_STEREOTOOL not set, real Stereo Tool test skipped");
    return;
  }
  struct Restore {
    ~Restore() { be::set_stereotool_library_dirs({}); }
  } restore;
  be::set_stereotool_library_dirs({dir});
  const auto info = be::stereotool_info();
  REQUIRE_MESSAGE(info.available, info.error);
  CHECK(info.version == be::kStereoToolVersion);
  TempDir home("real");
  CallTimes create_ms;
  auto st = create_ms.time([&] { return make("bam-test", "", home.path); });  // no licence key, ever
  const int latency = st->latency_frames();
  CHECK(latency >= 0);

  std::vector<float> buf(480 * 2);
  bool finite = true;
  CallTimes process_ms, licence_ms, save_ms, load_ms;
  auto process_silence = [&](int blocks) {
    for (int block = 0; block < blocks; ++block) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      REQUIRE_MESSAGE(process_ms.time([&] { return st->process(buf.data(), 480); }), st->failure());
      for (float x : buf)
        if (!std::isfinite(x)) finite = false;
    }
  };
  process_silence(50);  // some audio first: the licence check needs it
  const double first_process_ms = process_ms.ms.front();

  // a preset saved by the library itself, under the name the preset store gives its temporary
  // files (PresetStore: ".bounce-<n>.tmp" in the presets folder, renamed to the preset's name afterwards)
  const std::string tmp = (fs::path(home.path) / ".bounce-1.tmp").string();
  std::string err;
  for (int i = 0; i < 3; ++i) REQUIRE_MESSAGE(save_ms.time([&] { return st->save_preset(tmp, &err); }), err);
  std::error_code ec;
  REQUIRE(fs::is_regular_file(tmp, ec));  // that very name: no extension added
  const auto preset_bytes = fs::file_size(tmp, ec);
  CHECK(preset_bytes > 0);
  CHECK(preset_bytes <= be::kMaxPresetBytes);
  CHECK_FALSE(fs::exists(tmp + ".sts", ec));
  const std::string preset = (fs::path(home.path) / "real.sts").string();
  fs::rename(tmp, preset, ec);
  REQUIRE_FALSE(ec);
  REQUIRE_MESSAGE(load_ms.time([&] { return st->load_preset(preset, &err); }), err);

  // 1000 blocks (10 s of audio), the licence checked every 25 blocks (250 ms) as the worker does
  for (int block = 0; block < 1000; block += 25) {
    process_silence(25);
    const bool valid = licence_ms.time([&] { return st->license_valid(); });
    CHECK(valid);  // no key, default settings: nothing unlicensed
  }
  CHECK(finite);
  // silence may take a shortcut inside Stereo Tool: 10 s of noise at about -20 dBFS as well, for
  // a closer Process time (the settings are Stereo Tool's defaults, not a station's preset)
  CallTimes noise_ms;
  uint32_t rng = 12345;
  for (int block = 0; block < 1000; ++block) {
    for (float& x : buf) {
      rng = rng * 1664525u + 1013904223u;
      x = (float(rng >> 8) / float(1u << 24) - 0.5f) * 0.2f;
    }
    REQUIRE_MESSAGE(noise_ms.time([&] { return st->process(buf.data(), 480); }), st->failure());
    for (float x : buf)
      if (!std::isfinite(x)) finite = false;
    if (block % 25 == 24) CHECK(licence_ms.time([&] { return st->license_valid(); }));
  }
  CHECK(finite);
  CHECK_FALSE(st->failed());
  MESSAGE("real Stereo Tool " << info.version << ", latency " << latency << " frames, preset " << preset_bytes << " bytes");
  MESSAGE("real Stereo Tool timing: Create3 " << create_ms.text() << "; first Process(480) " << first_process_ms << " ms");
  MESSAGE("real Stereo Tool timing: Process(480) of silence " << process_ms.text());
  MESSAGE("real Stereo Tool timing: Process(480) of noise " << noise_ms.text());
  MESSAGE("real Stereo Tool timing: CheckLicenseValid " << licence_ms.text());
  MESSAGE("real Stereo Tool timing: SavePreset " << save_ms.text() << "; LoadPreset " << load_ms.text());
  // the worker's 200 ms rule counts a Process call and the licence check after it: report a
  // machine where they come near it (a warning, the CI runner is not the target computer)
  const double worst = std::max(process_ms.at(1.0), noise_ms.at(1.0)) + licence_ms.at(1.0);
  WARN_MESSAGE(worst < 100.0, "Process + CheckLicenseValid take up to " << worst << " ms");
  CallTimes delete_ms;
  delete_ms.time([&] {
    st.reset();
    return true;
  });
  MESSAGE("real Stereo Tool timing: Delete " << delete_ms.text());
}

#endif  // BAM_FAKE_STEREOTOOL_PATH
