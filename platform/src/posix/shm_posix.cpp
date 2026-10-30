// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>

#include "bam/platform/event.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"

namespace bam::platform {

namespace {
std::string posix_name(const std::string& name) { return "/" + name; }
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
  if (data_) munmap(data_, size_);
  if (handle_ >= 0) ::close(static_cast<int>(handle_));
  data_ = nullptr;
  handle_ = -1;
}

std::optional<SharedMemory> SharedMemory::create(const std::string& name, size_t size, bool* existed, std::string* error) {
  const std::string n = posix_name(name);
  bool was = false;
  int fd = shm_open(n.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0 && errno == EEXIST) {
    was = true;
    fd = shm_open(n.c_str(), O_RDWR, 0666);
  }
  if (fd < 0) {
    if (error) *error = std::string("shm_open: ") + std::strerror(errno);
    return std::nullopt;
  }
  struct stat st{};
  fstat(fd, &st);
  if (static_cast<size_t>(st.st_size) < size && ftruncate(fd, static_cast<off_t>(size)) != 0) {
    if (error) *error = std::string("ftruncate: ") + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    if (error) *error = std::string("mmap: ") + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  if (existed) *existed = was;
  SharedMemory m;
  m.data_ = p;
  m.size_ = size;
  m.handle_ = fd;
  return m;
}

std::optional<SharedMemory> SharedMemory::open(const std::string& name, size_t size, std::string* error) {
  int fd = shm_open(posix_name(name).c_str(), O_RDWR, 0666);
  if (fd < 0) {
    if (error) *error = std::string("shm_open: ") + std::strerror(errno);
    return std::nullopt;
  }
  struct stat st{};
  fstat(fd, &st);
  if (static_cast<size_t>(st.st_size) < size) {
    if (error) *error = "pamięć współdzielona ma nieprawidłowy rozmiar";
    ::close(fd);
    return std::nullopt;
  }
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    if (error) *error = std::string("mmap: ") + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  SharedMemory m;
  m.data_ = p;
  m.size_ = size;
  m.handle_ = fd;
  return m;
}

void SharedMemory::remove(const std::string& name) { shm_unlink(posix_name(name).c_str()); }

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
  if (handle_) sem_close(static_cast<sem_t*>(handle_));
  handle_ = nullptr;
}

std::optional<NamedEvent> NamedEvent::create(const std::string& name, std::string* error) {
  sem_t* s = sem_open(posix_name(name).c_str(), O_CREAT, 0666, 0);
  if (s == SEM_FAILED) {
    if (error) *error = std::string("sem_open: ") + std::strerror(errno);
    return std::nullopt;
  }
  NamedEvent e;
  e.handle_ = s;
  return e;
}

std::optional<NamedEvent> NamedEvent::open(const std::string& name, std::string* error) {
  sem_t* s = sem_open(posix_name(name).c_str(), 0);
  if (s == SEM_FAILED) {
    if (error) *error = std::string("sem_open: ") + std::strerror(errno);
    return std::nullopt;
  }
  NamedEvent e;
  e.handle_ = s;
  return e;
}

void NamedEvent::remove(const std::string& name) { sem_unlink(posix_name(name).c_str()); }

void NamedEvent::signal() {
  // Auto-reset semantics: never accumulate more than one pending signal.
  auto* s = static_cast<sem_t*>(handle_);
  while (sem_trywait(s) == 0) {
  }
  sem_post(s);
}

bool NamedEvent::wait(int timeout_ms) {
  auto* s = static_cast<sem_t*>(handle_);
  const int64_t deadline = monotonic_ns() + int64_t(timeout_ms) * 1'000'000;
  for (;;) {
    if (sem_trywait(s) == 0) return true;  // macOS has no sem_timedwait
    if (monotonic_ns() >= deadline) return false;
    sleep_ns(100'000);
  }
}

uint32_t current_pid() { return static_cast<uint32_t>(getpid()); }

bool process_alive(uint32_t pid) { return pid != 0 && (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM); }

std::string program_data_dir() { return "."; }

bool ensure_directory(const std::string& path) { return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; }

bool replace_file(const std::string& from, const std::string& to) { return ::rename(from.c_str(), to.c_str()) == 0; }

bool copy_file(const std::string& from, const std::string& to) {
  std::ifstream in(from, std::ios::binary);
  if (!in) return false;
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  out << in.rdbuf();
  return out.good();
}

std::string computer_name() {
  char buf[256] = {};
  if (gethostname(buf, sizeof(buf) - 1) != 0) return {};
  std::string s(buf);
  if (const auto dot = s.find('.'); dot != std::string::npos) s.resize(dot);
  return s;
}

}  // namespace bam::platform
