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

struct StereoToolInfo {
  bool available = false;
  std::string path;    // the library that was loaded
  int version = 0;     // stereoTool_GetSoftwareVersion(): 11070 for 11.07
  std::string error;   // Polish, for the panel, when not available
};

// The library, loaded once per search-dir setting (default: the program folder). Before the
// library is used for anything else, the web interface and UECP servers of every instance are
// bound to 127.0.0.1 (stereoTool_global_setBindIp, which must precede the first instance).
// A loaded library stays loaded until the process ends: Stereo Tool may keep threads of its own
// after the last instance is deleted.
StereoToolInfo stereotool_info();

// File names searched in each dir: libStereoTool_64.dll (Windows x64), libStereoTool_64.dylib
// (macOS), libStereoTool_intel64.so / libStereoTool_noX11_arm64.so / libStereoTool_arm64.so (Linux).
std::vector<std::string> stereotool_library_names();

// Tests: search these dirs instead of the program folder (empty: back to the program folder).
void set_stereotool_library_dirs(std::vector<std::string> dirs);

// One Stereo Tool instance (one per processed station; one licence key per instance).
// Not thread-safe: one thread uses an instance at a time.
// Every call into the library is guarded: a C++ exception (and on MSVC builds a structured
// exception, e.g. an access violation) is caught, the instance is marked failed and is not
// called again -- not even stereoTool_Delete: its state is unknown, so it is abandoned.
class StereoToolInstance {
 public:
  // stereoTool_Create3(has_gui = false, key, name, home_dir, load_ini = false); an empty key,
  // name or home_dir is passed as NULL (no licence / the library's defaults). nullptr and a
  // Polish message when the library is missing or the instance cannot be created; the message
  // never contains the key.
  static std::unique_ptr<StereoToolInstance> create(const std::string& key, const std::string& name,
                                                    const std::string& home_dir, std::string* error);
  ~StereoToolInstance();  // stereoTool_Delete (unless failed)
  StereoToolInstance(const StereoToolInstance&) = delete;
  StereoToolInstance& operator=(const StereoToolInstance&) = delete;

  // A .sts preset, all settings except Stereo Tool's configuration (ID_SAVE_ALLSETTINGS). The web
  // interface settings of set_web_interface() are applied again after loading.
  bool load_preset(const std::string& sts_path, std::string* error);
  bool save_preset(const std::string& sts_path, std::string* error);

  // The instance's web interface (enable + port, through stereoTool_SetStsValue), reachable on
  // 127.0.0.1 only (the global bind address). Port 1..65535 when enabled.
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
  const std::string& failure() const { return failure_; }  // Polish, empty unless failed()

 private:
  StereoToolInstance(std::shared_ptr<const detail::StereoToolApi> api, gStereoTool* st);
  template <typename F>
  bool call(const char* function, F&& f);
  bool apply_web_interface();

  std::shared_ptr<const detail::StereoToolApi> api_;
  gStereoTool* st_ = nullptr;
  bool failed_ = false;
  std::string failure_;
  bool web_set_ = false, web_enabled_ = false;
  int web_port_ = 0;
};

}  // namespace bam::encoder
