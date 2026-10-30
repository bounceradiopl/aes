// SPDX-License-Identifier: GPL-3.0-or-later
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "bam/platform/time.hpp"

namespace bam::platform {

int64_t monotonic_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

int64_t realtime_ns() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void sleep_ns(int64_t duration) {
  if (duration <= 0) return;
  timespec ts{static_cast<time_t>(duration / 1'000'000'000), static_cast<long>(duration % 1'000'000'000)};
  while (nanosleep(&ts, &ts) != 0) {
  }
}

void sleep_until_ns(int64_t target) { sleep_ns(target - monotonic_ns()); }

void set_realtime_priority(const char*) {
  sched_param p{};
  p.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);  // needs privileges; failure is fine
}

}  // namespace bam::platform
