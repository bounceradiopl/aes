// SPDX-License-Identifier: GPL-3.0-or-later
// Stereo Tool binding (spec §4): libStereoTool loaded at run time, only from the program folder
// (full paths, never a bare name; on Windows with the restricted DLL search of DynamicLibrary),
// only version 11.07, web interface and UECP bound to 127.0.0.1, and every call into the
// library guarded.
#include "bam/encoder/stereotool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#include <malloc.h>  // _resetstkoflw
#include <windows.h>
#endif

#include "bam/auth.hpp"  // random_hex: the web interface password and the JSON token
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
  st::GetStsValueFn get_sts_value = nullptr;
  st::CheckLicenseValidFn check_license_valid = nullptr;
  st::GetUnlicensedUsedFeaturesFn get_unlicensed_used_features = nullptr;
};

}  // namespace detail

namespace {

constexpr const char* kBindIp = "127.0.0.1";

// Instances abandoned after a structured exception (process-wide, see StereoToolInstance).
std::atomic<int> g_abandoned{0};

// Create3, Delete, LoadPreset, SavePreset, SetStsValue and GetStsValue of all instances, one at a
// time. Always taken outside guarded(): a structured exception unwinds no locals of the frames it
// leaves.
std::mutex g_api_mutex;

enum class Outcome { Ok, Exception, Crash };

#ifdef _MSC_VER
// MSVC builds also catch structured exceptions (access violation, ...) thrown inside the
// library. A C++ exception (SEH code 0xE06D7363) is left to the C++ handler of guarded(), which
// can read its message and knows the library unwound in order.
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

// Runs f (one call into the library); on failure a Polish reason.
template <typename F>
Outcome guarded(const char* function, F& f, std::string* reason) {
  try {
#ifdef _MSC_VER
    unsigned long code = 0;
    if (!seh_call(f, &code)) {
      // the stack guard page back, now that the handler is left (the instance is abandoned anyway)
      if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
      char hex[16];
      std::snprintf(hex, sizeof hex, "0x%08lX", code);
      *reason = std::string("awaria Stereo Tool w ") + function + " (wyjątek systemowy " + hex + ")";
      return Outcome::Crash;
    }
#else
    f();
#endif
    return Outcome::Ok;
  } catch (const std::exception& e) {
    *reason = std::string("Stereo Tool zgłosił wyjątek w ") + function + ": " + e.what();
  } catch (...) {
    *reason = std::string("Stereo Tool zgłosił nieznany wyjątek w ") + function;
  }
  return Outcome::Exception;
}

void replace_all(std::string& text, const std::string& what) {
  if (what.empty()) return;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 3)) text.replace(at, what.size(), "***");
}

// The key without its brackets ("<abc…>" -> "abc…"), when long enough to be told from other text
// (kStereoToolKeyInnerMin); empty otherwise.
std::string inner_key(const std::string& key) {
  if (key.size() < 2 || key.front() != '<' || key.back() != '>') return {};
  std::string inner = key.substr(1, key.size() - 2);
  return inner.size() >= kStereoToolKeyInnerMin ? inner : std::string();
}

// Password and token of the web interface: 20 and 32 hex digits from the OS CSPRNG.
constexpr size_t kWebPasswordBytes = 10;
constexpr size_t kJsonTokenBytes = 16;

template <typename Fn>
bool resolve(const platform::DynamicLibrary& lib, const char* name, Fn& fn) {
  fn = reinterpret_cast<Fn>(lib.symbol(name));
  return fn != nullptr;
}

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
}

bool file_exists(const std::string& utf8) {
  std::error_code ec;
  return std::filesystem::is_regular_file(fs_path(utf8), ec);
}

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += sep;
    s += v[i];
  }
  return s;
}

// 11070 -> "11.07 (11070)"
std::string version_text(int v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%d.%02d (%d)", v / 1000, (v % 1000) / 10, v);
  return buf;
}

using ApiPtr = std::shared_ptr<const detail::StereoToolApi>;

// `kept` receives every library that was opened: none is ever unloaded.
ApiPtr load(const std::vector<std::string>& dirs, std::vector<ApiPtr>& kept, StereoToolInfo& info) {
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
  kept.push_back(api);
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
  need("stereoTool_GetStsValue", api->get_sts_value);
  need("stereoTool_CheckLicenseValid", api->check_license_valid);
  need("stereoTool_GetUnlicensedUsedFeatures", api->get_unlicensed_used_features);
  if (missing) {
    info.error = "biblioteka Stereo Tool " + l.path() + " nie ma funkcji " + missing + " (potrzebna wersja " +
                 version_text(kStereoToolVersion) + ")";
    return nullptr;
  }
  // the parameter indices (stereotool_api.hpp) are those of 11.07: another version is not used
  std::string reason;
  auto version = [&] { api->version = api->get_software_version(); };
  Outcome outcome = guarded("stereoTool_GetSoftwareVersion", version, &reason);
  if (outcome != Outcome::Ok) {
    // a crash leaves the library's state unknown: as for an abandoned instance, nothing more is
    // created in it and it is not loaded again (stereotool_reload) until the service restarts
    if (outcome == Outcome::Crash) ++g_abandoned;
    info.error = reason;
    return nullptr;
  }
  info.version = api->version;
  if (api->version != kStereoToolVersion) {
    info.error = "nieobsługiwana wersja Stereo Tool " + version_text(api->version) + " w " + l.path() +
                 ": Bounce Encoder używa tylko wersji " + version_text(kStereoToolVersion);
    return nullptr;
  }
  // before any instance exists: the web interface and UECP servers listen on 127.0.0.1 only; a
  // library that cannot be told so is not used
  auto bind = [&] { api->set_bind_ip(kBindIp, kBindIp); };
  outcome = guarded("stereoTool_global_setBindIp", bind, &reason);
  if (outcome != Outcome::Ok) {
    if (outcome == Outcome::Crash) ++g_abandoned;  // see GetSoftwareVersion above
    info.error = reason;
    return nullptr;
  }
  info.available = true;
  info.path = l.path();
  return api;
}

// Loaded once per search-dir setting (stereotool_reload() retries a load that failed, which helps
// only when the library was missing or could not be opened). A library once opened is never
// unloaded (Stereo Tool may keep threads of its own running after its last instance): `kept` is
// never destroyed.
struct Cache {
  std::mutex mutex;
  bool dirs_set = false;
  std::vector<std::string> dirs;
  bool tried = false;
  ApiPtr api;
  StereoToolInfo info;
  std::vector<ApiPtr>* kept = new std::vector<ApiPtr>;
  std::atomic<bool> loading{false};  // load() runs (under mutex): stereotool_peek does not wait for it

  // absolute dirs only: a relative one would depend on the working directory
  std::vector<std::string> search_dirs() const {
    std::vector<std::string> out;
    if (dirs_set) {
      for (const auto& d : dirs)
        if (!d.empty() && fs_path(d).is_absolute()) out.push_back(d);
      return out;
    }
    const std::string exe = platform::executable_dir();
    if (!exe.empty() && fs_path(exe).is_absolute()) out.push_back(exe);
    return out;
  }
  void ensure_loaded() {
    if (tried) return;
    loading.store(true);
    api = load(search_dirs(), *kept, info);
    tried = true;
    loading.store(false);
  }
  StereoToolInfo current() const {
    StereoToolInfo i = info;
    i.abandoned = g_abandoned.load();
    i.checked = tried;
    return i;
  }
};

Cache& cache() {
  static Cache c;
  return c;
}

ApiPtr stereotool_api(StereoToolInfo* info) {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  c.ensure_loaded();
  if (info) *info = c.current();
  return c.api;
}

}  // namespace

std::string without_stereotool_key(std::string text, const std::string& key) {
  replace_all(text, key);
  replace_all(text, inner_key(key));
  return text;
}

StereoToolInfo stereotool_info() {
  StereoToolInfo info;
  stereotool_api(&info);
  return info;
}

StereoToolInfo stereotool_peek() {
  auto& c = cache();
  std::unique_lock<std::mutex> lock(c.mutex, std::defer_lock);
  // the mutex is held for a moment by everything else, but for the whole load (a 117 MB library)
  // by ensure_loaded(): then the answer is "loading", without waiting
  while (!lock.try_lock()) {
    if (c.loading.load()) {
      StereoToolInfo i;
      i.loading = true;
      i.error = "ładowanie biblioteki Stereo Tool…";
      i.abandoned = g_abandoned.load();
      return i;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return c.current();
}

StereoToolInfo stereotool_reload() {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  // a loaded library is kept; after an abandoned instance (or a crash while loading) nothing is
  // loaded again
  if (!c.api && g_abandoned.load() == 0) c.tried = false;
  c.ensure_loaded();
  return c.current();
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

std::vector<std::string> stereotool_library_candidates() {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  return detail::library_candidates(c.search_dirs(), stereotool_library_names());
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

namespace testing {
void set_stereotool_abandoned(int count) { g_abandoned.store(count); }
}  // namespace testing

// ---- StereoToolInstance ----

StereoToolInstance::StereoToolInstance(std::shared_ptr<const detail::StereoToolApi> api, gStereoTool* st)
    : api_(std::move(api)), st_(st) {}

std::unique_ptr<StereoToolInstance> StereoToolInstance::create(const std::string& key, const std::string& name,
                                                               const std::string& home_dir, std::string* error) {
  StereoToolInfo info;
  auto api = stereotool_api(&info);
  if (!api) {
    if (error) {
      *error = info.error;
      // after a crash (e.g. while loading) nothing is loaded again until the service restarts
      if (info.abandoned > 0) *error += " — uruchom ponownie usługę Bounce Encoder";
    }
    return nullptr;
  }
  const char* const crashed = "Stereo Tool uległ awarii — uruchom ponownie usługę Bounce Encoder (do tego czasu bez processingu)";
  if (g_abandoned.load() >= kStereoToolMaxAbandoned) {
    if (error) *error = crashed;
    return nullptr;
  }
  gStereoTool* st = nullptr;
  std::string reason;
  auto create3 = [&] {
    st = api->create3(false, key.empty() ? nullptr : key.c_str(), name.empty() ? nullptr : name.c_str(),
                      home_dir.empty() ? nullptr : home_dir.c_str(), false);
  };
  const std::string what = "nie udało się utworzyć instancji Stereo Tool" + (name.empty() ? "" : " " + name);
  Outcome outcome;
  std::unique_ptr<StereoToolInstance> instance;
  bool settings_ok = false;
  {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    // checked again here: while this thread waited for the mutex, a call of another instance
    // may have crashed (it counts g_abandoned without the mutex, Process for one)
    if (g_abandoned.load() >= kStereoToolMaxAbandoned) {
      if (error) *error = crashed;
      return nullptr;
    }
    outcome = guarded("stereoTool_Create3", create3, &reason);
    if (outcome == Outcome::Ok && st) {
      instance.reset(new StereoToolInstance(api, st));
      instance->key_ = key;
      instance->web_password_ = random_hex(kWebPasswordBytes);
      instance->json_token_ = random_hex(kJsonTokenBytes);
      // before anything else uses it: no network feature on, the password protection on
      settings_ok = instance->apply_settings();
    }
  }
  if (outcome != Outcome::Ok) {
    // no instance pointer came back: nothing to delete; after a crash the library's state is
    // unknown, as if an instance had been abandoned
    if (outcome == Outcome::Crash) ++g_abandoned;
    if (error) *error = what + ": " + without_stereotool_key(reason, key);
    return nullptr;
  }
  if (!st) {
    if (error) *error = what + " (zła wersja biblioteki albo za mało pamięci)";
    return nullptr;
  }
  if (!settings_ok) {
    if (error) *error = what + ": " + without_stereotool_key(instance->failure_, key);
    return nullptr;  // deleted here (outside the lock), or left alone when it crashed
  }
  return instance;
}

StereoToolInstance::~StereoToolInstance() {
  if (!st_ || abandoned_ || left_) return;
  // also after a C++ exception: the library unwound in order, so its ports and memory are freed
  std::string reason;
  auto destroy = [&] { api_->destroy(st_); };
  Outcome outcome;
  {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    outcome = guarded("stereoTool_Delete", destroy, &reason);
  }
  if (outcome == Outcome::Crash) ++g_abandoned;
}

template <typename F>
bool StereoToolInstance::call(const char* function, F&& f) {
  if (failed_) return false;
  std::string reason;
  const Outcome outcome = guarded(function, f, &reason);
  if (outcome == Outcome::Ok) return true;
  failed_ = true;
  failure_ = std::move(reason);
  if (outcome == Outcome::Crash) {
    abandoned_ = true;
    ++g_abandoned;
  }
  return false;
}

bool StereoToolInstance::load_preset(const std::string& sts_path, std::string* error) {
  bool ok = false;
  bool called;
  {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    called = call("stereoTool_LoadPreset", [&] { ok = api_->load_preset(st_, sts_path.c_str(), detail::st::kSaveAllSettings); });
    // a preset can switch network features on and carry web interface settings of its own: the
    // instance keeps ours, set before the lock is let go (nothing else runs it meanwhile)
    if (called && ok) apply_settings();
  }
  if (!called || failed_) {
    if (error) *error = failure_;
    return false;
  }
  if (!ok) {
    if (error) *error = "Stereo Tool nie wczytał presetu " + sts_path;
    return false;
  }
  return true;
}

bool StereoToolInstance::save_preset(const std::string& sts_path, std::string* error) {
  bool ok = false;
  bool called;
  std::string scrub_error;
  bool scrubbed = true;
  {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    called = call("stereoTool_SavePreset", [&] { ok = api_->save_preset(st_, sts_path.c_str(), detail::st::kSaveAllSettings); });
    if (called && ok) scrubbed = scrub_preset(sts_path, &scrub_error);
  }
  if (!called) {
    if (error) *error = failure_;
    return false;
  }
  if (!ok) {
    if (error) *error = "Stereo Tool nie zapisał presetu " + sts_path;
    return false;
  }
  if (!scrubbed) {
    if (error) *error = scrub_error;
    return false;
  }
  return true;
}

// The secrets out of a preset the library wrote: every line holding the web interface password,
// the JSON token or the licence key (also without its brackets) keeps its name and loses its
// value ("name=" ; a line without "=" goes). Rewritten in place, then read back: a file still
// holding one is removed.
bool StereoToolInstance::scrub_preset(const std::string& path, std::string* error) const {
  std::vector<std::string> secrets = {web_password_, json_token_, key_, inner_key(key_)};
  std::erase_if(secrets, [](const std::string& x) { return x.empty(); });
  auto holds_secret = [&](const std::string& text) {
    return std::any_of(secrets.begin(), secrets.end(), [&](const std::string& x) { return text.find(x) != std::string::npos; });
  };
  auto read = [&](std::string* text) {
    std::ifstream in(fs_path(path), std::ios::binary);
    if (!in) return false;
    text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
  };
  const std::string failed = "nie udało się usunąć hasła interfejsu WWW Stereo Tool z zapisanego presetu — preset nie został zapisany";
  std::string text;
  if (!read(&text)) {
    *error = failed;
    return false;
  }
  if (!holds_secret(text)) return true;
  std::string clean;
  clean.reserve(text.size());
  for (size_t begin = 0; begin < text.size();) {
    size_t end = text.find('\n', begin);
    end = end == std::string::npos ? text.size() : end + 1;
    std::string line = text.substr(begin, end - begin);
    begin = end;
    if (holds_secret(line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const bool crlf = line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0;
      const bool lf = !crlf && !line.empty() && line.back() == '\n';
      line = line.substr(0, eq + 1) + (crlf ? "\r\n" : lf ? "\n" : "");
      if (holds_secret(line)) continue;  // in the name itself
    }
    clean += line;
  }
  {
    std::ofstream out(fs_path(path), std::ios::binary | std::ios::trunc);
    out << clean;
  }
  if (!read(&text) || holds_secret(text) || text != clean) {
    std::error_code ec;
    std::filesystem::remove(fs_path(path), ec);
    *error = failed;
    return false;
  }
  return true;
}

bool StereoToolInstance::set_web_interface(bool enabled, int port, const std::string& password) {
  if (enabled && (port < 1 || port > 65535)) return false;
  if (failed_) return false;
  std::lock_guard<std::mutex> lock(g_api_mutex);
  web_enabled_ = enabled;
  if (enabled) web_port_ = port;
  if (!password.empty()) web_password_ = password;
  return apply_settings();
}

std::optional<std::string> StereoToolInstance::sts_value(int index) {
  std::lock_guard<std::mutex> lock(g_api_mutex);
  return get_sts(index);
}

bool StereoToolInstance::set_sts(int index, const std::string& value) {
  bool ok = false;
  return call("stereoTool_SetStsValue", [&] { ok = api_->set_sts_value(st_, index, 0, value.c_str()); }) && ok;
}

std::optional<std::string> StereoToolInstance::get_sts(int index) {
  std::optional<std::string> value;
  call("stereoTool_GetStsValue", [&] {
    const char* text = nullptr;
    if (api_->get_sts_value(st_, index, 0, &text) && text) value = std::string(text);  // copied at once
  });
  return value;
}

// One network switch off: true when it is off now, or the library has no such switch.
bool StereoToolInstance::switch_off(int index) {
  set_sts(index, "0");  // refused when the library has no such switch: then it reads none either
  if (failed_) return false;
  const auto value = get_sts(index);
  return !failed_ && (!value || *value == "0");
}

// Under g_api_mutex: every network switch off, the password protection, then the web interface.
// false: the web interface could not be set as asked (it is off), or the instance failed (a
// network feature stays on, or a call failed: failed_).
bool StereoToolInstance::apply_settings() {
  if (failed_) return false;
  for (const auto& sw : detail::st::kNetworkSwitches)
    for (int i = 0; i < sw.count; ++i) {
      if (switch_off(sw.first + i)) continue;
      if (!failed_) {
        failed_ = true;
        failure_ = std::string("Stereo Tool nie wyłączył funkcji sieciowej: ") + sw.what + " (parametr " +
                   std::to_string(sw.first + i) + ") — processing wyłączony, sprawdź preset";
      }
      return false;
    }
  namespace st = detail::st;
  // the account's password first, then the protection: never switched on with an empty password
  bool pw = set_sts(st::kParamPassword0, web_password_) && set_sts(st::kParamJsonToken, json_token_) &&
            set_sts(st::kParamPasswordProtection0, "1") && set_sts(st::kParamPasswordProtectionAny, "1");
  if (failed_) return false;
  pw = pw && get_sts(st::kParamPassword0) == web_password_ && get_sts(st::kParamPasswordProtectionAny) == std::optional<std::string>("1");
  if (failed_) return false;
  protected_ = pw;
  return apply_web_interface();
}

bool StereoToolInstance::apply_web_interface() {
  bool ok = true;
  if (web_enabled_ && !protected_) {
    web_enabled_ = false;  // never without its password
    ok = false;
  }
  if (web_enabled_) {
    // the port first: the server is switched on only once it is set (else it would start on
    // whatever port the library had)
    const bool port_ok = set_sts(detail::st::kParamWebserverPort, std::to_string(web_port_));
    if (failed_) return false;
    if (!port_ok) {
      web_enabled_ = false;
      ok = false;
    }
  }
  const bool enable_ok = set_sts(detail::st::kParamWebserverEnable, web_enabled_ ? "1" : "0");
  return ok && enable_ok;
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
