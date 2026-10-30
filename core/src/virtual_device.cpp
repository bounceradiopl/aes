// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/virtual_device.hpp"

#include <cctype>

namespace bam {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return std::string(s);
}

bool starts_with(const std::string& s, std::string_view prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// "Speakers (Scream (WDM))" -> description "Speakers", adapter "Scream (WDM)"
bool split_name(std::string_view name, std::string* description, std::string* adapter) {
  if (name.empty() || name.back() != ')') return false;
  int depth = 0;
  for (size_t i = name.size(); i-- > 0;) {
    if (name[i] == ')') ++depth;
    if (name[i] == '(' && --depth == 0) {
      *description = trim(name.substr(0, i));
      *adapter = trim(name.substr(i + 1, name.size() - i - 2));
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<VirtualDevice> classify_virtual_device(std::string_view friendly_name) {
  std::string description, adapter;
  if (!split_name(friendly_name, &description, &adapter)) return std::nullopt;
  const std::string a = lower(adapter);

  if (a == "vb-audio virtual cable") return VirtualDevice{"VB-CABLE", "CABLE", a};
  if (a.size() == 16 && starts_with(a, "vb-audio cable ") && std::isalpha(static_cast<unsigned char>(a[15])))
    return VirtualDevice{"VB-CABLE", std::string("CABLE-") + static_cast<char>(std::toupper(static_cast<unsigned char>(a[15]))), a};
  if (a == "vb-audio hi-fi cable") return VirtualDevice{"VB-CABLE", "Hi-Fi Cable", a};
  if (a == "virtual audio cable" && !description.empty())
    return VirtualDevice{"Virtual Audio Cable", description, "vac:" + lower(description)};
  if (starts_with(a, "vb-audio voicemeeter")) return VirtualDevice{"Voicemeeter", {}, {}};
  if (a == "virtual audio driver" || a == "virtual mic driver") return VirtualDevice{"Virtual Audio Driver", {}, {}};
  if (starts_with(a, "steam streaming")) return VirtualDevice{"Steam Streaming", {}, {}};
  if (starts_with(a, "scream")) return VirtualDevice{"Scream", {}, {}};
  return std::nullopt;
}

}  // namespace bam
