// SPDX-License-Identifier: GPL-3.0-or-later
#include "bam/ipv4.hpp"

namespace bam {

std::optional<Ipv4> parse_ipv4(std::string_view text) {
  Ipv4 out{};
  int part = 0;
  int value = -1;
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value) * 10 + (c - '0');
      if (value > 255) return std::nullopt;
    } else if (c == '.') {
      if (value < 0 || part >= 3) return std::nullopt;
      out[part++] = static_cast<uint8_t>(value);
      value = -1;
    } else {
      return std::nullopt;
    }
  }
  if (value < 0 || part != 3) return std::nullopt;
  out[3] = static_cast<uint8_t>(value);
  return out;
}

std::string ipv4_to_string(const Ipv4& a) {
  return std::to_string(a[0]) + "." + std::to_string(a[1]) + "." + std::to_string(a[2]) + "." +
         std::to_string(a[3]);
}

bool is_multicast(const Ipv4& a) { return a[0] >= 224 && a[0] <= 239; }

bool is_link_local_multicast(const Ipv4& a) { return a[0] == 224 && a[1] == 0 && a[2] == 0; }

}  // namespace bam
