// SPDX-License-Identifier: GPL-3.0-or-later
// The Stereo Tool binding (bam/encoder/stereotool.hpp) against the fake library of
// tests/fake_stereotool (gain + delay, counters, failure modes). The real libStereoTool is used
// only when BAM_TEST_REAL_STEREOTOOL names a folder holding it (no licence key, silence only).
#include <doctest/doctest.h>

#ifdef BAM_FAKE_STEREOTOOL_PATH

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "bam/encoder/stereotool.hpp"
#include "bam/platform/dynlib.hpp"
#include "bam/platform/process.hpp"
#include "test_util.hpp"

namespace be = bam::encoder;
namespace bp = bam::platform;
namespace fs = std::filesystem;

namespace {

constexpr int kWebserverEnable = 524;  // ParameterEnum.h 11.07
constexpr int kWebserverPort = 525;
constexpr long long kSaveAllSettings = 20386;  // ID_SAVE_ALLSETTINGS
const std::string kUnlicensed = "Fake: Declipper, Fake: Multiband";

// The fake's control functions, from the same library file the binding loads (shared state).
struct Fake {
  bp::DynamicLibrary lib;
  void (*reset)() = nullptr;
  void (*set)(const char*, double) = nullptr;
  long long (*value)(const char*) = nullptr;
  int (*text_fn)(const char*, char*, int) = nullptr;
  int (*sts_fn)(const char*, int, char*, int) = nullptr;

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

Fake& fake() {
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
    REQUIRE(x.reset);
    REQUIRE(x.set);
    REQUIRE(x.value);
    REQUIRE(x.text_fn);
    REQUIRE(x.sts_fn);
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
  ~UseFake() { be::set_stereotool_library_dirs({}); }
  UseFake(const UseFake&) = delete;
  UseFake& operator=(const UseFake&) = delete;
};

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

TEST_CASE("stereotool: an exception in process is caught, the instance is failed and abandoned") {
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
  // abandoned: no stereoTool_Delete on an instance in an unknown state
  st.reset();
  CHECK(fake().value("deleted") == 0);
  CHECK(fake().value("delete_unknown") == 0);
}

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
  REQUIRE_MESSAGE(st->load_preset(path, &err), err);
  CHECK(fake().sts("station-web", kWebserverEnable) == std::optional<std::string>("0"));
  // still only on 127.0.0.1
  CHECK(fake().text("bind_web") == std::optional<std::string>("127.0.0.1"));
}

TEST_CASE("stereotool: the real library (BAM_TEST_REAL_STEREOTOOL) processes silence without a licence key") {
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
  CHECK(info.version >= 11070);
  TempDir home("real");
  auto st = make("bam-test", "", home.path);  // no licence key, ever
  CHECK(st->latency_frames() >= 0);
  std::vector<float> buf(480 * 2);
  bool finite = true;
  for (int block = 0; block < 200; ++block) {
    std::fill(buf.begin(), buf.end(), 0.0f);
    REQUIRE_MESSAGE(st->process(buf.data(), 480), st->failure());
    for (float x : buf)
      if (!std::isfinite(x)) finite = false;
  }
  CHECK(finite);
  MESSAGE("real Stereo Tool " << info.version << ", latency " << st->latency_frames() << " frames, licence valid "
                              << st->license_valid());
  st.reset();
}

#endif  // BAM_FAKE_STEREOTOOL_PATH
