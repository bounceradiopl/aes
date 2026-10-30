// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace bam::platform {

uint32_t current_pid();
bool process_alive(uint32_t pid);
std::string program_data_dir();  // Windows: %ProgramData%\BounceAES Master; POSIX: current directory
bool ensure_directory(const std::string& path);
// Atomically replaces `to` with `from` (rename over an existing file).
bool replace_file(const std::string& from, const std::string& to);

}  // namespace bam::platform
