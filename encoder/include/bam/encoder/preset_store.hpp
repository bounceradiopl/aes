// SPDX-License-Identifier: GPL-3.0-or-later
// Stereo Tool's folder of Bounce Encoder 1.1 (spec §2, §5, §7):
//
//   %ProgramData%\Bounce Encoder\stereotool\    administrators + SYSTEM only, like config.json
//     presets\*.sts                              the uploaded presets (at most 64, 4 MB each)
//     station-<station id>\                      each station's Stereo Tool home folder
//
// The station folders carry a prefix so that no station id (e.g. "presets") can name the
// presets folder. Folders the service creates get the protected admin-only DACL of data_dir.hpp
// (Windows), never the access they would inherit.
//
// Preset names are valid_preset_name() (config.hpp) and compare without regard to case
// everywhere (Windows file names do not): a name is stored as given, and "fm.sts" finds, refuses
// a duplicate of, or is in use as "FM.sts". Only valid names in presets\ are presets; anything else
// there (a temporary file of an upload or a save) is ignored, and left-over temporary files go at
// the next start. Thread-safe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bam::encoder {

inline constexpr size_t kMaxPresetBytes = 4 * 1024 * 1024;
inline constexpr size_t kMaxPresets = 64;
inline constexpr const char* kStereoToolDirName = "stereotool";  // next to config.json
inline constexpr const char* kPresetsDirName = "presets";
inline constexpr const char* kStationHomePrefix = "station-";

enum class PresetError {
  None,
  BadName,     // not valid_preset_name()
  Empty,       // no data
  TooLarge,    // over kMaxPresetBytes
  TooMany,     // kMaxPresets stored already
  Exists,      // a preset of that name (any case) is stored
  NotFound,
  InUse,       // a station refers to it (EncoderEngine::remove_preset)
  NotRunning,  // the station runs no processing (EncoderEngine::save_processing_preset)
  Busy,        // a save of that station is under way already
  Failed,      // the file system or Stereo Tool failed
};

struct PresetResult {
  PresetError error = PresetError::None;
  std::string message;  // Polish, when not ok
  bool ok() const { return error == PresetError::None; }
};

struct PresetInfo {
  std::string name;  // as stored
  uint64_t bytes = 0;
};

// ASCII case-insensitive equality (preset names are ASCII).
bool same_preset_name(std::string_view a, std::string_view b);

class PresetStore {
 public:
  // `root`: the stereotool folder; empty: none (every operation fails, paths are empty).
  // `protect`: create the folders with kDataDirSddl.
  PresetStore(std::string root, bool protect);

  const std::string& root() const { return root_; }
  std::string presets_dir() const;  // empty without a root
  // Creates the stereotool and presets folders when missing and removes left-over temporary files.
  bool prepare(std::string* error);
  // The station's home folder, created when missing; empty (with a Polish *error) when it cannot be.
  std::string station_home(const std::string& station_id, std::string* error);
  // Removes a deleted station's home folder with everything in it (Stereo Tool's files for that
  // instance); true when it is gone (or never was). Only for a station no longer configured, after
  // its worker stopped.
  bool remove_station_home(const std::string& station_id, std::string* error);

  std::vector<PresetInfo> list() const;  // sorted by name, case-insensitively
  // The full path of the stored preset matching `name` (any case); when there is none, where it
  // would be stored; empty without a root or for an invalid name.
  std::string path_of(std::string_view name) const;
  PresetResult put(std::string_view name, std::string_view data);
  PresetResult get(std::string_view name, std::string* data) const;
  PresetResult remove(std::string_view name);

  // Saving a preset made elsewhere (Stereo Tool writes the file itself): reserve() checks the
  // name, the count and duplicates and gives a temporary path in presets\ to write to; commit()
  // makes it the preset `name` (checked again) or removes it; abandon() removes it.
  PresetResult reserve(std::string_view name, std::string* temp_path);
  PresetResult commit(std::string_view name, const std::string& temp_path);
  void abandon(const std::string& temp_path);

 private:
  std::string find_locked(std::string_view name) const;  // stored file name or ""
  size_t count_locked() const;
  std::string temp_path_locked();
  PresetResult check_new_locked(std::string_view name) const;

  const std::string root_;
  const bool protect_;
  mutable std::mutex m_;
  uint64_t temp_counter_ = 0;
};

}  // namespace bam::encoder
