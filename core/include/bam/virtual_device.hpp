// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace bam {

// A virtual sound card recognised by its Windows endpoint name ("CABLE Input (VB-Audio Virtual
// Cable)"): applications play into its render end and record from its capture end.
struct VirtualDevice {
  std::string family;    // "VB-CABLE", "Virtual Audio Cable", "Voicemeeter", "Virtual Audio Driver", ...
  std::string cable;     // short name of a loop cable ("CABLE-A", "Line 1"); empty when not a loop
  std::string pair_key;  // shared by the render and capture end of one loop cable; empty when not a loop
};

// Recognises the device by the adapter name in the trailing brackets, which stays when the user
// renames the endpoint in Windows. Case-insensitive.
std::optional<VirtualDevice> classify_virtual_device(std::string_view friendly_name);

}  // namespace bam
