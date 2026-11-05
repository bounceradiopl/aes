// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: run-time codec libraries and the per-codec encoder factories.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bam/encoder/audio_encoder.hpp"

namespace bam::encoder::detail {

struct LameApi;  // codec_lame.cpp
struct FdkApi;   // codec_fdk.cpp

// Full paths in `dirs` first, then the plain names (system search path).
std::vector<std::string> library_candidates(const std::vector<std::string>& dirs,
                                            const std::vector<std::string>& names);

// nullptr when the library or one of its functions is missing.
std::shared_ptr<const LameApi> load_lame(const std::vector<std::string>& dirs);
std::shared_ptr<const FdkApi> load_fdk(const std::vector<std::string>& dirs);

std::unique_ptr<AudioEncoder> make_lame_encoder(std::shared_ptr<const LameApi> api);
std::unique_ptr<AudioEncoder> make_fdk_encoder(Codec codec, std::shared_ptr<const FdkApi> api);
std::unique_ptr<AudioEncoder> make_flac_encoder();

// open(): the settings must be for this encoder's codec and pass validate_encoder_settings().
bool check_open_settings(Codec encoder_codec, const EncoderSettings& s, std::string* error);

// Display name for messages: "MP3", "AAC-LC", "HE-AAC", "HE-AACv2", "FLAC".
const char* codec_label(Codec);

// float (full scale ±1.0) -> 16-bit PCM, rounded, saturating at the limits (no wrap-around).
inline int32_t float_to_pcm16(float x) {
  const float v = x * 32768.0f;
  if (v != v) return 0;  // NaN
  if (v >= 32767.0f) return 32767;
  if (v <= -32768.0f) return -32768;
  return static_cast<int32_t>(std::lrint(v));
}

}  // namespace bam::encoder::detail
