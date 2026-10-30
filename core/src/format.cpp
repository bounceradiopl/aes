// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/format.hpp"

namespace bam {

size_t bytes_per_sample(Encoding e) { return e == Encoding::L16 ? 2 : 3; }

const char* encoding_name(Encoding e) { return e == Encoding::L16 ? "L16" : "L24"; }

}  // namespace bam
