// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bam {

using Ipv4 = std::array<uint8_t, 4>;

std::optional<Ipv4> parse_ipv4(std::string_view text);
std::string ipv4_to_string(const Ipv4& a);
bool is_multicast(const Ipv4& a);          // 224.0.0.0/4
bool is_link_local_multicast(const Ipv4& a);  // 224.0.0.0/24

}  // namespace bam
