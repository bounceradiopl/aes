// SPDX-License-Identifier: GPL-3.0-or-later
#include <dlfcn.h>

#include "bam/platform/dynlib.hpp"

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
    dlclose(handle_);
    handle_ = nullptr;
  }
}

std::optional<DynamicLibrary> DynamicLibrary::open(const std::vector<std::string>& candidates, std::string* error) {
  std::string last_error;
  for (const auto& c : candidates) {
    dlerror();  // clear any pending error
    void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h) {
      DynamicLibrary lib;
      lib.handle_ = h;
      lib.path_ = c;
      return lib;
    }
    const char* e = dlerror();
    last_error = c + ": " + (e ? e : "nieznany błąd");
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

void* DynamicLibrary::symbol(const char* name) const { return handle_ ? dlsym(handle_, name) : nullptr; }

}  // namespace bam::platform
