// SPDX-License-Identifier: GPL-3.0-or-later
// The service's data folders, created with their access (data_dir.hpp).
#include "bam/encoder/data_dir.hpp"

#include "bam/encoder/paths.hpp"
#include "bam/platform/process.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace bam::encoder {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
#endif

// true when the folder exists afterwards (created here or already there)
bool create_dir(const DataDir& d) {
#ifdef _WIN32
  if (!d.sddl.empty()) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(widen(d.sddl).c_str(), SDDL_REVISION_1, &sd, nullptr))
      return false;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    const bool ok = CreateDirectoryW(widen(d.path).c_str(), &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    LocalFree(sd);
    return ok;
  }
#endif
  return platform::ensure_directory(d.path);
}

}  // namespace

std::vector<DataDir> data_dirs(const std::string& config_path, bool protect) {
  const auto pos = config_path.find_last_of("/\\");
  const std::string dir = pos == std::string::npos ? std::string(".") : config_path.substr(0, pos);
  return {{dir, protect ? kDataDirSddl : ""}, {panel_public_dir(config_path), protect ? kPublicDirSddl : ""}};
}

bool create_data_dirs(const std::vector<DataDir>& dirs, std::string* error) {
  for (const DataDir& d : dirs) {
    if (create_dir(d)) continue;
    if (error) *error = "nie można utworzyć folderu " + d.path;
    return false;
  }
  return true;
}

}  // namespace bam::encoder
