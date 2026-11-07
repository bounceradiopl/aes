// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for Thimeo's libStereoTool in the tests, built as a shared library under the real
// library's file name (libStereoTool_64.dll / .dylib, ...) in a folder of its own. It exports
// the stereoTool_* functions the binding uses, with the same signatures, and behaves simply and
// exactly: the output is the input delayed by `latency` frames and multiplied by `gain` (float),
// per channel. Presets are small text files holding gain and latency.
//
// The tests steer it through the fake_stereotool_* functions (options, counters, recorded
// arguments); they open this library by its path, so they share its state with the binding.
// Options (fake_stereotool_set; defaults after fake_stereotool_reset):
//   gain 0.5, latency 512 (frames; both copied into an instance when it is created),
//   version 11070, license_valid 1, create_fails 0, create_throws 0 (a C++ exception whose text
//   holds the key), process_throws 0 (a C++ exception), process_crashes 0 (Windows: an access
//   violation, a structured exception), process_sleep_us 0, create_sleep_us 0 (inside Create3),
//   version_crashes 0 / bind_crashes 0 (Windows: an access violation in GetSoftwareVersion /
//   global_setBindIp), save_fails 0, sts_rejects -1 (the .sts index SetStsValue refuses),
//   save_sleep_us 0 (inside SavePreset), version_sleep_us 0 (inside GetSoftwareVersion: a slow load).
// A manual run can set options through the environment (BAM_FAKE_STEREOTOOL_OPTIONS, see State).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../../encoder/src/stereotool_api.hpp"  // the parameter indices the binding uses

#ifdef _WIN32
#include <windows.h>
#define FAKE_ST_EXPORT extern "C" __declspec(dllexport)
#else
#define FAKE_ST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

class gStereoTool {
 public:
  std::string name;
  float gain = 1.0f;
  int latency = 0;
  int channels = 2;
  std::vector<float> line;  // latency * channels samples, a ring
  size_t pos = 0;
  std::map<int, std::string> sts;

  void reset_line() {
    line.assign(size_t(latency) * size_t(channels), 0.0f);
    pos = 0;
  }
};

namespace {

constexpr int kWebserverEnable = bam::encoder::detail::st::kParamWebserverEnable;
constexpr int kWebserverPort = bam::encoder::detail::st::kParamWebserverPort;

struct State {
  std::mutex mutex;
  std::map<std::string, double> options;
  std::map<std::string, long long> values;   // counters and recorded numbers
  std::map<std::string, std::string> texts;  // recorded strings (absent: not recorded / NULL)
  std::vector<gStereoTool*> live;            // instances not deleted yet

  void defaults() {
    options = {{"gain", 0.5},          {"latency", 512},      {"version", 11070},
               {"license_valid", 1},   {"create_fails", 0},   {"create_throws", 0},
               {"process_throws", 0},  {"process_crashes", 0}, {"process_sleep_us", 0},
               {"create_sleep_us", 0}, {"version_crashes", 0}, {"bind_crashes", 0},
               {"save_fails", 0},      {"sts_rejects", -1},    {"save_sleep_us", 0},
               {"version_sleep_us", 0}};
    values.clear();
    texts.clear();
  }
  // BAM_FAKE_STEREOTOOL_OPTIONS="license_valid=0;process_sleep_us=30000" sets options when the
  // library is loaded, for a manual run of the service with the fake (the panel's browser check);
  // the tests never set it, and fake_stereotool_reset() goes back to the defaults.
  State() {
    defaults();
    const char* env = std::getenv("BAM_FAKE_STEREOTOOL_OPTIONS");
    for (std::string rest = env ? env : ""; !rest.empty();) {
      const size_t end = rest.find(';');
      const std::string item = rest.substr(0, end);
      rest = end == std::string::npos ? std::string() : rest.substr(end + 1);
      if (item.empty()) continue;
      const size_t eq = item.find('=');
      const std::string name = item.substr(0, eq);
      if (eq == std::string::npos || !options.count(name)) {
        std::fprintf(stderr, "fake Stereo Tool: unknown option \"%s\" in BAM_FAKE_STEREOTOOL_OPTIONS (ignored)\n", name.c_str());
        continue;
      }
      const char* text = item.c_str() + eq + 1;
      char* stop = nullptr;
      const double value = std::strtod(text, &stop);
      if (stop == text || *stop != '\0') {
        std::fprintf(stderr, "fake Stereo Tool: bad value \"%s\" for option \"%s\" (ignored)\n", text, name.c_str());
        continue;
      }
      options[name] = value;
    }
  }
  double opt(const char* name) {
    auto it = options.find(name);
    return it == options.end() ? 0.0 : it->second;
  }
  void text(const char* what, const char* value) {
    if (value)
      texts[what] = value;
    else
      texts.erase(what);
  }
};

State& state() {
  static State s;
  return s;
}

[[noreturn]] void fail_process() { throw std::runtime_error("fake Stereo Tool process failure"); }
[[noreturn]] void fail_create(const std::string& key) {
  throw std::runtime_error("fake Create3 failure, key " + key);
}

// Raised with no lock held (see stereoTool_Process).
void crash() {
#ifdef _WIN32
  RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
}

const char* kUnlicensed = "Fake: Declipper, Fake: Multiband";

}  // namespace

// ---- the libStereoTool functions the binding uses ----

FAKE_ST_EXPORT void stereoTool_global_setBindIp(const char* web, const char* uecp) {
  auto& s = state();
  bool crashes = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.values["bind_calls"];
    s.values["bind_create_calls"] = s.values["create_calls"];  // Create3 calls (since reset) before it
    s.text("bind_web", web);
    s.text("bind_uecp", uecp);
    crashes = s.opt("bind_crashes") != 0;
  }
  if (crashes) crash();
}

FAKE_ST_EXPORT int stereoTool_GetSoftwareVersion() {
  auto& s = state();
  int version = 0;
  bool crashes = false;
  long long sleep_us = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.values["version_calls"];
    version = int(s.opt("version"));
    crashes = s.opt("version_crashes") != 0;
    sleep_us = (long long)s.opt("version_sleep_us");
  }
  if (sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  if (crashes) crash();
  return version;
}

FAKE_ST_EXPORT gStereoTool* stereoTool_Create3(const bool has_gui, const char* key, const char* st_name,
                                               const char* home_path, bool load_ini) {
  auto& s = state();
  bool throws = false;
  long long sleep_us = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.values["create_calls"];
    s.values["create_has_gui"] = has_gui;
    s.values["create_load_ini"] = load_ini;
    s.text("create_key", key);
    s.text("create_name", st_name);
    s.text("create_home", home_path);
    throws = s.opt("create_throws") != 0;
    sleep_us = (long long)s.opt("create_sleep_us");
  }
  if (sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  if (throws) fail_create(key ? key : "");  // no lock held (see stereoTool_Process)
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.opt("create_fails") != 0) return nullptr;
  auto* st = new gStereoTool;
  st->name = st_name ? st_name : "";
  st->gain = float(s.opt("gain"));
  st->latency = int(s.opt("latency"));
  st->reset_line();
  st->sts[kWebserverEnable] = "0";
  st->sts[kWebserverPort] = "8080";
  s.live.push_back(st);
  ++s.values["created"];
  return st;
}

FAKE_ST_EXPORT void stereoTool_Delete(gStereoTool* st) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  for (auto it = s.live.begin(); it != s.live.end(); ++it)
    if (*it == st) {
      s.live.erase(it);
      ++s.values["deleted"];
      delete st;
      return;
    }
  ++s.values["delete_unknown"];  // not an instance of ours (or deleted twice)
}

FAKE_ST_EXPORT void stereoTool_Process(gStereoTool* st, float* samples, int32_t numsamples, int32_t channels,
                                       int32_t samplerate) {
  auto& s = state();
  long long sleep_us = 0;
  bool throws = false, crashes = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.values["process_calls"];
    s.values["process_frames"] += numsamples;
    s.values["process_channels"] = channels;
    s.values["process_rate"] = samplerate;
    throws = s.opt("process_throws") != 0;
    crashes = s.opt("process_crashes") != 0;
    sleep_us = (long long)s.opt("process_sleep_us");
  }
  // thrown with no lock held: MSVC (/EHsc) need not unwind an extern "C" function's locals
  if (throws) fail_process();
  if (crashes) crash();
  if (sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  if (channels != st->channels) {
    st->channels = channels;
    st->reset_line();
  }
  const size_t n = size_t(numsamples) * size_t(channels);
  if (st->line.empty()) {
    for (size_t i = 0; i < n; ++i) samples[i] = samples[i] * st->gain;
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    const float delayed = st->line[st->pos];
    st->line[st->pos] = samples[i];
    st->pos = (st->pos + 1) % st->line.size();
    samples[i] = delayed * st->gain;
  }
}

FAKE_ST_EXPORT int stereoTool_GetLatency2(gStereoTool* st, int32_t samplerate, bool feed_silence) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.values["latency_calls"];
  s.values["latency_rate"] = samplerate;
  s.values["latency_feed_silence"] = feed_silence;
  return st->latency;
}

// Preset file: "[FakeStereoTool]", "gain=<float>", "latency=<frames>". Loading resets the web
// interface settings (as a real preset may carry them) and the delay line.
FAKE_ST_EXPORT bool stereoTool_LoadPreset(gStereoTool* st, const char* filename, int loadsave_type) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.values["load_calls"];
  s.values["load_type"] = loadsave_type;
  s.text("load_path", filename);
  std::ifstream f(filename ? filename : "", std::ios::binary);
  std::string line;
  if (!f || !std::getline(f, line) || line != "[FakeStereoTool]") return false;
  float gain = st->gain;
  int latency = st->latency;
  while (std::getline(f, line)) {
    if (line.rfind("gain=", 0) == 0) gain = std::strtof(line.c_str() + 5, nullptr);
    if (line.rfind("latency=", 0) == 0) latency = std::atoi(line.c_str() + 8);
  }
  st->gain = gain;
  st->latency = latency < 0 ? 0 : latency;
  st->reset_line();
  st->sts[kWebserverEnable] = "0";
  st->sts[kWebserverPort] = "8080";
  return true;
}

FAKE_ST_EXPORT bool stereoTool_SavePreset(gStereoTool* st, const char* filename, int loadsave_type) {
  auto& s = state();
  long long sleep_us = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    sleep_us = (long long)s.opt("save_sleep_us");
  }
  if (sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.values["save_calls"];
  s.values["save_type"] = loadsave_type;
  s.text("save_path", filename);
  if (s.opt("save_fails") != 0) return false;
  std::ofstream f(filename ? filename : "", std::ios::binary | std::ios::trunc);
  if (!f) return false;
  char gain[32];
  std::snprintf(gain, sizeof gain, "%.9g", double(st->gain));
  f << "[FakeStereoTool]\ngain=" << gain << "\nlatency=" << st->latency << "\n";
  return bool(f);
}

FAKE_ST_EXPORT bool stereoTool_SetStsValue(gStereoTool* st, int index, int subindex, const char* value) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.values["sts_set_calls"];
  if (subindex != 0 || !value || index == int(s.opt("sts_rejects"))) return false;
  st->sts[index] = value;
  // a web interface port is one socket: two live instances serving on it is a conflict
  auto serving = [](const gStereoTool* i) {
    auto e = i->sts.find(kWebserverEnable);
    return e != i->sts.end() && e->second == "1";
  };
  if (serving(st))
    for (const gStereoTool* other : s.live)
      if (other != st && serving(other) && other->sts.at(kWebserverPort) == st->sts.at(kWebserverPort)) ++s.values["port_conflicts"];
  return true;
}

FAKE_ST_EXPORT bool stereoTool_CheckLicenseValid(gStereoTool*) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.values["license_calls"];
  return s.opt("license_valid") != 0;
}

FAKE_ST_EXPORT bool stereoTool_GetUnlicensedUsedFeatures(gStereoTool*, char* text, int text_maxlen) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  const bool valid = s.opt("license_valid") != 0;
  if (text && text_maxlen > 0) {
    const char* t = valid ? "" : kUnlicensed;
    std::strncpy(text, t, size_t(text_maxlen) - 1);
    text[text_maxlen - 1] = '\0';
  }
  return valid;
}

// ---- test control ----

FAKE_ST_EXPORT void fake_stereotool_reset() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.defaults();
}

FAKE_ST_EXPORT void fake_stereotool_set(const char* option, double value) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.options[option] = value;
}

// Counters ("created", "deleted", "process_calls", "bind_calls", "port_conflicts": an instance
// switched its web interface on at a port another live instance serves on, ...) and recorded numbers
// ("process_rate", "load_type", ...); "live" is the number of instances not deleted.
FAKE_ST_EXPORT long long fake_stereotool_value(const char* what) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (std::strcmp(what, "live") == 0) return (long long)s.live.size();
  auto it = s.values.find(what);
  return it == s.values.end() ? 0 : it->second;
}

// A recorded string ("bind_web", "create_key", "load_path", ...): 1 and the text, 0 when none
// was recorded (or the function got NULL).
FAKE_ST_EXPORT int fake_stereotool_text(const char* what, char* out, int maxlen) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.texts.find(what);
  if (it == s.texts.end()) return 0;
  std::snprintf(out, size_t(maxlen), "%s", it->second.c_str());
  return 1;
}

// The .sts value `index` of the newest live instance called `st_name`: 1 and the text, 0 when
// there is no such instance or value.
FAKE_ST_EXPORT int fake_stereotool_sts(const char* st_name, int index, char* out, int maxlen) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  for (auto it = s.live.rbegin(); it != s.live.rend(); ++it) {
    if ((*it)->name != st_name) continue;
    auto v = (*it)->sts.find(index);
    if (v == (*it)->sts.end()) return 0;
    std::snprintf(out, size_t(maxlen), "%s", v->second.c_str());
    return 1;
  }
  return 0;
}
