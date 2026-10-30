// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/meters.hpp"

#include <cmath>
#include <cstdlib>

namespace bam {

float peak_dbfs(const int32_t* samples, size_t n) {
  int64_t peak = 0;
  for (size_t i = 0; i < n; ++i) {
    int64_t v = std::llabs(static_cast<int64_t>(samples[i]));
    if (v > peak) peak = v;
  }
  if (peak == 0) return kSilenceDb;
  float db = static_cast<float>(20.0 * std::log10(static_cast<double>(peak) / 2147483647.0));
  return db < kSilenceDb ? kSilenceDb : db;
}

}  // namespace bam
