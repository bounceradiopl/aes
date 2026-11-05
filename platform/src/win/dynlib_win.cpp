// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "bam/platform/dynlib.hpp"
#include "win_util.hpp"

namespace bam::platform {

DynamicLibrary::DynamicLibrary(DynamicLibrary&& o) noexcept : handle_(o.handle_), path_(std::move(o.path_)) {
  o.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& o) noexcept {
  if (this != &o) {
    close();
    handle_ = o.handle_;
    path_ = std::move(o.path_);
    o.handle_ = nullptr;
  }
  return *this;
}

DynamicLibrary::~DynamicLibrary() { close(); }

void DynamicLibrary::close() {
  if (handle_) {
    FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;
  }
}

std::optional<DynamicLibrary> DynamicLibrary::open(const std::vector<std::string>& candidates, std::string* error) {
  std::string last_error;
  for (const auto& c : candidates) {
    HMODULE h = LoadLibraryW(widen(c).c_str());
    if (h) {
      DynamicLibrary lib;
      lib.handle_ = h;
      lib.path_ = c;
      return lib;
    }
    last_error = c + ": " + last_error_text("LoadLibrary");
  }
  if (error) {
    std::string list;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (i) list += ", ";
      list += candidates[i];
    }
    *error = "nie udało się otworzyć żadnej z bibliotek: " + list;
    if (!last_error.empty()) *error += " (" + last_error + ")";
  }
  return std::nullopt;
}

void* DynamicLibrary::symbol(const char* name) const {
  return handle_ ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name)) : nullptr;
}

}  // namespace bam::platform
