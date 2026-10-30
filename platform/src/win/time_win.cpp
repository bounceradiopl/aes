// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>

#include <cstring>
#include <string>

#include "bam/platform/time.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace bam::platform {

namespace {
int64_t qpc_frequency() {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    timeBeginPeriod(1);  // improves Sleep()/fallback timer granularity for the whole process
    return li.QuadPart;
  }();
  return f;
}

HANDLE thread_timer() {
  thread_local HANDLE timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // pre-1803 Windows
    return h;
  }();
  return timer;
}
}  // namespace

int64_t monotonic_ns() {
  const int64_t f = qpc_frequency();
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  const int64_t c = li.QuadPart;
  return (c / f) * 1'000'000'000 + (c % f) * 1'000'000'000 / f;
}

int64_t realtime_ns() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  const int64_t t = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601
  return (t - 116444736000000000LL) * 100;
}

void sleep_ns(int64_t duration) {
  if (duration <= 0) return;
  HANDLE timer = thread_timer();
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -(duration / 100);
    if (due.QuadPart == 0) due.QuadPart = -1;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  Sleep(static_cast<DWORD>((duration + 999'999) / 1'000'000));
}

void sleep_until_ns(int64_t target) { sleep_ns(target - monotonic_ns()); }

void set_realtime_priority(const char* mmcss_task) {
  DWORD index = 0;
  std::wstring task = mmcss_task ? std::wstring(mmcss_task, mmcss_task + strlen(mmcss_task)) : L"Pro Audio";
  HANDLE h = AvSetMmThreadCharacteristicsW(task.c_str(), &index);
  if (h) {
    AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
  } else {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }
}

}  // namespace bam::platform
