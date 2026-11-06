// SPDX-License-Identifier: GPL-3.0-or-later
// Stereo Tool binding (spec §4): libStereoTool loaded at run time, only from the program folder
// (full paths, never a bare name; on Windows with the restricted DLL search of DynamicLibrary),
// web interface and UECP bound to 127.0.0.1, and every call into the library guarded.
#include "bam/encoder/stereotool.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>

#ifdef _MSC_VER
#include <windows.h>
#endif

#include "bam/platform/dynlib.hpp"
#include "bam/platform/process.hpp"
#include "codec_impl.hpp"
#include "stereotool_api.hpp"

namespace bam::encoder {

namespace detail {

struct StereoToolApi {
  platform::DynamicLibrary lib;
  int version = 0;
  st::GlobalSetBindIpFn set_bind_ip = nullptr;
  st::GetSoftwareVersionFn get_software_version = nullptr;
  st::Create3Fn create3 = nullptr;
  st::DeleteFn destroy = nullptr;
  st::ProcessFn process = nullptr;
  st::GetLatency2Fn get_latency2 = nullptr;
  st::LoadPresetFn load_preset = nullptr;
  st::SavePresetFn save_preset = nullptr;
  st::SetStsValueFn set_sts_value = nullptr;
  st::CheckLicenseValidFn check_license_valid = nullptr;
  st::GetUnlicensedUsedFeaturesFn get_unlicensed_used_features = nullptr;
};

}  // namespace detail

namespace {

constexpr const char* kBindIp = "127.0.0.1";

#ifdef _MSC_VER
// MSVC builds also catch structured exceptions (access violation, ...) thrown inside the
// library. A C++ exception (SEH code 0xE06D7363) is left to the C++ handler of guarded(), which
// can read its message.
constexpr unsigned long kMsvcCppException = 0xE06D7363;

int seh_filter(unsigned long code) {
  return code == kMsvcCppException ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}

template <typename F>
bool seh_call(F& f, unsigned long* code) {
  __try {
    f();
    return true;
  } __except (seh_filter(GetExceptionCode())) {
    *code = GetExceptionCode();
    return false;
  }
}
#endif

// Runs f (one call into the library). false and a Polish reason when it threw.
template <typename F>
bool guarded(const char* function, F& f, std::string* reason) {
  try {
#ifdef _MSC_VER
    unsigned long code = 0;
    if (!seh_call(f, &code)) {
      char hex[16];
      std::snprintf(hex, sizeof hex, "0x%08lX", code);
      *reason = std::string("awaria Stereo Tool w ") + function + " (wyjątek systemowy " + hex + ")";
      return false;
    }
#else
    f();
#endif
    return true;
  } catch (const std::exception& e) {
    *reason = std::string("Stereo Tool zgłosił wyjątek w ") + function + ": " + e.what();
  } catch (...) {
    *reason = std::string("Stereo Tool zgłosił nieznany wyjątek w ") + function;
  }
  return false;
}

// A message that may come from the library must not carry the licence key.
std::string without_key(std::string text, const std::string& key) {
  if (key.empty()) return text;
  for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 3)) text.replace(at, key.size(), "***");
  return text;
}

template <typename Fn>
bool resolve(const platform::DynamicLibrary& lib, const char* name, Fn& fn) {
  fn = reinterpret_cast<Fn>(lib.symbol(name));
  return fn != nullptr;
}

bool file_exists(const std::string& utf8) {
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str())), ec);
}

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += sep;
    s += v[i];
  }
  return s;
}

std::shared_ptr<const detail::StereoToolApi> load(const std::vector<std::string>& dirs, StereoToolInfo& info) {
  info = {};
  const auto names = stereotool_library_names();
  if (names.empty()) {
    info.error = "Stereo Tool nie jest dostępny dla tej platformy";
    return nullptr;
  }
  const auto candidates = detail::library_candidates(dirs, names);
  bool present = false;
  for (const auto& c : candidates) present = present || file_exists(c);
  if (!present) {
    info.error = "brak biblioteki Stereo Tool (" + join(names, ", ") + ") w folderze programu: " +
                 (dirs.empty() ? std::string("nieznany folder programu") : join(dirs, "; "));
    return nullptr;
  }
  std::string err;
  auto lib = platform::DynamicLibrary::open(candidates, &err);
  if (!lib) {
    info.error = "nie udało się wczytać biblioteki Stereo Tool: " + err;
    return nullptr;
  }
  auto api = std::make_shared<detail::StereoToolApi>();
  api->lib = std::move(*lib);
  const auto& l = api->lib;
  const char* missing = nullptr;
  auto need = [&](const char* name, auto& fn) {
    if (!missing && !resolve(l, name, fn)) missing = name;
  };
  need("stereoTool_global_setBindIp", api->set_bind_ip);
  need("stereoTool_GetSoftwareVersion", api->get_software_version);
  need("stereoTool_Create3", api->create3);
  need("stereoTool_Delete", api->destroy);
  need("stereoTool_Process", api->process);
  need("stereoTool_GetLatency2", api->get_latency2);
  need("stereoTool_LoadPreset", api->load_preset);
  need("stereoTool_SavePreset", api->save_preset);
  need("stereoTool_SetStsValue", api->set_sts_value);
  need("stereoTool_CheckLicenseValid", api->check_license_valid);
  need("stereoTool_GetUnlicensedUsedFeatures", api->get_unlicensed_used_features);
  if (missing) {
    info.error = "biblioteka Stereo Tool " + l.path() + " nie ma funkcji " + missing + " (potrzebna wersja 11.07)";
    return nullptr;
  }
  // before any instance exists: the web interface and UECP servers listen on 127.0.0.1 only; a
  // library that cannot be told so is not used
  std::string reason;
  auto bind = [&] { api->set_bind_ip(kBindIp, kBindIp); };
  auto version = [&] { api->version = api->get_software_version(); };
  if (!guarded("stereoTool_global_setBindIp", bind, &reason) ||
      !guarded("stereoTool_GetSoftwareVersion", version, &reason)) {
    info.error = reason;
    return nullptr;
  }
  info.available = true;
  info.path = l.path();
  info.version = api->version;
  return api;
}

// Loaded once per search-dir setting. A library once loaded is never unloaded (Stereo Tool may
// keep threads of its own running after its last instance): `kept` is never destroyed.
struct Cache {
  std::mutex mutex;
  bool dirs_set = false;
  std::vector<std::string> dirs;
  bool tried = false;
  std::shared_ptr<const detail::StereoToolApi> api;
  StereoToolInfo info;
  std::vector<std::shared_ptr<const detail::StereoToolApi>>* kept =
      new std::vector<std::shared_ptr<const detail::StereoToolApi>>;

  std::vector<std::string> search_dirs() const {
    if (dirs_set) return dirs;
    const std::string exe = platform::executable_dir();
    return exe.empty() ? std::vector<std::string>{} : std::vector<std::string>{exe};
  }
  void ensure_loaded() {
    if (tried) return;
    api = load(search_dirs(), info);
    if (api) kept->push_back(api);
    tried = true;
  }
};

Cache& cache() {
  static Cache c;
  return c;
}

std::shared_ptr<const detail::StereoToolApi> stereotool_api(StereoToolInfo* info) {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  c.ensure_loaded();
  if (info) *info = c.info;
  return c.api;
}

}  // namespace

StereoToolInfo stereotool_info() {
  StereoToolInfo info;
  stereotool_api(&info);
  return info;
}

std::vector<std::string> stereotool_library_names() {
#if defined(_WIN64)
  return {"libStereoTool_64.dll"};
#elif defined(_WIN32)
  return {};  // the encoder ships 64-bit only
#elif defined(__APPLE__)
  return {"libStereoTool_64.dylib"};  // universal (x86_64 + arm64)
#elif defined(__x86_64__)
  return {"libStereoTool_intel64.so"};
#elif defined(__aarch64__)
  return {"libStereoTool_noX11_arm64.so", "libStereoTool_arm64.so"};
#else
  return {};
#endif
}

void set_stereotool_library_dirs(std::vector<std::string> dirs) {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  c.dirs_set = !dirs.empty();
  c.dirs = std::move(dirs);
  c.tried = false;
  c.api.reset();
  c.info = {};
}

// ---- StereoToolInstance ----

StereoToolInstance::StereoToolInstance(std::shared_ptr<const detail::StereoToolApi> api, gStereoTool* st)
    : api_(std::move(api)), st_(st) {}

std::unique_ptr<StereoToolInstance> StereoToolInstance::create(const std::string& key, const std::string& name,
                                                               const std::string& home_dir, std::string* error) {
  StereoToolInfo info;
  auto api = stereotool_api(&info);
  if (!api) {
    if (error) *error = info.error;
    return nullptr;
  }
  gStereoTool* st = nullptr;
  std::string reason;
  auto create3 = [&] {
    st = api->create3(false, key.empty() ? nullptr : key.c_str(), name.empty() ? nullptr : name.c_str(),
                      home_dir.empty() ? nullptr : home_dir.c_str(), false);
  };
  const std::string what = "nie udało się utworzyć instancji Stereo Tool" + (name.empty() ? "" : " " + name);
  if (!guarded("stereoTool_Create3", create3, &reason)) {
    // whatever the library did, no instance pointer came back: nothing to delete
    if (error) *error = what + ": " + without_key(reason, key);
    return nullptr;
  }
  if (!st) {
    if (error) *error = what + " (zła wersja biblioteki albo za mało pamięci)";
    return nullptr;
  }
  return std::unique_ptr<StereoToolInstance>(new StereoToolInstance(std::move(api), st));
}

StereoToolInstance::~StereoToolInstance() {
  if (!st_ || failed_) return;  // a failed instance is abandoned, not deleted
  std::string reason;
  auto destroy = [&] { api_->destroy(st_); };
  guarded("stereoTool_Delete", destroy, &reason);
}

template <typename F>
bool StereoToolInstance::call(const char* function, F&& f) {
  if (failed_) return false;
  std::string reason;
  if (guarded(function, f, &reason)) return true;
  failed_ = true;
  failure_ = std::move(reason);
  return false;
}

bool StereoToolInstance::load_preset(const std::string& sts_path, std::string* error) {
  bool ok = false;
  if (!call("stereoTool_LoadPreset", [&] { ok = api_->load_preset(st_, sts_path.c_str(), detail::st::kSaveAllSettings); })) {
    if (error) *error = failure_;
    return false;
  }
  if (!ok) {
    if (error) *error = "Stereo Tool nie wczytał presetu " + sts_path;
    return false;
  }
  // a preset can carry web interface settings of its own; the instance keeps ours
  if (web_set_) apply_web_interface();
  if (failed_) {
    if (error) *error = failure_;
    return false;
  }
  return true;
}

bool StereoToolInstance::save_preset(const std::string& sts_path, std::string* error) {
  bool ok = false;
  if (!call("stereoTool_SavePreset", [&] { ok = api_->save_preset(st_, sts_path.c_str(), detail::st::kSaveAllSettings); })) {
    if (error) *error = failure_;
    return false;
  }
  if (!ok) {
    if (error) *error = "Stereo Tool nie zapisał presetu " + sts_path;
    return false;
  }
  return true;
}

bool StereoToolInstance::set_web_interface(bool enabled, int port) {
  if (enabled && (port < 1 || port > 65535)) return false;
  if (failed_) return false;
  web_set_ = true;
  web_enabled_ = enabled;
  if (enabled) web_port_ = port;
  return apply_web_interface();
}

bool StereoToolInstance::apply_web_interface() {
  bool port_ok = true, enable_ok = false;
  if (web_enabled_) {
    // the port first, so the server starts on it
    const std::string port = std::to_string(web_port_);
    call("stereoTool_SetStsValue", [&] { port_ok = api_->set_sts_value(st_, detail::st::kParamWebserverPort, 0, port.c_str()); });
  }
  const char* on = web_enabled_ ? "1" : "0";
  call("stereoTool_SetStsValue", [&] { enable_ok = api_->set_sts_value(st_, detail::st::kParamWebserverEnable, 0, on); });
  return !failed_ && port_ok && enable_ok;
}

bool StereoToolInstance::process(float* stereo, int32_t frames) {
  if (failed_ || !stereo || frames < 0) return false;
  if (frames == 0) return true;
  return call("stereoTool_Process", [&] { api_->process(st_, stereo, frames, kStereoToolChannels, kStereoToolRate); });
}

int StereoToolInstance::latency_frames() {
  int frames = 0;
  if (!call("stereoTool_GetLatency2", [&] { frames = api_->get_latency2(st_, kStereoToolRate, true); })) return 0;
  return frames < 0 ? 0 : frames;
}

bool StereoToolInstance::license_valid() {
  bool valid = false;
  return call("stereoTool_CheckLicenseValid", [&] { valid = api_->check_license_valid(st_); }) && valid;
}

std::string StereoToolInstance::unlicensed_features() {
  char text[4096] = {};
  bool none = true;
  if (!call("stereoTool_GetUnlicensedUsedFeatures",
            [&] { none = api_->get_unlicensed_used_features(st_, text, int(sizeof text)); }))
    return {};
  text[sizeof text - 1] = '\0';
  return none ? std::string() : std::string(text);
}

}  // namespace bam::encoder
