// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <optional>
#include <string>

namespace bam::platform {

// Named shared memory. On Windows the name lives in Global\ (service <-> user processes) with a
// DACL that lets authenticated users read and write; Local\ is used when Global\ is not permitted.
class SharedMemory {
 public:
  SharedMemory() = default;
  SharedMemory(SharedMemory&& other) noexcept;
  SharedMemory& operator=(SharedMemory&& other) noexcept;
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;
  ~SharedMemory();

  static std::optional<SharedMemory> create(const std::string& name, size_t size, bool* existed, std::string* error);
  static std::optional<SharedMemory> open(const std::string& name, size_t size, std::string* error);
  static void remove(const std::string& name);  // POSIX: shm_unlink; no-op on Windows

  void* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  void release();
  void* data_ = nullptr;
  size_t size_ = 0;
  intptr_t handle_ = -1;
};

}  // namespace bam::platform
