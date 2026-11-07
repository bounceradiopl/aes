// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace bam {

inline constexpr uint32_t kSampleRate = 48000;
inline constexpr int kDeviceCount = 32;
inline constexpr int kChannelsPerDevice = 2;
inline constexpr int kChannelCount = kDeviceCount * kChannelsPerDevice;

// Every channel owns a ring of kRingFrames samples addressed by absolute media-clock sample index.
inline constexpr uint32_t kRingFrames = 65536;
inline constexpr uint32_t kRingMask = kRingFrames - 1;
inline constexpr uint32_t kBlockFrames = 16;
inline constexpr uint32_t kBlocksPerRing = kRingFrames / kBlockFrames;

inline constexpr uint16_t kRtpPort = 5004;
inline constexpr uint16_t kSapPort = 9875;
inline constexpr uint16_t kPtpEventPort = 319;
inline constexpr uint16_t kPtpGeneralPort = 320;
inline constexpr uint16_t kDefaultHttpPort = 8067;
inline constexpr const char* kSapGroup = "239.255.255.255";
inline constexpr const char* kPtpGroup = "224.0.1.129";
inline constexpr int kMulticastTtl = 15;

inline constexpr const char* kProductName = "BounceAES Master";
inline constexpr const char* kVersion = "1.2.5";

}  // namespace bam
