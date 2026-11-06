// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "win_util.hpp"

namespace bam::platform {

namespace {
// SYSTEM and Administrators full access, authenticated users full access (ASIO hosts run as users).
constexpr const wchar_t* kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)";

struct Security {
  SECURITY_ATTRIBUTES sa{};
  PSECURITY_DESCRIPTOR sd = nullptr;
  Security() {
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) sa.lpSecurityDescriptor = sd;
  }
  ~Security() {
    if (sd) LocalFree(sd);
  }
};

std::wstring scoped(const char* scope, const std::string& name) { return widen(std::string(scope) + name); }
}  // namespace

SharedMemory::SharedMemory(SharedMemory&& o) noexcept : data_(o.data_), size_(o.size_), handle_(o.handle_) {
  o.data_ = nullptr;
  o.handle_ = -1;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& o) noexcept {
  if (this != &o) {
    release();
    data_ = o.data_;
    size_ = o.size_;
    handle_ = o.handle_;
    o.data_ = nullptr;
    o.handle_ = -1;
  }
  return *this;
}

SharedMemory::~SharedMemory() { release(); }

void SharedMemory::release() {
  if (data_) UnmapViewOfFile(data_);
  if (handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(handle_));
  data_ = nullptr;
  handle_ = -1;
}

std::optional<SharedMemory> SharedMemory::create(const std::string& name, size_t size, bool* existed, std::string* error) {
  Security sec;
  const DWORD hi = static_cast<DWORD>(uint64_t(size) >> 32);
  const DWORD lo = static_cast<DWORD>(size & 0xFFFFFFFF);
  HANDLE h = nullptr;
  for (const char* scope : {"Global\\", "Local\\"}) {
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sec.sa, PAGE_READWRITE, hi, lo, scoped(scope, name).c_str());
    if (h) break;
  }
  if (!h) {
    if (error) *error = last_error_text("CreateFileMapping");
    return std::nullopt;
  }
  if (existed) *existed = GetLastError() == ERROR_ALREADY_EXISTS;
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!p) {
    if (error) *error = last_error_text("MapViewOfFile");
    CloseHandle(h);
    return std::nullopt;
  }
  SharedMemory m;
  m.data_ = p;
  m.size_ = size;
  m.handle_ = reinterpret_cast<intptr_t>(h);
  return m;
}

std::optional<SharedMemory> SharedMemory::open(const std::string& name, size_t size, std::string* error) {
  HANDLE h = nullptr;
  for (const char* scope : {"Global\\", "Local\\"}) {
    h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, scoped(scope, name).c_str());
    if (h) break;
  }
  if (!h) {
    if (error) *error = last_error_text("OpenFileMapping");
    return std::nullopt;
  }
  void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!p) {
    if (error) *error = last_error_text("MapViewOfFile");
    CloseHandle(h);
    return std::nullopt;
  }
  SharedMemory m;
  m.data_ = p;
  m.size_ = size;
  m.handle_ = reinterpret_cast<intptr_t>(h);
  return m;
}

void SharedMemory::remove(const std::string&) {}

NamedEvent::NamedEvent(NamedEvent&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }

NamedEvent& NamedEvent::operator=(NamedEvent&& o) noexcept {
  if (this != &o) {
    release();
    handle_ = o.handle_;
    o.handle_ = nullptr;
  }
  return *this;
}

NamedEvent::~NamedEvent() { release(); }

void NamedEvent::release() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
}

std::optional<NamedEvent> NamedEvent::create(const std::string& name, std::string* error) {
  Security sec;
  HANDLE h = nullptr;
  for (const char* scope : {"Global\\", "Local\\"}) {
    h = CreateEventW(&sec.sa, FALSE, FALSE, scoped(scope, name).c_str());
    if (h) break;
  }
  if (!h) {
    if (error) *error = last_error_text("CreateEvent");
    return std::nullopt;
  }
  NamedEvent e;
  e.handle_ = h;
  return e;
}

std::optional<NamedEvent> NamedEvent::open(const std::string& name, std::string* error) {
  HANDLE h = nullptr;
  for (const char* scope : {"Global\\", "Local\\"}) {
    h = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, scoped(scope, name).c_str());
    if (h) break;
  }
  if (!h) {
    if (error) *error = last_error_text("OpenEvent");
    return std::nullopt;
  }
  NamedEvent e;
  e.handle_ = h;
  return e;
}

void NamedEvent::remove(const std::string&) {}

void NamedEvent::signal() { SetEvent(static_cast<HANDLE>(handle_)); }

bool NamedEvent::wait(int timeout_ms) {
  return WaitForSingleObject(static_cast<HANDLE>(handle_), static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
}

uint32_t current_pid() { return GetCurrentProcessId(); }

bool process_alive(uint32_t pid) {
  if (pid == 0) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
}

std::string program_data_dir() {
  PWSTR path = nullptr;
  std::string out = "C:\\ProgramData";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path))) out = narrow(path);
  if (path) CoTaskMemFree(path);
  return out + "\\BounceAES Master";
}

bool ensure_directory(const std::string& path) {
  return CreateDirectoryW(widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool replace_file(const std::string& from, const std::string& to) {
  return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool copy_file(const std::string& from, const std::string& to) {
  return CopyFileW(widen(from).c_str(), widen(to).c_str(), FALSE) != 0;
}

std::string computer_name() {
  wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (!GetComputerNameW(buf, &n)) return {};
  return narrow(buf);
}

std::string executable_dir() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);  // truncated: long path
  }
  const auto sep = buf.find_last_of(L"\\/");
  if (sep == std::wstring::npos) return {};
  buf.resize(sep);
  return narrow(buf.c_str());
}

}  // namespace bam::platform
