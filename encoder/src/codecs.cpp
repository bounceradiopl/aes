// SPDX-License-Identifier: GPL-3.0-or-later
// Codec table (spec §5.2), settings validation, run-time library cache and the encoder factory.
#include <algorithm>
#include <mutex>

#include "bam/platform/process.hpp"
#include "codec_impl.hpp"

namespace bam::encoder {

namespace {

const std::vector<uint32_t> kFiveRates(kStreamRates.begin(), kStreamRates.end());
const std::vector<uint32_t> kThreeRates = {48000, 44100, 32000};

// MPEG-1 (32/44.1/48 kHz) and MPEG-2 (16/22.05/24 kHz) Layer III bitrate steps, from 32 kb/s up
const std::vector<uint32_t> kMpeg1Steps = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
const std::vector<uint32_t> kMpeg2Steps = {32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};

std::string join(const std::vector<uint32_t>& v, const char* sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += sep;
    s += std::to_string(v[i]);
  }
  return s;
}

bool fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

// Run-time libraries: loaded once per search-dir setting, shared by all encoders (an encoder
// keeps its library loaded through the shared_ptr even after the dirs change).
struct LibraryCache {
  std::mutex mutex;
  bool dirs_set = false;
  std::vector<std::string> dirs;
  bool lame_tried = false, fdk_tried = false;
  std::shared_ptr<const detail::LameApi> lame;
  std::shared_ptr<const detail::FdkApi> fdk;

  std::vector<std::string> search_dirs() {
    if (dirs_set) return dirs;
    const std::string exe = platform::executable_dir();
    return exe.empty() ? std::vector<std::string>{} : std::vector<std::string>{exe};
  }
};

LibraryCache& cache() {
  static LibraryCache c;
  return c;
}

std::shared_ptr<const detail::LameApi> lame_api() {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  if (!c.lame_tried) {
    c.lame = detail::load_lame(c.search_dirs());
    c.lame_tried = true;
  }
  return c.lame;
}

std::shared_ptr<const detail::FdkApi> fdk_api() {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  if (!c.fdk_tried) {
    c.fdk = detail::load_fdk(c.search_dirs());
    c.fdk_tried = true;
  }
  return c.fdk;
}

}  // namespace

const CodecLimits& codec_limits(Codec c) {
  static const CodecLimits mp3{32, 320, kFiveRates, false};
  static const CodecLimits aac_lc{32, 320, kFiveRates, false};
  static const CodecLimits he_aac{24, 128, kThreeRates, false};
  static const CodecLimits he_aac_v2{16, 64, kThreeRates, false};
  static const CodecLimits flac{0, 0, kFiveRates, true};
  switch (c) {
    case Codec::Mp3: return mp3;
    case Codec::AacLc: return aac_lc;
    case Codec::HeAac: return he_aac;
    case Codec::HeAacV2: return he_aac_v2;
    case Codec::Flac: return flac;
  }
  return flac;
}

std::optional<Codec> parse_codec(std::string_view s) {
  for (auto c : {Codec::Mp3, Codec::AacLc, Codec::HeAac, Codec::HeAacV2, Codec::Flac})
    if (s == codec_name(c)) return c;
  return std::nullopt;
}

const char* codec_name(Codec c) {
  switch (c) {
    case Codec::Mp3: return "mp3";
    case Codec::AacLc: return "aac";
    case Codec::HeAac: return "he-aac";
    case Codec::HeAacV2: return "he-aac-v2";
    case Codec::Flac: return "flac";
  }
  return "";
}

const char* codec_content_type(Codec c) {
  switch (c) {
    case Codec::Mp3: return "audio/mpeg";
    case Codec::AacLc: return "audio/aac";
    case Codec::HeAac:
    case Codec::HeAacV2: return "audio/aacp";
    case Codec::Flac: return "audio/ogg";
  }
  return "";
}

bool validate_encoder_settings(const EncoderSettings& s, std::string* error) {
  const CodecLimits& lim = codec_limits(s.codec);
  const std::string label = detail::codec_label(s.codec);
  // streams carry the station's stereo mix (spec §4.3); HE-AACv2 needs stereo anyway
  if (s.channels != 2)
    return fail(error, "obsługiwany jest tylko sygnał stereo (2 kanały), podano " + std::to_string(s.channels));
  if (std::find(lim.rates.begin(), lim.rates.end(), s.sample_rate) == lim.rates.end())
    return fail(error, "nieobsługiwana częstotliwość próbkowania " + std::to_string(s.sample_rate) + " Hz dla " +
                           label + " (dozwolone: " + join(lim.rates, ", ") + " Hz)");
  if (lim.lossless) return true;
  if (s.bitrate_kbps < lim.min_kbps || s.bitrate_kbps > lim.max_kbps)
    return fail(error, "nieobsługiwany bitrate " + std::to_string(s.bitrate_kbps) + " kb/s dla " + label +
                           " (dozwolone " + std::to_string(lim.min_kbps) + "–" + std::to_string(lim.max_kbps) +
                           " kb/s)");
  if (s.codec == Codec::AacLc) {
    // AAC allows at most 6144 bits per channel and 1024-sample frame: 288 kb/s stereo at
    // 24 kHz, 264 kb/s at 22.05 kHz (fdk-aac would silently lower the bitrate)
    const uint32_t max_kbps = 12 * s.sample_rate / 1000;
    if (s.bitrate_kbps > max_kbps)
      return fail(error, "bitrate " + std::to_string(s.bitrate_kbps) + " kb/s jest za wysoki dla AAC-LC przy " +
                             std::to_string(s.sample_rate) + " Hz (najwyżej " + std::to_string(max_kbps) + " kb/s)");
  }
  if (s.codec == Codec::Mp3) {
    const auto& steps = s.sample_rate >= 32000 ? kMpeg1Steps : kMpeg2Steps;
    if (std::find(steps.begin(), steps.end(), s.bitrate_kbps) == steps.end())
      return fail(error, "bitrate " + std::to_string(s.bitrate_kbps) + " kb/s nie jest standardowym krokiem MP3 przy " +
                             std::to_string(s.sample_rate) + " Hz (dozwolone: " + join(steps, ", ") + " kb/s)");
  }
  return true;
}

std::unique_ptr<AudioEncoder> make_encoder(Codec c) {
  switch (c) {
    case Codec::Mp3: {
      auto api = lame_api();
      return api ? detail::make_lame_encoder(std::move(api)) : nullptr;
    }
    case Codec::AacLc:
    case Codec::HeAac:
    case Codec::HeAacV2: {
      auto api = fdk_api();
      return api ? detail::make_fdk_encoder(c, std::move(api)) : nullptr;
    }
    case Codec::Flac: return detail::make_flac_encoder();
  }
  return nullptr;
}

bool codec_available(Codec c) {
  switch (c) {
    case Codec::Mp3: return lame_api() != nullptr;
    case Codec::AacLc:
    case Codec::HeAac:
    case Codec::HeAacV2: return fdk_api() != nullptr;
    case Codec::Flac: return true;  // linked in
  }
  return false;
}

void set_codec_library_dirs(std::vector<std::string> dirs) {
  auto& c = cache();
  std::lock_guard<std::mutex> lock(c.mutex);
  c.dirs_set = !dirs.empty();
  c.dirs = std::move(dirs);
  c.lame_tried = c.fdk_tried = false;
  c.lame.reset();
  c.fdk.reset();
}

namespace detail {

std::vector<std::string> library_candidates(const std::vector<std::string>& dirs,
                                            const std::vector<std::string>& names) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  std::vector<std::string> out;
  for (const auto& d : dirs) {
    if (d.empty()) continue;
    const bool has_sep = d.back() == '/' || d.back() == '\\';
    for (const auto& n : names) out.push_back(has_sep ? d + n : d + sep + n);
  }
  return out;  // no bare names: a codec library found elsewhere must not enable the codec
}

bool check_open_settings(Codec encoder_codec, const EncoderSettings& s, std::string* error) {
  if (s.codec != encoder_codec)
    return fail(error, std::string("ustawienia dla ") + codec_label(s.codec) + " podane koderowi " +
                           codec_label(encoder_codec));
  return validate_encoder_settings(s, error);
}

const char* codec_label(Codec c) {
  switch (c) {
    case Codec::Mp3: return "MP3";
    case Codec::AacLc: return "AAC-LC";
    case Codec::HeAac: return "HE-AAC";
    case Codec::HeAacV2: return "HE-AACv2";
    case Codec::Flac: return "FLAC";
  }
  return "";
}

}  // namespace detail

}  // namespace bam::encoder

namespace bam::encoder::testing {

void fail_next_encode(AudioEncoder& e) { e.inject_failure_ = true; }
size_t buffered_bytes(const AudioEncoder& e) { return e.buffered_bytes(); }

}  // namespace bam::encoder::testing
