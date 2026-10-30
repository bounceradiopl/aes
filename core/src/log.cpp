// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/log.hpp"

#include <chrono>

namespace bam {

namespace {
constexpr size_t kKeep = 500;
}

const char* log_level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warning:
      return "OSTRZEŻENIE";
    case LogLevel::Error:
      return "BŁĄD";
  }
  return "?";
}

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::write(LogLevel level, std::string message) {
  LogEntry e;
  e.unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  e.level = level;
  e.message = std::move(message);
  std::vector<std::function<void(const LogEntry&)>> sinks;
  {
    std::lock_guard lock(mutex_);
    entries_.push_back(e);
    while (entries_.size() > kKeep) entries_.pop_front();
    sinks = sinks_;
  }
  for (auto& s : sinks) s(e);
}

std::vector<LogEntry> Logger::recent(size_t max) const {
  std::lock_guard lock(mutex_);
  const size_t n = std::min(max, entries_.size());
  return std::vector<LogEntry>(entries_.end() - static_cast<std::ptrdiff_t>(n), entries_.end());
}

void Logger::add_sink(std::function<void(const LogEntry&)> sink) {
  std::lock_guard lock(mutex_);
  sinks_.push_back(std::move(sink));
}

void Logger::clear_sinks() {
  std::lock_guard lock(mutex_);
  sinks_.clear();
}

void log_info(std::string m) { Logger::instance().write(LogLevel::Info, std::move(m)); }
void log_warning(std::string m) { Logger::instance().write(LogLevel::Warning, std::move(m)); }
void log_error(std::string m) { Logger::instance().write(LogLevel::Error, std::move(m)); }

}  // namespace bam
