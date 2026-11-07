// SPDX-License-Identifier: GPL-3.0-or-later
// Thimeo Stereo Tool (libStereoTool 11.07) for the per-station processing of Bounce Encoder 1.1
// (spec docs/superpowers/specs/2026-11-06-bounce-encoder-stereotool-design.md §4). The library is
// commercial and not GPL: it is loaded at run time, only from the program folder (like the
// codecs, never a bare name), and only the functions used here are declared (the Thimeo headers
// are not part of this repository).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class gStereoTool;  // the library's instance type, only ever used through pointers

namespace bam::encoder {

namespace detail {
struct StereoToolApi;  // stereotool.cpp
}

// Stereo Tool always runs on the station mix: 48 kHz, interleaved stereo float.
inline constexpr int32_t kStereoToolRate = 48000;
inline constexpr int32_t kStereoToolChannels = 2;
inline constexpr int kStereoToolVersion = 11070;     // the only version used (parameter indices of 11.07)
inline constexpr int kStereoToolMaxAbandoned = 1;    // abandoned instances before create() refuses (StereoToolInstance)

struct StereoToolInfo {
  bool available = false;
  std::string path;    // the library that was loaded
  int version = 0;     // stereoTool_GetSoftwareVersion(): 11070 for 11.07
  std::string error;   // Polish, for the panel, when not available
  int abandoned = 0;   // instances abandoned after a crash in the library (process-wide)
  bool checked = false;  // the library was looked for (stereotool_peek: not yet when false)
  bool loading = false;  // stereotool_peek: being loaded right now (nothing else known yet)
};

// The library, loaded once per search-dir setting (default: the program folder). Before the
// library is used for anything else, the web interface and UECP servers of every instance are
// bound to 127.0.0.1 (stereoTool_global_setBindIp, which must precede the first instance).
// A loaded library stays loaded until the process ends: Stereo Tool may keep threads of its own
// after the last instance is deleted.
StereoToolInfo stereotool_info();
// The same, without looking for the library when nothing has asked for it yet (checked false):
// the engine's status, which must not load the 117 MB library on an encoder without processing.
// Never waits for a load under way either: then loading is true (and error says so).
StereoToolInfo stereotool_peek();

// File names searched in each dir: libStereoTool_64.dll (Windows x64), libStereoTool_64.dylib
// (macOS), libStereoTool_intel64.so / libStereoTool_noX11_arm64.so / libStereoTool_arm64.so (Linux).
std::vector<std::string> stereotool_library_names();

// Tests: search these dirs instead of the program folder (empty: back to the program folder).
void set_stereotool_library_dirs(std::vector<std::string> dirs);

// The full paths searched for the current dir setting (absolute dirs only, never a bare name).
std::vector<std::string> stereotool_library_candidates();

// Tries again to load a library that could not be loaded because it was missing or could not be
// opened (e.g. "restart processing" after the DLL was put in place). A library of another version
// stays loaded in the process (a library is never unloaded), so replacing a wrong-version DLL
// needs a service restart. A library that is loaded, holds an abandoned instance or crashed while
// loading is kept as it is.
StereoToolInfo stereotool_reload();

namespace testing {
// Test hook: sets the process-wide count of abandoned instances.
void set_stereotool_abandoned(int count);
}  // namespace testing

// One Stereo Tool instance (one per processed station; one licence key per instance).
// Not thread-safe: one thread (the station's processing worker) uses an instance at a time;
// failed()/failure() are that thread's state too, so other threads (HTTP) ask the worker.
// Create3, Delete, LoadPreset and SavePreset of all instances are serialised by one process-wide
// mutex (never Process).
//
// Every call into the library is guarded, and the first failure marks the instance failed: it
// is not called again, except for one thing:
// - a C++ exception (ordered unwinding): the instance is still deleted when destroyed;
// - a structured exception (an access violation, a stack overflow, ...; caught by MSVC builds
//   only): its state is unknown, so it is abandoned, never deleted. Its memory stays taken and so
//   does its web interface port (that station's web interface stays off until the service
//   restarts). After the first abandoned instance no new instance is created: create() refuses
//   ("uruchom ponownie usługę Bounce Encoder"; a create already waiting for the API mutex too)
//   and processing stays bypassed. A crash while the library is loaded counts the same way.
// MinGW builds (development only; releases are built with MSVC) catch no structured exception,
// nor a C++ exception of the MSVC-built real DLL: a crash there ends the process.
class StereoToolInstance {
 public:
  // stereoTool_Create3(has_gui = false, key, name, home_dir, load_ini = false); an empty key,
  // name or home_dir is passed as NULL (no licence / the library's defaults). nullptr and a
  // Polish message when the library is missing or the instance cannot be created; the message
  // never contains the key, and asks for a service restart when that is the only way out.
  static std::unique_ptr<StereoToolInstance> create(const std::string& key, const std::string& name,
                                                    const std::string& home_dir, std::string* error);
  ~StereoToolInstance();  // stereoTool_Delete (unless abandoned)
  StereoToolInstance(const StereoToolInstance&) = delete;
  StereoToolInstance& operator=(const StereoToolInstance&) = delete;

  // A .sts preset, all settings except Stereo Tool's configuration (ID_SAVE_ALLSETTINGS). The web
  // interface settings of set_web_interface() are applied again after loading.
  bool load_preset(const std::string& sts_path, std::string* error);
  bool save_preset(const std::string& sts_path, std::string* error);

  // The instance's web interface (port, then enable, through stereoTool_SetStsValue), reachable
  // on 127.0.0.1 only (the global bind address). Port 1..65535 when enabled. When the library
  // refuses the port, the web interface is switched off (false).
  bool set_web_interface(bool enabled, int port);

  // In place, `frames` frames of interleaved stereo at 48 kHz. false when the instance has
  // failed (now or before); the samples are then not to be used.
  bool process(float* stereo, int32_t frames);

  // stereoTool_GetLatency2(48000, feed_silence = true): frames of delay of the active preset.
  // It sends silence through the chain, so call it only before processing (after a preset load).
  int latency_frames();

  // Whether the licence covers the features the current settings use (needs some audio
  // processed first), and if not, Stereo Tool's text listing the unlicensed features.
  bool license_valid();
  std::string unlicensed_features();

  bool failed() const { return failed_; }
  bool abandoned() const { return abandoned_; }  // this instance crashed (a structured exception)
  const std::string& failure() const { return failure_; }  // Polish, empty unless failed()

 private:
  StereoToolInstance(std::shared_ptr<const detail::StereoToolApi> api, gStereoTool* st);
  template <typename F>
  bool call(const char* function, F&& f);
  bool apply_web_interface();
  bool set_sts(int index, const std::string& value);

  std::shared_ptr<const detail::StereoToolApi> api_;
  gStereoTool* st_ = nullptr;
  bool failed_ = false;
  bool abandoned_ = false;
  std::string failure_;
  bool web_set_ = false, web_enabled_ = false;
  int web_port_ = 0;
};

}  // namespace bam::encoder
