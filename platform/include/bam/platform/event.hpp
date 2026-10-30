// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>

namespace bam::platform {

// Named auto-reset event shared between processes (period boundary signal for the ASIO driver).
class NamedEvent {
 public:
  NamedEvent() = default;
  NamedEvent(NamedEvent&& other) noexcept;
  NamedEvent& operator=(NamedEvent&& other) noexcept;
  NamedEvent(const NamedEvent&) = delete;
  NamedEvent& operator=(const NamedEvent&) = delete;
  ~NamedEvent();

  static std::optional<NamedEvent> create(const std::string& name, std::string* error);
  static std::optional<NamedEvent> open(const std::string& name, std::string* error);
  static void remove(const std::string& name);  // POSIX: sem_unlink; no-op on Windows

  void signal();
  bool wait(int timeout_ms);  // true when signalled

 private:
  void release();
  void* handle_ = nullptr;
};

}  // namespace bam::platform
