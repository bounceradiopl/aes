// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "bam/bridge_protocol.hpp"

namespace bam::bridge {

std::vector<BridgeEndpoint> list_endpoints();  // active render and capture devices
std::vector<BridgeApp> list_apps();            // processes with an audio session on any render device
uint32_t find_process(const std::string& exe); // root process of that executable (lower case), 0 if none
uint32_t windows_build();                      // e.g. 22631
std::string lower(std::string s);

}  // namespace bam::bridge
