// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

namespace bam {

enum class Encoding { L16, L24 };

size_t bytes_per_sample(Encoding e);
const char* encoding_name(Encoding e);

}  // namespace bam
