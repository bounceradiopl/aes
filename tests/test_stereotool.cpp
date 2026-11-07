// SPDX-License-Identifier: GPL-3.0-or-later
// The Stereo Tool binding (bam/encoder/stereotool.hpp) against the fake library of
// tests/fake_stereotool (gain + delay, counters, failure modes). The real libStereoTool is used
// only when BAM_TEST_REAL_STEREOTOOL names a folder holding it (no licence key, Stereo Tool's defaults).
#include <doctest/doctest.h>

#ifdef BAM_FAKE_STEREOTOOL_PATH

#include <httplib.h>  // first: on Windows it brings winsock2.h before windows.h (iphlpapi.h needs both)

#ifdef _WIN32
#include <iphlpapi.h>  // GetExtendedTcpTable / GetExtendedUdpTable
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../encoder/src/stereotool_api.hpp"  // the parameter indices the binding uses (11.07)
#include "bam/encoder/preset_store.hpp"
#include "bam/encoder/stereotool.hpp"
#include "bam/platform/dynlib.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/tcp.hpp"
#include "bam/platform/time.hpp"
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
  fake().set("create_throws", 1);  // the fake's exception text contains the key, and its inner text
  std::string err;
  CHECK(be::StereoToolInstance::create("<SECRET-KEY-456>", "station-ct", "", &err) == nullptr);
  CHECK_MESSAGE(err.find("nie udało się utworzyć instancji Stereo Tool") != std::string::npos, err);
  CHECK_MESSAGE(err.find("wyjątek") != std::string::npos, err);
  CHECK_MESSAGE(err.find("fake Create3 failure") != std::string::npos, err);
  CHECK_MESSAGE(err.find("SECRET-KEY-456") == std::string::npos, err);  // neither "<SECRET-KEY-456>" nor without the brackets
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

constexpr int kPassword0 = st::kParamPassword0;
constexpr int kProtectionAny = st::kParamPasswordProtectionAny;
constexpr int kJsonToken = st::kParamJsonToken;

// Every network switch of stereotool_api.hpp, one by one.
std::vector<std::pair<int, std::string>> network_switches() {
  std::vector<std::pair<int, std::string>> out;
  for (const auto& sw : st::kNetworkSwitches)
    for (int i = 0; i < sw.count; ++i) out.emplace_back(sw.first + i, sw.what);
  return out;
}

// A fake preset that switches every network feature on, the web interface on port 9999 too, and
// sets Stereo Tool's password protection off (what a .sts from elsewhere might hold).
std::string network_preset(const std::string& dir) {
  const std::string path = (fs::path(dir) / "network.sts").string();
  std::string text = "[FakeStereoTool]\ngain=0.5\nlatency=8\n";
  for (const auto& [index, what] : network_switches()) text += "sts." + std::to_string(index) + "=1\n";
  text += "sts." + std::to_string(kWebserverEnable) + "=1\nsts." + std::to_string(kWebserverPort) + "=9999\n";
  text += "sts." + std::to_string(kProtectionAny) + "=0\nsts." + std::to_string(kPassword0) + "=\n";
  write_file(path, text);
  return path;
}

}  // namespace

TEST_CASE("stereotool: every network feature but the web interface is switched off, at create and after every preset load") {
  UseFake use;
  TempDir dir("network");
  auto st = make("station-net");
  for (const auto& [index, what] : network_switches())
    CHECK_MESSAGE(fake().sts("station-net", index) == std::optional<std::string>("0"), index << " " << what);
  CHECK(fake().sts("station-net", kWebserverEnable) == std::optional<std::string>("0"));  // no web port given: off

  // a preset that switches them all on (and the web interface, on a port of its own)
  const std::string preset = network_preset(dir.path);
  std::string err;
  REQUIRE_MESSAGE(st->load_preset(preset, &err), err);
  for (const auto& [index, what] : network_switches())
    CHECK_MESSAGE(fake().sts("station-net", index) == std::optional<std::string>("0"), index << " " << what);
  CHECK(fake().sts("station-net", kWebserverEnable) == std::optional<std::string>("0"));
  REQUIRE(st->set_web_interface(true, 8111));
  REQUIRE_MESSAGE(st->load_preset(preset, &err), err);
  for (const auto& [index, what] : network_switches())
    CHECK_MESSAGE(fake().sts("station-net", index) == std::optional<std::string>("0"), index << " " << what);
  CHECK(fake().sts("station-net", kWebserverPort) == std::optional<std::string>("8111"));
  CHECK(fake().sts("station-net", kWebserverEnable) == std::optional<std::string>("1"));
  CHECK_FALSE(st->failed());

  // a switch the library does not have (the plugin of a platform without the feature) cannot be on
  fake().set("sts_unknown", 5066);
  REQUIRE_MESSAGE(st->load_preset(preset, &err), err);
  const auto absent = fake().sts("station-net", 5066);
  CHECK((!absent || *absent == "0"));
  CHECK_FALSE(st->failed());
  fake().set("sts_unknown", -1);

  // one the library keeps on fails the instance: no processing with a stream to who knows where
  fake().set("sts_rejects", 3838);  // PARAM_UECP_Enable
  err.clear();
  CHECK_FALSE(st->load_preset(preset, &err));
  CHECK_MESSAGE(err.find("UECP") != std::string::npos, err);
  CHECK_MESSAGE(err.find("nie wyłączył") != std::string::npos, err);
  CHECK(st->failed());
  st.reset();
  CHECK(fake().value("deleted") == 1);  // deleted (in order), not abandoned
  CHECK(be::stereotool_info().abandoned == 0);
  // at create the same: no instance
  fake().set("sts_rejects", 6344);  // PARAM_UECP_Enable_1 ...
  std::string create_err;
  auto again = be::StereoToolInstance::create("", "station-net-2", "", &create_err);
  CHECK(again);  // ... is not on in a new instance (the library's defaults): nothing to refuse
  fake().set("sts_rejects", -1);
}

TEST_CASE("stereotool: the web interface asks for a random password per instance, never kept in a saved preset") {
  UseFake use;
  TempDir dir("password");
  const std::string key = "<FAKEKEY0123456789abcdef>";
  auto st = make("station-pw", key);
  REQUIRE(st->set_web_interface(true, 8107));
  const auto pw = fake().sts("station-pw", kPassword0);
  REQUIRE(pw.has_value());
  CHECK(pw->size() >= 16);
  CHECK(fake().sts("station-pw", kProtectionAny) == std::optional<std::string>("1"));
  CHECK(fake().sts("station-pw", st::kParamPasswordProtection0) == std::optional<std::string>("1"));
  const auto token = fake().sts("station-pw", kJsonToken);
  REQUIRE(token.has_value());
  CHECK(token->size() >= 16);
  CHECK(*token != *pw);
  auto other = make("station-pw-2");
  REQUIRE(other->set_web_interface(true, 8108));
  CHECK(fake().sts("station-pw-2", kPassword0) != pw);

  // a preset with the protection off: on again, with the same password
  std::string err;
  REQUIRE_MESSAGE(st->load_preset(network_preset(dir.path), &err), err);
  CHECK(fake().sts("station-pw", kPassword0) == pw);
  CHECK(fake().sts("station-pw", kProtectionAny) == std::optional<std::string>("1"));
  CHECK(fake().sts("station-pw", kJsonToken) == token);
  CHECK(fake().sts("station-pw", kWebserverEnable) == std::optional<std::string>("1"));

  // saved: the fake writes every value and the key into the file; the binding takes them out
  const std::string saved = (fs::path(dir.path) / ".bounce-7.tmp").string();
  REQUIRE_MESSAGE(st->save_preset(saved, &err), err);
  const std::string text = read_file(saved);
  CHECK(text.rfind("[FakeStereoTool]", 0) == 0);
  CHECK(text.find("sts." + std::to_string(kWebserverPort) + "=8107") != std::string::npos);  // the rest stays
  CHECK(text.find(*pw) == std::string::npos);
  CHECK(text.find(*token) == std::string::npos);
  CHECK(text.find("FAKEKEY0123456789abcdef") == std::string::npos);
  CHECK(text.find("sts." + std::to_string(kPassword0) + "=") != std::string::npos);  // the line, without its value
  // and it loads again (the protection set again after the load)
  REQUIRE_MESSAGE(st->load_preset(saved, &err), err);
  CHECK(fake().sts("station-pw", kPassword0) == pw);

  // never a web interface without its password
  fake().set("sts_rejects", kPassword0);
  CHECK_FALSE(other->set_web_interface(true, 8109));
  CHECK(fake().sts("station-pw-2", kWebserverEnable) == std::optional<std::string>("0"));
  fake().set("sts_rejects", -1);
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

namespace {

// The sockets of this process that are not on the loopback interface, as text (empty: none), and
// every socket of the process in `listing`. Windows: the TCP and UDP tables (IPv4 and IPv6); POSIX:
// lsof (nullopt when it cannot be run).
std::optional<std::vector<std::string>> foreign_sockets(std::string* listing) {
  std::vector<std::string> bad;
#ifdef _WIN32
  const DWORD pid = GetCurrentProcessId();
  auto v4 = [](DWORD addr) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr a{};
    a.s_addr = addr;
    inet_ntop(AF_INET, &a, text, sizeof text);
    return std::string(text);
  };
  auto v6 = [](const UCHAR* addr) {
    char text[INET6_ADDRSTRLEN] = {};
    inet_ntop(AF_INET6, addr, text, sizeof text);
    return std::string(text);
  };
  auto port = [](DWORD p) { return std::to_string(ntohs(static_cast<u_short>(p))); };
  auto table = [](auto get) {
    ULONG size = 0;
    get(nullptr, &size);
    std::vector<unsigned char> buf(size_t(size) + 16384);
    size = ULONG(buf.size());
    if (get(buf.data(), &size) != NO_ERROR) buf.clear();
    return buf;
  };
  auto tcp4 = table([](void* b, ULONG* n) { return GetExtendedTcpTable(b, n, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0); });
  auto tcp6 = table([](void* b, ULONG* n) { return GetExtendedTcpTable(b, n, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0); });
  auto udp4 = table([](void* b, ULONG* n) { return GetExtendedUdpTable(b, n, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0); });
  auto udp6 = table([](void* b, ULONG* n) { return GetExtendedUdpTable(b, n, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0); });
  if (tcp4.empty() || tcp6.empty() || udp4.empty() || udp6.empty()) return std::nullopt;
  for (const auto& r : std::span(reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(tcp4.data())->table,
                                 reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(tcp4.data())->dwNumEntries)) {
    if (r.dwOwningPid != pid) continue;
    const bool listen = r.dwState == MIB_TCP_STATE_LISTEN;
    const std::string text = "TCP " + v4(r.dwLocalAddr) + ":" + port(r.dwLocalPort) +
                             (listen ? std::string(" LISTEN") : " -> " + v4(r.dwRemoteAddr) + ":" + port(r.dwRemotePort));
    *listing += text + "\n";
    if (v4(r.dwLocalAddr) != "127.0.0.1" || (!listen && v4(r.dwRemoteAddr) != "127.0.0.1")) bad.push_back(text);
  }
  for (const auto& r : std::span(reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(tcp6.data())->table,
                                 reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(tcp6.data())->dwNumEntries)) {
    if (r.dwOwningPid != pid) continue;
    const std::string text = "TCP6 [" + v6(r.ucLocalAddr) + "]:" + port(r.dwLocalPort) + " -> [" + v6(r.ucRemoteAddr) + "]";
    *listing += text + "\n";
    if (v6(r.ucLocalAddr) != "::1") bad.push_back(text);
  }
  for (const auto& r : std::span(reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(udp4.data())->table,
                                 reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(udp4.data())->dwNumEntries)) {
    if (r.dwOwningPid != pid) continue;
    const std::string text = "UDP " + v4(r.dwLocalAddr) + ":" + port(r.dwLocalPort);
    *listing += text + "\n";
    if (v4(r.dwLocalAddr) != "127.0.0.1") bad.push_back(text);
  }
  for (const auto& r : std::span(reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(udp6.data())->table,
                                 reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(udp6.data())->dwNumEntries)) {
    if (r.dwOwningPid != pid) continue;
    const std::string text = "UDP6 [" + v6(r.ucLocalAddr) + "]:" + port(r.dwLocalPort);
    *listing += text + "\n";
    if (v6(r.ucLocalAddr) != "::1") bad.push_back(text);
  }
#else
  const std::string command = "lsof -nP -a -p " + std::to_string(bp::current_pid()) + " -i 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return std::nullopt;
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof buf, pipe)) out += buf;
  if (pclose(pipe) != 0 && out.empty()) return std::nullopt;  // no lsof, or no socket at all
  auto loopback = [](const std::string& end) {
    return end.rfind("127.0.0.1:", 0) == 0 || end.rfind("localhost:", 0) == 0 || end.rfind("[::1]:", 0) == 0;
  };
  std::istringstream lines(out);
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("COMMAND", 0) == 0 || line.empty()) continue;
    *listing += line + "\n";
    // NAME: "127.0.0.1:8100 (LISTEN)", "127.0.0.1:5000->127.0.0.1:8100 (ESTABLISHED)", "*:5353"
    std::istringstream fields(line);
    std::vector<std::string> f;
    for (std::string x; fields >> x;) f.push_back(x);
    std::string name;
    for (const auto& x : f)
      if (x.find(':') != std::string::npos && x.find("0x") != 0) name = x;
    const size_t arrow = name.find("->");
    const std::string local = name.substr(0, arrow);
    if (!loopback(local) || (arrow != std::string::npos && !loopback(name.substr(arrow + 2)))) bad.push_back(line);
  }
#endif
  return bad;
}

// The status line Stereo Tool's web server answers "GET /" with ("HTTP/1.0 200 OK"; it sends the
// page gzip-encoded, which httplib's client refuses without zlib): empty when nothing answers.
std::string http_status_line(uint16_t port) {
  std::string err;
  auto s = bp::TcpStream::connect("127.0.0.1", port, 500, &err);
  if (!s) return {};
  const std::string request = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  if (s->send_some({reinterpret_cast<const uint8_t*>(request.data()), request.size()}, 500) != int(request.size())) return {};
  std::string text;
  uint8_t buf[512];
  const int64_t end = bp::monotonic_ns() + 2'000'000'000;
  while (text.find("\r\n") == std::string::npos && bp::monotonic_ns() < end) {
    const int n = s->recv_some(buf, 50);
    if (n < 0) break;
    text.append(reinterpret_cast<const char*>(buf), size_t(n));
  }
  return text.substr(0, text.find("\r\n"));
}

// A free TCP port on 127.0.0.1 (for the web interface).
uint16_t free_loopback_port() {
  std::string err;
  auto l = bp::TcpListener::listen("127.0.0.1", 0, &err);
  REQUIRE_MESSAGE(l, err);
  return l->port();
}

// The library's own functions, for an instance outside the binding (a preset made "elsewhere").
struct RawStereoTool {
  bp::DynamicLibrary lib;
  st::Create3Fn create3 = nullptr;
  st::DeleteFn destroy = nullptr;
  st::SetStsValueFn set = nullptr;
  st::GetStsValueFn get = nullptr;
  st::SavePresetFn save = nullptr;
  explicit RawStereoTool(const std::string& dir) {
    std::string err;
    auto l = bp::DynamicLibrary::open({(fs::path(dir) / be::stereotool_library_names().front()).string()}, &err);
    REQUIRE_MESSAGE(l, err);
    lib = std::move(*l);
    create3 = reinterpret_cast<st::Create3Fn>(lib.symbol("stereoTool_Create3"));
    destroy = reinterpret_cast<st::DeleteFn>(lib.symbol("stereoTool_Delete"));
    set = reinterpret_cast<st::SetStsValueFn>(lib.symbol("stereoTool_SetStsValue"));
    get = reinterpret_cast<st::GetStsValueFn>(lib.symbol("stereoTool_GetStsValue"));
    save = reinterpret_cast<st::SavePresetFn>(lib.symbol("stereoTool_SavePreset"));
    REQUIRE((create3 && destroy && set && get && save));
  }
};

}  // namespace

// Windows CI runs this with the DLL next to the encoder (BAM_TEST_REAL_STEREOTOOL), in a run of its
// own, never with a licence key. A preset made by an instance outside the binding switches network
// features on (UECP, stream output where the library has it, MicroMPX, Poll STS, the scheduler)
// and the web interface without a password; loaded by the binding they are off, the web interface
// asks for the instance's password, and a preset the binding saves does not carry it. With the web
// interface serving, every socket of the process is on 127.0.0.1 (listeners and connections).
TEST_CASE("stereotool: the real library (BAM_TEST_REAL_STEREOTOOL) switches off a preset's network features; its web interface listens on 127.0.0.1 only, behind its password") {
  const char* dir = std::getenv("BAM_TEST_REAL_STEREOTOOL");
  if (!dir || !*dir) {
    MESSAGE("BAM_TEST_REAL_STEREOTOOL not set, real Stereo Tool network test skipped");
    return;
  }
  struct Restore {
    ~Restore() { be::set_stereotool_library_dirs({}); }
  } restore;
  be::set_stereotool_library_dirs({dir});
  const auto info = be::stereotool_info();  // binds web and UECP to 127.0.0.1 before any instance
  REQUIRE_MESSAGE(info.available, info.error);
  TempDir home("real-net");

  // "elsewhere": every network switch on, the web interface on without a password
  const std::string preset = (fs::path(home.path) / "network.sts").string();
  std::vector<int> carried;  // the switches this library has (accepted "1")
  {
    RawStereoTool raw(dir);
    gStereoTool* other = raw.create3(false, nullptr, "bam-elsewhere", home.path.c_str(), false);
    REQUIRE(other);
    for (const auto& [index, what] : network_switches())
      if (raw.set(other, index, 0, "1")) {
        const char* v = nullptr;
        if (raw.get(other, index, 0, &v) && v && std::string(v) == "1") carried.push_back(index);
      }
    raw.set(other, kWebserverEnable, 0, "1");
    raw.set(other, kWebserverPort, 0, std::to_string(free_loopback_port()).c_str());
    raw.set(other, kProtectionAny, 0, "0");
    REQUIRE(raw.save(other, preset.c_str(), kSaveAllSettings));
    raw.destroy(other);
  }
  REQUIRE_FALSE(carried.empty());
  CHECK(std::find(carried.begin(), carried.end(), 3838) != carried.end());  // PARAM_UECP_Enable: in every build
  const std::string text = read_file(preset);
  CHECK(text.find("Enable UECP=1") != std::string::npos);  // the preset does carry them
  CHECK(text.find("Poll STS Enabled=1") != std::string::npos);
  CHECK(text.find("Scheduler Enabled=1") != std::string::npos);

  auto st = make("bam-network", "", home.path);  // no licence key, ever
  const uint16_t port = free_loopback_port();
  REQUIRE(st->set_web_interface(true, port));
  std::string err;
  REQUIRE_MESSAGE(st->load_preset(preset, &err), err);
  for (const auto& [index, what] : network_switches()) {
    const auto v = st->sts_value(index);
    CHECK_MESSAGE((!v || *v == "0"), index << " " << what << " = " << v.value_or("(none)"));
  }
  CHECK(st->sts_value(kWebserverPort) == std::optional<std::string>(std::to_string(port)));
  CHECK(st->sts_value(kWebserverEnable) == std::optional<std::string>("1"));
  CHECK(st->sts_value(kProtectionAny) == std::optional<std::string>("1"));
  CHECK(st->sts_value(kPassword0) == std::optional<std::string>(st->web_password()));
  CHECK(st->web_password().size() >= 16);

  // a preset saved through the binding: without the password
  const std::string saved = (fs::path(home.path) / ".bounce-2.tmp").string();
  REQUIRE_MESSAGE(st->save_preset(saved, &err), err);
  const std::string saved_text = read_file(saved);
  CHECK(saved_text.size() > 1000);
  CHECK(saved_text.find(st->web_password()) == std::string::npos);
  CHECK(saved_text.find("Enable UECP=0") != std::string::npos);

  // the web interface serves (Stereo Tool opens it while processing), on 127.0.0.1 only
  std::vector<float> buf(480 * 2, 0.0f);
  bool served = false;
  for (int round = 0; round < 50 && !served; ++round) {
    for (int block = 0; block < 20; ++block) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      REQUIRE_MESSAGE(st->process(buf.data(), 480), st->failure());
    }
    served = http_status_line(port).find(" 200") != std::string::npos;
  }
  CHECK_MESSAGE(served, "the web interface did not answer on 127.0.0.1:" << port);
  std::string listing;
  const auto bad = foreign_sockets(&listing);
  if (!bad) {
    WARN_MESSAGE(false, "the sockets of the process could not be listed");
  } else {
    CHECK_MESSAGE(bad->empty(), "sockets outside 127.0.0.1:\n" << listing);
    CHECK_MESSAGE(listing.find(":" + std::to_string(port)) != std::string::npos, listing);  // the listener was seen
  }
  MESSAGE("real Stereo Tool network: " << carried.size() << " network switches carried by a preset, all off after the load; sockets:\n"
                                       << listing);
}

#endif  // BAM_FAKE_STEREOTOOL_PATH
