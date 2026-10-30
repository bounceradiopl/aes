// SPDX-License-Identifier: GPL-3.0-or-later
#include <pthread.h>
#include <sched.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

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
#ifdef __APPLE__
  // Time-constraint policy: what CoreAudio clients use; available without privileges.
  mach_timebase_info_data_t tb{};
  mach_timebase_info(&tb);
  const double abs_per_ns = static_cast<double>(tb.denom) / tb.numer;
  thread_time_constraint_policy_data_t pol{};
  pol.period = static_cast<uint32_t>(2'666'667 * abs_per_ns);  // one 128-frame period at 48 kHz
  pol.computation = static_cast<uint32_t>(500'000 * abs_per_ns);
  pol.constraint = static_cast<uint32_t>(2'000'000 * abs_per_ns);
  pol.preemptible = 1;
  thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                    reinterpret_cast<thread_policy_t>(&pol), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
  sched_param p{};
  p.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);  // needs CAP_SYS_NICE; failure is fine
#endif
}

}  // namespace bam::platform
