// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAESService: runs as a Windows service, or in the foreground with --console (all platforms).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

#include "bam/constants.hpp"
#include "bam/engine.hpp"
#include "bam/http_server.hpp"
#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"

#ifdef _WIN32
#include "win_service.hpp"
#endif

namespace {

constexpr size_t kLogFileBytes = 10 * 1024 * 1024;
constexpr int kLogFiles = 5;

std::atomic<bool> g_console_stop{false};

#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

std::string default_config_path() {
#ifdef _WIN32
  return bam::platform::program_data_dir() + "\\config.json";
#else
  return "bounceaes-config.json";
#endif
}

std::string dir_of(const std::string& path) {
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string timestamp(int64_t unix_ms) {
  std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[40];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  char out[48];
  std::snprintf(out, sizeof(out), "%s.%03d", buf, static_cast<int>(unix_ms % 1000));
  return out;
}

// Rotating log file: bounceaes.log, bounceaes.log.1 … .4 (5 × 10 MB).
void install_file_sink(const std::string& dir) {
  bam::platform::ensure_directory(dir);
  const std::string path = dir + kSep + "bounceaes.log";
  auto mutex = std::make_shared<std::mutex>();
  bam::Logger::instance().add_sink([path, mutex](const bam::LogEntry& e) {
    std::lock_guard lock(*mutex);
    {
      std::ifstream in(path, std::ios::binary | std::ios::ate);
      if (in && static_cast<size_t>(in.tellg()) > kLogFileBytes) {
        in.close();
        std::remove((path + "." + std::to_string(kLogFiles - 1)).c_str());
        for (int i = kLogFiles - 2; i >= 1; --i)
          bam::platform::replace_file(path + "." + std::to_string(i), path + "." + std::to_string(i + 1));
        bam::platform::replace_file(path, path + ".1");
      }
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << timestamp(e.unix_ms) << " [" << bam::log_level_name(e.level) << "] " << e.message << "\n";
  });
}

int run(const std::string& config_path, std::atomic<bool>& stop) {
  bam::EngineOptions opts;
  opts.config_path = config_path;
  bam::Engine engine(opts);
  std::string err;
  if (!engine.start(&err)) {
    bam::log_error("Nie można uruchomić usługi: " + err);
    return 1;
  }
  bam::HttpServer http(engine);
  const auto cfg = engine.config();
  if (!http.start(cfg.http_bind, cfg.http_port, &err)) bam::log_error(err + " (audio działa dalej, panel niedostępny)");
  while (!stop) bam::platform::sleep_ns(100'000'000);
  http.stop();
  engine.stop();
  return 0;
}

void print_usage() {
  std::printf(
      "%s %s\n\n"
      "Użycie:\n"
      "  BounceAESService --console [--config plik.json]   uruchom na pierwszym planie (Ctrl+C kończy)\n"
      "  BounceAESService --version\n\n"
      "Bez argumentów program działa jako usługa Windows (uruchamiana przez instalator).\n",
      bam::kProductName, bam::kVersion);
}

}  // namespace

int main(int argc, char** argv) {
  bool console = false;
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--console") {
      console = true;
    } else if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--version") {
      std::printf("%s %s\n", bam::kProductName, bam::kVersion);
      return 0;
    } else {
      print_usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  if (config_path.empty()) config_path = default_config_path();
  install_file_sink(dir_of(config_path) + kSep + "logs");

#ifdef _WIN32
  if (!console) {
    bam::install_event_log_sink();
    if (bam::run_as_windows_service([&](std::atomic<bool>& stop) { return run(config_path, stop); })) return 0;
    print_usage();  // started by hand, not by the service control manager
    return 2;
  }
#else
  (void)console;  // POSIX builds always run in the foreground
#endif

  bam::Logger::instance().add_sink([](const bam::LogEntry& e) {
    std::fprintf(stderr, "%s [%s] %s\n", timestamp(e.unix_ms).c_str(), bam::log_level_name(e.level), e.message.c_str());
  });
  std::signal(SIGINT, [](int) { g_console_stop = true; });
  std::signal(SIGTERM, [](int) { g_console_stop = true; });
  return run(config_path, g_console_stop);
}
