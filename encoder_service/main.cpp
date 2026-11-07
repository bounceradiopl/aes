// SPDX-License-Identifier: GPL-3.0-or-later
// BounceEncoder: the Bounce Encoder engine and its web panel. Runs as the Windows service
// "BounceEncoder" ("Bounce Encoder"), or in the foreground with --console (all platforms).
// Modelled on service/main.cpp (BounceAESService).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "bam/encoder/data_dir.hpp"
#include "bam/encoder/engine.hpp"
#include "bam/encoder/http_server.hpp"
#include "bam/encoder/paths.hpp"
#include "bam/encoder/version.hpp"
#include "bam/log.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"

#ifdef _WIN32
#include "win_service.hpp"
#endif

namespace {

namespace be = bam::encoder;

constexpr size_t kLogFileBytes = 10 * 1024 * 1024;
constexpr int kLogFiles = 5;

std::atomic<bool> g_console_stop{false};

#ifdef _WIN32
constexpr char kSep = '\\';
constexpr const wchar_t* kEncoderServiceName = L"BounceEncoder";
constexpr const wchar_t* kEncoderEventSource = L"Bounce Encoder";
#else
constexpr char kSep = '/';
#endif

std::string dir_of(const std::string& path) {
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

// %ProgramData%\Bounce Encoder\config.json (platform::program_data_dir() is the BounceAES Master
// folder inside %ProgramData%); POSIX: the current directory.
std::string default_config_path() {
#ifdef _WIN32
  return dir_of(bam::platform::program_data_dir()) + "\\Bounce Encoder\\config.json";
#else
  return "bounce-encoder-config.json";
#endif
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

// Rotating log file: bounce-encoder.log, bounce-encoder.log.1 … .4 (5 × 10 MB).
void install_file_sink(const std::string& dir) {
  bam::platform::ensure_directory(dir);
  const std::string path = dir + kSep + "bounce-encoder.log";
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

int run(const std::string& config_path, bool protect, const std::string& dir_error, std::atomic<bool>& stop) {
  bam::log_info(std::string(be::kEncoderProductName) + " " + be::kEncoderVersion + ", konfiguracja: " + config_path);
  if (!dir_error.empty()) bam::log_error("Folder danych: " + dir_error);
  const be::EncoderConfig cfg = be::load_startup_config(config_path);
  // Stereo Tool's folder next to config.json (preset_store.hpp), created with the same protection
  be::EncoderEngineOptions opts;
  opts.stereotool_dir = dir_of(config_path) + kSep + be::kStereoToolDirName;
  opts.protect_stereotool_dir = protect;
  opts.keep_stereotool_at_stop = true;  // engine.stop() below is the end of the process (ruling S7)
  be::EncoderEngine engine(opts);
  std::string err;
  if (!engine.start(cfg, &err)) {
    bam::log_error("Nie można uruchomić usługi: " + err);
    return 1;
  }
  // the tray reads the panel address from public\panel.json: config.json is for administrators and
  // the service only (paths.hpp)
  if (!be::write_panel_file(be::panel_file_path(config_path), cfg.http_bind, cfg.http_port, &err))
    bam::log_warning("Nie można zapisać adresu panelu dla ikony w zasobniku: " + err);
  be::EncoderHttpServer http(engine, config_path);
  if (!http.start(cfg.http_bind, static_cast<uint16_t>(cfg.http_port), &err))
    bam::log_error(err + " (strumienie działają dalej, panel niedostępny)");
  while (!stop) bam::platform::sleep_ns(100'000'000);
  http.stop();
  engine.stop();
  return 0;
}

void print_usage() {
  std::printf(
      "%s %s\n\n"
      "Użycie:\n"
      "  BounceEncoder --console [--config plik.json]   uruchom na pierwszym planie (Ctrl+C kończy)\n"
      "  BounceEncoder --version\n\n"
      "Bez argumentów program działa jako usługa Windows \"Bounce Encoder\" (uruchamiana przez instalator).\n",
      be::kEncoderProductName, be::kEncoderVersion);
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
      std::printf("%s %s\n", be::kEncoderProductName, be::kEncoderVersion);
      return 0;
    } else {
      print_usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  if (config_path.empty()) config_path = default_config_path();
  // The service's own folder (deleted after the installation, say) is created with the installer's
  // protected ACL, never with the one it would inherit from %ProgramData%; a --config elsewhere is
  // created as any folder. CreateDirectory does not create parents.
#ifdef _WIN32
  const bool protect = config_path == default_config_path();
#else
  const bool protect = false;
#endif
  std::string dir_error;
  be::create_data_dirs(be::data_dirs(config_path, protect), &dir_error);
  install_file_sink(dir_of(config_path) + kSep + "logs");

#ifdef _WIN32
  if (!console) {
    bam::install_event_log_sink(kEncoderEventSource);
    if (bam::run_as_windows_service([&](std::atomic<bool>& stop) { return run(config_path, protect, dir_error, stop); }, kEncoderServiceName))
      return 0;
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
  return run(config_path, protect, dir_error, g_console_stop);
}
