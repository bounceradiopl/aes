// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace bam {

double db_to_gain(double db);
// Multiplies samples in place with saturation. gain == 1.0 leaves the samples bit-identical.
void apply_gain(int32_t* samples, size_t n, double gain);

}  // namespace bam
