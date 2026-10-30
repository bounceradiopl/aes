// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace bam::platform {

int64_t monotonic_ns();  // QueryPerformanceCounter / CLOCK_MONOTONIC
int64_t realtime_ns();   // Unix epoch
void sleep_until_ns(int64_t monotonic_target);  // high-resolution timer where available
void sleep_ns(int64_t duration);
// Raise the calling thread for audio work: MMCSS task (e.g. "Pro Audio") on Windows, best effort elsewhere.
void set_realtime_priority(const char* mmcss_task);

}  // namespace bam::platform
