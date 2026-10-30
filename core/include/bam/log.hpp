// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace bam {

enum class LogLevel { Info, Warning, Error };

struct LogEntry {
  int64_t unix_ms = 0;
  LogLevel level = LogLevel::Info;
  std::string message;
};

const char* log_level_name(LogLevel l);

// Process-wide log: keeps the last 500 entries for the panel and forwards each entry to sinks
// (console, rotating file, Windows Event Log).
class Logger {
 public:
  static Logger& instance();
  void write(LogLevel level, std::string message);
  std::vector<LogEntry> recent(size_t max = 500) const;
  void add_sink(std::function<void(const LogEntry&)> sink);
  void clear_sinks();

 private:
  mutable std::mutex mutex_;
  std::deque<LogEntry> entries_;
  std::vector<std::function<void(const LogEntry&)>> sinks_;
};

void log_info(std::string message);
void log_warning(std::string message);
void log_error(std::string message);

}  // namespace bam
