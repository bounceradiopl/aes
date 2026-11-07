// SPDX-License-Identifier: GPL-3.0-or-later
// Stereo Tool's folder: the presets and the stations' home folders (preset_store.hpp).
#include "bam/encoder/preset_store.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <io.h>  // _commit, _fileno
#else
#include <unistd.h>  // fsync, fileno
#endif

#include "bam/encoder/config.hpp"
#include "bam/encoder/data_dir.hpp"

namespace bam::encoder {

namespace fs = std::filesystem;

namespace {

constexpr const char* kTempPrefix = ".bounce-";  // never a valid preset name: does not end in ".sts"
constexpr const char* kTempSuffix = ".tmp";

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

fs::path fs_path(const std::string& utf8) { return fs::path(reinterpret_cast<const char8_t*>(utf8.c_str())); }

std::string utf8(const fs::path& p) {
  const auto u8 = p.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string join(const std::string& dir, std::string_view name) { return utf8(fs_path(dir) / fs_path(std::string(name))); }

bool make_dir(const std::string& path, bool protect, std::string* error) {
  return create_data_dirs({{path, protect ? kDataDirSddl : ""}}, error);
}

// The names in `dir` that are presets (valid names, regular files).
std::vector<std::pair<std::string, uint64_t>> scan(const std::string& dir) {
  std::vector<std::pair<std::string, uint64_t>> out;
  std::error_code ec;
  for (fs::directory_iterator it(fs_path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8(it->path().filename());
    std::error_code fec;
    if (!valid_preset_name(name) || !it->is_regular_file(fec)) continue;
    const auto size = it->file_size(fec);
    out.emplace_back(name, fec ? 0 : uint64_t(size));
  }
  return out;
}

std::FILE* open_file(const std::string& path, bool write) {
#ifdef _WIN32
  return _wfopen(fs_path(path).c_str(), write ? L"wb" : L"r+b");
#else
  return std::fopen(path.c_str(), write ? "wb" : "r+b");
#endif
}

// Flushed to the disk, so a power cut after the rename never leaves an empty preset (like
// save_config_file). `data` null: the file as it is (written by Stereo Tool).
bool write_synced(const std::string& path, const std::string_view* data) {
  std::FILE* f = open_file(path, data != nullptr);
  if (!f) return false;
  bool ok = true;
  if (data) ok = std::fwrite(data->data(), 1, data->size(), f) == data->size();
  ok = ok && std::fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  return std::fclose(f) == 0 && ok;
}

PresetResult fail(PresetError e, std::string message) { return PresetResult{e, std::move(message)}; }

std::string in_quotes(std::string_view name) { return "„" + std::string(name) + "”"; }

PresetResult not_found(std::string_view name) {
  // a name that is not a preset name is not quoted (it may be anything)
  return fail(PresetError::NotFound, valid_preset_name(name) ? "nie ma presetu " + in_quotes(name) : "nie ma takiego presetu");
}

}  // namespace

bool same_preset_name(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

PresetStore::PresetStore(std::string root, bool protect) : root_(std::move(root)), protect_(protect) {}

std::string PresetStore::presets_dir() const { return root_.empty() ? std::string() : join(root_, kPresetsDirName); }

bool PresetStore::prepare(std::string* error) {
  if (root_.empty()) {
    if (error) *error = "brak folderu Stereo Tool";
    return false;
  }
  std::lock_guard lock(m_);
  if (!make_dir(root_, protect_, error) || !make_dir(presets_dir(), protect_, error)) return false;
  // temporary files of uploads and saves that never finished (the service stopped meanwhile)
  std::error_code ec;
  for (fs::directory_iterator it(fs_path(presets_dir()), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = utf8(it->path().filename());
    if (name.starts_with(kTempPrefix) && name.ends_with(kTempSuffix)) {
      std::error_code rec;
      fs::remove(it->path(), rec);
    }
  }
  return true;
}

std::string PresetStore::station_home(const std::string& station_id, std::string* error) {
  if (root_.empty()) {
    if (error) *error = "brak folderu Stereo Tool";
    return {};
  }
  const std::string home = join(root_, kStationHomePrefix + station_id);
  std::lock_guard lock(m_);
  if (!make_dir(root_, protect_, error) || !make_dir(home, protect_, error)) return {};
  return home;
}

std::string PresetStore::find_locked(std::string_view name) const {
  if (root_.empty() || !valid_preset_name(name)) return {};
  for (const auto& [stored, bytes] : scan(presets_dir()))
    if (same_preset_name(stored, name)) return stored;
  return {};
}

size_t PresetStore::count_locked() const { return root_.empty() ? 0 : scan(presets_dir()).size(); }

std::vector<PresetInfo> PresetStore::list() const {
  std::lock_guard lock(m_);
  std::vector<PresetInfo> out;
  if (root_.empty()) return out;
  for (auto& [name, bytes] : scan(presets_dir())) out.push_back({std::move(name), bytes});
  std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) {
    return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
  });
  return out;
}

std::string PresetStore::path_of(std::string_view name) const {
  std::lock_guard lock(m_);
  if (root_.empty() || !valid_preset_name(name)) return {};
  const std::string stored = find_locked(name);
  return join(presets_dir(), stored.empty() ? name : std::string_view(stored));
}

PresetResult PresetStore::check_new_locked(std::string_view name) const {
  if (root_.empty()) return fail(PresetError::Failed, "brak folderu presetów Stereo Tool");
  if (!valid_preset_name(name))
    return fail(PresetError::BadName, "nazwa presetu: do " + std::to_string(kMaxPresetNameChars) +
                                          " znaków (litery, cyfry, spacja, _ . -), zakończona .sts");
  if (const std::string stored = find_locked(name); !stored.empty())
    return fail(PresetError::Exists, "preset " + in_quotes(stored) + " już jest (wielkość liter nie ma znaczenia)");
  if (count_locked() >= kMaxPresets)
    return fail(PresetError::TooMany, "najwyżej " + std::to_string(kMaxPresets) + " presetów: usuń nieużywany");
  return {};
}

std::string PresetStore::temp_path_locked() {
  return join(presets_dir(), kTempPrefix + std::to_string(++temp_counter_) + kTempSuffix);
}

PresetResult PresetStore::put(std::string_view name, std::string_view data) {
  if (data.empty()) return fail(PresetError::Empty, "pusty plik presetu");
  if (data.size() > kMaxPresetBytes)
    return fail(PresetError::TooLarge, "preset większy niż " + std::to_string(kMaxPresetBytes / (1024 * 1024)) + " MB");
  std::lock_guard lock(m_);
  if (auto r = check_new_locked(name); !r.ok()) return r;
  if (std::string err; !make_dir(root_, protect_, &err) || !make_dir(presets_dir(), protect_, &err))
    return fail(PresetError::Failed, err);
  // written next to it, then renamed: a preset is never seen half written
  const std::string tmp = temp_path_locked();
  if (!write_synced(tmp, &data)) {
    std::error_code ec;
    fs::remove(fs_path(tmp), ec);
    return fail(PresetError::Failed, "nie udało się zapisać presetu " + in_quotes(name));
  }
  std::error_code ec;
  fs::rename(fs_path(tmp), fs_path(join(presets_dir(), name)), ec);
  if (ec) {
    fs::remove(fs_path(tmp), ec);
    return fail(PresetError::Failed, "nie udało się zapisać presetu " + in_quotes(name));
  }
  return {};
}

PresetResult PresetStore::get(std::string_view name, std::string* data) const {
  std::lock_guard lock(m_);
  const std::string stored = find_locked(name);
  if (stored.empty()) return not_found(name);
  std::ifstream f(fs_path(join(presets_dir(), stored)), std::ios::binary);
  std::string text;
  char buf[65536];
  while (f && text.size() <= kMaxPresetBytes) {
    f.read(buf, sizeof buf);
    text.append(buf, size_t(f.gcount()));
  }
  if (f.bad() || text.size() > kMaxPresetBytes) return fail(PresetError::Failed, "nie udało się odczytać presetu " + in_quotes(stored));
  if (data) *data = std::move(text);
  return {};
}

PresetResult PresetStore::remove(std::string_view name) {
  std::lock_guard lock(m_);
  const std::string stored = find_locked(name);
  if (stored.empty()) return not_found(name);
  std::error_code ec;
  if (!fs::remove(fs_path(join(presets_dir(), stored)), ec) || ec)
    return fail(PresetError::Failed, "nie udało się usunąć presetu " + in_quotes(stored));
  return {};
}

PresetResult PresetStore::reserve(std::string_view name, std::string* temp_path) {
  std::lock_guard lock(m_);
  if (auto r = check_new_locked(name); !r.ok()) return r;
  if (std::string err; !make_dir(root_, protect_, &err) || !make_dir(presets_dir(), protect_, &err))
    return fail(PresetError::Failed, err);
  *temp_path = temp_path_locked();
  return {};
}

PresetResult PresetStore::commit(std::string_view name, const std::string& temp_path) {
  std::lock_guard lock(m_);
  std::error_code ec;
  const auto size = fs::file_size(fs_path(temp_path), ec);
  PresetResult r;
  if (ec || size == 0)
    r = fail(PresetError::Failed, "Stereo Tool nie zapisał presetu " + in_quotes(name));
  else if (size > kMaxPresetBytes)
    r = fail(PresetError::TooLarge, "preset większy niż " + std::to_string(kMaxPresetBytes / (1024 * 1024)) + " MB");
  else if (!write_synced(temp_path, nullptr))
    r = fail(PresetError::Failed, "nie udało się zapisać presetu " + in_quotes(name));
  else
    r = check_new_locked(name);  // an upload of that name may have come meanwhile
  if (r.ok()) {
    fs::rename(fs_path(temp_path), fs_path(join(presets_dir(), name)), ec);
    if (!ec) return r;
    r = fail(PresetError::Failed, "nie udało się zapisać presetu " + in_quotes(name));
  }
  fs::remove(fs_path(temp_path), ec);
  return r;
}

void PresetStore::abandon(const std::string& temp_path) {
  std::lock_guard lock(m_);
  std::error_code ec;
  fs::remove(fs_path(temp_path), ec);
}

}  // namespace bam::encoder
