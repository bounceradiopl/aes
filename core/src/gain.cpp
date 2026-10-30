// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/gain.hpp"

#include <cmath>
#include <limits>

namespace bam {

double db_to_gain(double db) { return std::pow(10.0, db / 20.0); }

void apply_gain(int32_t* samples, size_t n, double gain) {
  if (gain == 1.0) return;
  constexpr double kMax = std::numeric_limits<int32_t>::max();
  constexpr double kMin = std::numeric_limits<int32_t>::min();
  for (size_t i = 0; i < n; ++i) {
    double v = std::round(samples[i] * gain);
    if (v > kMax) v = kMax;
    if (v < kMin) v = kMin;
    samples[i] = static_cast<int32_t>(v);
  }
}

}  // namespace bam
