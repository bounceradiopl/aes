// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

inline std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The values of a JSON text as bam::http::json_shape_error counts them: each ',' and ':' outside
// strings.
inline size_t json_value_separators(const std::string& text) {
  size_t values = 0;
  bool in_string = false, escaped = false;
  for (const char ch : text) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') in_string = false;
      continue;
    }
    if (ch == '"') in_string = true;
    else if (ch == ',' || ch == ':') ++values;
  }
  return values;
}
