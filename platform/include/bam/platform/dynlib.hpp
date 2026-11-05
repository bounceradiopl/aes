// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace bam::platform {

// Run-time loaded shared library (codec DLLs). Tries each candidate path/name in order and
// keeps the first one that loads.
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
