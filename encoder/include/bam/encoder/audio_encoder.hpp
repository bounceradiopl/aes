// SPDX-License-Identifier: GPL-3.0-or-later
// Audio encoders for Bounce Encoder (spec §5): MP3 (LAME), AAC-LC / HE-AAC / HE-AACv2 (fdk-aac,
// ADTS) and FLAC in Ogg (libFLAC + libogg). LAME and fdk-aac are shared libraries loaded at run
// time (licences, spec §5.3); libFLAC and libogg are linked in.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bam::encoder {

enum class Codec { Mp3, AacLc, HeAac, HeAacV2, Flac };

// Allowed settings per codec (spec §5.2). For a lossless codec the bitrate is not used.
struct CodecLimits {
  uint32_t min_kbps, max_kbps;
  std::vector<uint32_t> rates;
  bool lossless;
};
const CodecLimits& codec_limits(Codec);

std::optional<Codec> parse_codec(std::string_view);  // "mp3", "aac", "he-aac", "he-aac-v2", "flac"
const char* codec_name(Codec);                       // the inverse of parse_codec
const char* codec_content_type(Codec);               // "audio/mpeg", "audio/aac", "audio/aacp", "audio/ogg"

struct EncoderSettings {
  Codec codec;
  uint32_t bitrate_kbps;  // ignored for FLAC
  uint32_t sample_rate;
  int channels = 2;       // stereo only: streams carry the station's stereo mix
};

// Checks the settings against the codec table; on failure a Polish message for the user.
// Within the table two limits of the formats themselves also apply:
// - MP3 needs a standard bitrate step of the MPEG version the sample rate selects
//   (MPEG-1 at 32/44.1/48 kHz: up to 320 kb/s, MPEG-2 at 22.05/24 kHz: up to 160 kb/s);
// - AAC-LC at most 12 bits per sample period in stereo (288 kb/s at 24 kHz, 264 at 22.05 kHz).
bool validate_encoder_settings(const EncoderSettings&, std::string* error);

class AudioEncoder;
namespace testing {
// Test hook: the encoder's next call into its codec library reports an error.
void fail_next_encode(AudioEncoder&);
// Test hook: PCM/bytes the encoder holds between calls (must stay bounded).
size_t buffered_bytes(const AudioEncoder&);
}  // namespace testing

class AudioEncoder {
 public:
  virtual ~AudioEncoder() = default;
  // (Re)starts the stream: any previous state is dropped, the next bytes begin a new stream
  // (for Ogg FLAC a new logical stream with its own serial number and headers).
  virtual bool open(const EncoderSettings&, std::string* error) = 0;
  // Interleaved float samples, full scale ±1.0; appends bytes ready to send.
  virtual void encode(const float* interleaved, size_t frames, std::vector<uint8_t>& out) = 0;
  // Ends the stream and appends the remaining bytes; open() again to start a new one.
  virtual void flush(std::vector<uint8_t>& out) = 0;
  virtual const char* content_type() const = 0;

  // An encode error from the codec library: the encoder drops what it buffered, closes and
  // outputs nothing more until the next successful open(). error() is Polish user text.
  bool failed() const { return !error_.empty(); }
  const std::string& error() const { return error_; }

 protected:
  void set_failed(std::string message) { error_ = std::move(message); }
  void clear_failed() { error_.clear(); }
  bool take_injected_failure() {
    const bool f = inject_failure_;
    inject_failure_ = false;
    return f;
  }
  virtual size_t buffered_bytes() const { return 0; }

 private:
  friend void testing::fail_next_encode(AudioEncoder&);
  friend size_t testing::buffered_bytes(const AudioEncoder&);
  std::string error_;
  bool inject_failure_ = false;
};

std::unique_ptr<AudioEncoder> make_encoder(Codec);  // nullptr when the codec library is not available
bool codec_available(Codec);

// The only directories (absolute paths) the run-time codec libraries (libmp3lame, libfdk-aac)
// are loaded from; empty = the default, the directory of the executable. There is no fallback
// to the system search path: without the bundled library next to the program the codec is
// unavailable (spec §5.3), and nothing can be planted elsewhere on the search path.
void set_codec_library_dirs(std::vector<std::string> dirs);

}  // namespace bam::encoder
