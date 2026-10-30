// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include "bam/format.hpp"

TEST_CASE("bytes per sample") {
  CHECK(bam::bytes_per_sample(bam::Encoding::L16) == 2);
  CHECK(bam::bytes_per_sample(bam::Encoding::L24) == 3);
  CHECK(std::string(bam::encoding_name(bam::Encoding::L24)) == "L24");
  CHECK(std::string(bam::encoding_name(bam::Encoding::L16)) == "L16");
}
