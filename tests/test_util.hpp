// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <fstream>
#include <sstream>
#include <string>

inline std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
