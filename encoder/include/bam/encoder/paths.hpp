// SPDX-License-Identifier: GPL-3.0-or-later
// Where the Bounce Encoder files live, header-only so the tray (which links nothing of the
// encoder) uses the same names as the service.
//
// %ProgramData%\Bounce Encoder\            administrators + SYSTEM only (the installer sets the
//   config.json, logs\                     ACL): stream passwords and the panel password hash
//   public\                                inheritable read access for Users (installer)
//     panel.json                           {"bind", "port"} for the tray, nothing secret
//
// panel.json is replaced by a rename at every service start. A file created in public\ inherits
// that folder's ACL, so the rename keeps it readable for the tray; an ACE on the file itself would
// be lost with every start.
#pragma once
#include <string>

namespace bam::encoder {

inline constexpr const char* kEncoderDataDirName = "Bounce Encoder";  // inside %ProgramData%
inline constexpr const char* kPanelPublicDirName = "public";
inline constexpr const char* kPanelFileName = "panel.json";

// The folder that holds panel.json for a given configuration file: "<config dir>/public", with
// the separator the configuration path uses ('\\' or '/'; a bare file name means the current
// directory).
inline std::string panel_public_dir(const std::string& config_path) {
  const auto pos = config_path.find_last_of("/\\");
  if (pos == std::string::npos) return std::string(".") + '/' + kPanelPublicDirName;
  return config_path.substr(0, pos + 1) + kPanelPublicDirName;
}

// "<config dir>/public/panel.json" (see panel_public_dir).
inline std::string panel_file_path(const std::string& config_path) {
  const auto pos = config_path.find_last_of("/\\");
  const char sep = pos == std::string::npos ? '/' : config_path[pos];
  return panel_public_dir(config_path) + sep + kPanelFileName;
}

}  // namespace bam::encoder
