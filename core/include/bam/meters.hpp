// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace bam {

inline constexpr float kSilenceDb = -120.0f;

// Peak level of left-aligned int32 samples in dBFS, floored at kSilenceDb.
float peak_dbfs(const int32_t* samples, size_t n);

}  // namespace bam
