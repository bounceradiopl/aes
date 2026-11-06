// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace bam::platform {

// Run-time loaded shared library (codec DLLs). Tries each candidate path/name in order and
// keeps the first one that loads.
// Windows: an absolute path is loaded with its own dependencies resolved only from the DLL's
// directory and System32 (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | ..._SYSTEM32); a bare name only
// from System32 -- never the current directory or PATH (no DLL planting, CWE-427).
// POSIX: dlopen() semantics (a name with a '/' is opened as that path, no search).
class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  ~DynamicLibrary();

  static std::optional<DynamicLibrary> open(const std::vector<std::string>& candidates, std::string* error);
  void* symbol(const char* name) const;
  const std::string& path() const { return path_; }

 private:
  void close();
  void* handle_ = nullptr;
  std::string path_;
};

}  // namespace bam::platform
