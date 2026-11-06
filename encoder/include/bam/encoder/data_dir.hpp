// SPDX-License-Identifier: GPL-3.0-or-later
// The service's data folders (paths.hpp) and the access they are created with. The installer sets
// the ACL of %ProgramData%\Bounce Encoder and public\; when the service has to create a missing
// folder itself (deleted after the installation), it creates it with the same protected DACL in
// the CreateDirectoryW call, so the folder never exists with the ACL it would inherit from
// %ProgramData% (Users may read there), not even for a moment.
#pragma once
#include <string>
#include <vector>

namespace bam::encoder {

// %ProgramData%\Bounce Encoder: protected (nothing inherited from %ProgramData%), full control for
// Administrators (BA) and SYSTEM (SY), inherited by subfolders and files. The installer's
// FOLDER_SDDL is the same string.
inline constexpr const char* kDataDirSddl = "D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";
// public\ (panel.json for the tray): the same, plus read and list (FRFX) for Users (BU).
inline constexpr const char* kPublicDirSddl = "D:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;FRFX;;;BU)";

struct DataDir {
  std::string path;
  std::string sddl;  // empty: created as the platform creates any folder (inherited access)
};

// The folders for `config_path`, in creation order: the configuration folder, then its "public"
// folder (paths.hpp). `protect`: the service's own folder on Windows (%ProgramData%\Bounce
// Encoder), created with kDataDirSddl and kPublicDirSddl; otherwise (a --config elsewhere, POSIX)
// no SDDL.
std::vector<DataDir> data_dirs(const std::string& config_path, bool protect);

// Creates every folder that is missing; an existing folder keeps its ACL (the installer's job).
// A folder with an SDDL is created with that security descriptor (Windows; POSIX ignores it).
// false: `error` (Polish) names the first folder that could not be created.
bool create_data_dirs(const std::vector<DataDir>& dirs, std::string* error);

}  // namespace bam::encoder
