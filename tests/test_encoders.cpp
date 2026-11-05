// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder audio encoders (spec §5): settings table, and for every codec an encode +
// decode-back check (FLAC bit-exact, MP3/AAC level and correlation after the codec delay).
#include <doctest/doctest.h>

#ifdef BAM_WITH_CODECS

#include <FLAC/stream_decoder.h>
#include <aacdecoder_lib.h>
#include <aacenc_lib.h>
#include <lame.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "bam/encoder/audio_encoder.hpp"
#include "bam/platform/dynlib.hpp"

namespace be = bam::encoder;
namespace bp = bam::platform;

namespace {

constexpr double kPi = 3.14159265358979323846;

#define SKIP_UNLESS_AVAILABLE(codec)                                                          \
  if (!be::codec_available(codec)) {                                                          \
    MESSAGE("codec library for " << std::string(be::codec_name(codec)) << " missing, skipped"); \
    return;                                                                                   \
  }

// Deterministic stereo noise, every value a 16-bit sample (so float -> 16 bit is exact).
std::vector<int16_t> noise16(size_t frames, int channels, uint32_t seed) {
  std::vector<int16_t> s(frames * size_t(channels));
  uint32_t x = seed;
  for (auto& v : s) {
    x = x * 1664525u + 1013904223u;
    v = static_cast<int16_t>(x >> 16);
  }
  return s;
}

std::vector<float> to_float(const std::vector<int16_t>& s) {
  std::vector<float> f(s.size());
  for (size_t i = 0; i < s.size(); ++i) f[i] = float(s[i]) / 32768.0f;
  return f;
}

// Interleaved stereo: left `freq_l` Hz, right `freq_r` Hz, both at `amplitude`.
std::vector<float> stereo_sine(uint32_t rate, double seconds, double freq_l, double freq_r, double amplitude) {
  const size_t frames = size_t(rate * seconds);
  std::vector<float> f(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    f[2 * i] = float(amplitude * std::sin(2 * kPi * freq_l * double(i) / rate));
    f[2 * i + 1] = float(amplitude * std::sin(2 * kPi * freq_r * double(i) / rate));
  }
  return f;
}

std::vector<double> channel(const std::vector<float>& interleaved, int channels, int ch) {
  std::vector<double> c(interleaved.size() / size_t(channels));
  for (size_t i = 0; i < c.size(); ++i) c[i] = interleaved[i * size_t(channels) + size_t(ch)];
  return c;
}

std::vector<double> mono_sum(const std::vector<float>& stereo) {
  std::vector<double> m(stereo.size() / 2);
  for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5 * (double(stereo[2 * i]) + double(stereo[2 * i + 1]));
  return m;
}

double rms_db(const std::vector<double>& x, size_t from, size_t n) {
  double acc = 0;
  for (size_t i = from; i < from + n; ++i) acc += x[i] * x[i];
  return 10 * std::log10(acc / double(n));
}

// Pearson correlation of ref[from..from+n) with dec[from+lag..).
double correlation(const std::vector<double>& ref, const std::vector<double>& dec, size_t from, size_t n, size_t lag) {
  double xy = 0, xx = 0, yy = 0;
  for (size_t i = from; i < from + n; ++i) {
    xy += ref[i] * dec[i + lag];
    xx += ref[i] * ref[i];
    yy += dec[i + lag] * dec[i + lag];
  }
  return xy / std::sqrt(xx * yy);
}

// Encodes `pcm` in uneven chunks (as the stream worker would), then flushes.
std::vector<uint8_t> encode_all(be::AudioEncoder& enc, const std::vector<float>& pcm, int channels) {
  std::vector<uint8_t> out;
  const size_t frames = pcm.size() / size_t(channels);
  const size_t chunks[] = {1, 479, 480, 1000, 4096, 37};
  size_t pos = 0, k = 0;
  while (pos < frames) {
    const size_t n = std::min(chunks[k++ % 6], frames - pos);
    enc.encode(pcm.data() + pos * size_t(channels), n, out);
    pos += n;
  }
  enc.flush(out);
  return out;
}

// ---- FLAC ----

struct FlacDecoded {
  std::vector<int32_t> samples;  // interleaved
  unsigned sample_rate = 0, channels = 0, bits = 0;
  bool error = false;
};

struct FlacReadState {
  const std::vector<uint8_t>* data;
  size_t pos = 0;
  FlacDecoded* out;
};

FlacDecoded decode_ogg_flac(const std::vector<uint8_t>& data) {
  FlacDecoded d;
  FlacReadState st{&data, 0, &d};
  FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
  auto read_cb = [](const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes,
                    void* cd) -> FLAC__StreamDecoderReadStatus {
    auto* s = static_cast<FlacReadState*>(cd);
    const size_t n = std::min(*bytes, s->data->size() - s->pos);
    std::memcpy(buffer, s->data->data() + s->pos, n);
    s->pos += n;
    *bytes = n;
    return n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  };
  auto write_cb = [](const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[],
                     void* cd) -> FLAC__StreamDecoderWriteStatus {
    auto* s = static_cast<FlacReadState*>(cd);
    for (unsigned i = 0; i < frame->header.blocksize; ++i)
      for (unsigned c = 0; c < frame->header.channels; ++c) s->out->samples.push_back(buffer[c][i]);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  };
  auto meta_cb = [](const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* cd) {
    auto* s = static_cast<FlacReadState*>(cd);
    if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
      s->out->sample_rate = m->data.stream_info.sample_rate;
      s->out->channels = m->data.stream_info.channels;
      s->out->bits = m->data.stream_info.bits_per_sample;
    }
  };
  auto error_cb = [](const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* cd) {
    static_cast<FlacReadState*>(cd)->out->error = true;
  };
  REQUIRE(FLAC__stream_decoder_init_ogg_stream(dec, read_cb, nullptr, nullptr, nullptr, nullptr, write_cb, meta_cb,
                                               error_cb, &st) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
  CHECK(FLAC__stream_decoder_process_until_end_of_stream(dec));
  FLAC__stream_decoder_finish(dec);
  FLAC__stream_decoder_delete(dec);
  return d;
}

uint32_t ogg_serial(const std::vector<uint8_t>& page) {
  REQUIRE(page.size() >= 27);
  return uint32_t(page[14]) | uint32_t(page[15]) << 8 | uint32_t(page[16]) << 16 | uint32_t(page[17]) << 24;
}

// ---- MP3 ----

struct Mp3Frames {
  size_t count = 0;
  bool all_valid = true;
  std::set<uint32_t> bitrates, rates;
  int mpeg_version = 0;  // 1 or 2
  size_t min_size = SIZE_MAX, max_size = 0;
};

// Walks every frame header: sync, Layer III, bitrate, rate, size from the header fields.
Mp3Frames parse_mp3(const std::vector<uint8_t>& data) {
  static const uint32_t kBitratesV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const uint32_t kBitratesV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  static const uint32_t kRatesV1[4] = {44100, 48000, 32000, 0};
  Mp3Frames f;
  size_t pos = 0;
  while (pos + 4 <= data.size()) {
    const uint8_t* h = data.data() + pos;
    const bool sync = ((uint32_t(h[0]) << 8 | h[1]) & 0xFFE0) == 0xFFE0;
    const int version_bits = (h[1] >> 3) & 3;  // 3 = MPEG-1, 2 = MPEG-2
    const int layer_bits = (h[1] >> 1) & 3;    // 1 = Layer III
    const int br_index = h[2] >> 4, sr_index = (h[2] >> 2) & 3, padding = (h[2] >> 1) & 1;
    if (!sync || (version_bits != 3 && version_bits != 2) || layer_bits != 1 || br_index == 0 || br_index == 15 ||
        sr_index == 3) {
      f.all_valid = false;
      break;
    }
    const bool v1 = version_bits == 3;
    f.mpeg_version = v1 ? 1 : 2;
    const uint32_t kbps = v1 ? kBitratesV1[br_index] : kBitratesV2[br_index];
    const uint32_t rate = v1 ? kRatesV1[sr_index] : kRatesV1[sr_index] / 2;
    const size_t size = (v1 ? 144000 : 72000) * size_t(kbps) / rate + size_t(padding);
    f.bitrates.insert(kbps);
    f.rates.insert(rate);
    f.min_size = std::min(f.min_size, size);
    f.max_size = std::max(f.max_size, size);
    ++f.count;
    pos += size;
  }
  if (pos != data.size()) f.all_valid = false;
  return f;
}

// Decodes with LAME's mpglib (hip_*) from the same shared library; returns interleaved stereo.
std::vector<float> decode_mp3(const std::vector<uint8_t>& data) {
  std::string err;
  auto lib = bp::DynamicLibrary::open({BAM_MP3LAME_PATH}, &err);
  REQUIRE_MESSAGE(lib, err);
  auto init = reinterpret_cast<decltype(&hip_decode_init)>(lib->symbol("hip_decode_init"));
  auto decode = reinterpret_cast<decltype(&hip_decode)>(lib->symbol("hip_decode"));
  auto exit = reinterpret_cast<decltype(&hip_decode_exit)>(lib->symbol("hip_decode_exit"));
  REQUIRE((init && decode && exit));
  hip_t hip = init();
  std::vector<float> pcm;
  std::vector<short> l(1152 * 16), r(1152 * 16);
  std::vector<uint8_t> copy = data;  // hip_decode takes a non-const buffer
  for (size_t pos = 0; pos < copy.size(); pos += 512) {
    const size_t n = std::min<size_t>(512, copy.size() - pos);
    int got = decode(hip, copy.data() + pos, n, l.data(), r.data());
    REQUIRE(got >= 0);
    for (int i = 0; i < got; ++i) {
      pcm.push_back(float(l[size_t(i)]) / 32768.0f);
      pcm.push_back(float(r[size_t(i)]) / 32768.0f);
    }
  }
  exit(hip);
  return pcm;
}

// ---- AAC ----

struct AdtsFrames {
  size_t count = 0;
  bool all_valid = true;
  std::set<int> profiles, rate_indices, channel_configs;
};

AdtsFrames parse_adts(const std::vector<uint8_t>& data) {
  AdtsFrames f;
  size_t pos = 0;
  while (pos + 7 <= data.size()) {
    const uint8_t* h = data.data() + pos;
    const bool sync = (uint32_t(h[0]) << 4 | h[1] >> 4) == 0xFFF;
    const int layer = (h[1] >> 1) & 3;
    const size_t length = size_t(h[3] & 3) << 11 | size_t(h[4]) << 3 | size_t(h[5] >> 5);
    if (!sync || layer != 0 || length < 7) {
      f.all_valid = false;
      break;
    }
    f.profiles.insert(h[2] >> 6);  // audio object type - 1
    f.rate_indices.insert((h[2] >> 2) & 0xF);
    f.channel_configs.insert((h[2] & 1) << 2 | h[3] >> 6);
    ++f.count;
    pos += length;
  }
  if (pos != data.size()) f.all_valid = false;
  return f;
}

struct AacDecoded {
  std::vector<float> pcm;  // interleaved, `channels` channels
  int channels = 0, sample_rate = 0, core_channels = 0, core_sample_rate = 0, output_delay = 0;
};

AacDecoded decode_aac(const std::vector<uint8_t>& data) {
  std::string err;
  auto lib = bp::DynamicLibrary::open({BAM_FDKAAC_PATH}, &err);
  REQUIRE_MESSAGE(lib, err);
  auto open = reinterpret_cast<decltype(&aacDecoder_Open)>(lib->symbol("aacDecoder_Open"));
  auto fill = reinterpret_cast<decltype(&aacDecoder_Fill)>(lib->symbol("aacDecoder_Fill"));
  auto decode = reinterpret_cast<decltype(&aacDecoder_DecodeFrame)>(lib->symbol("aacDecoder_DecodeFrame"));
  auto info = reinterpret_cast<decltype(&aacDecoder_GetStreamInfo)>(lib->symbol("aacDecoder_GetStreamInfo"));
  auto close = reinterpret_cast<decltype(&aacDecoder_Close)>(lib->symbol("aacDecoder_Close"));
  REQUIRE((open && fill && decode && info && close));
  HANDLE_AACDECODER dec = open(TT_MP4_ADTS, 1);
  REQUIRE(dec != nullptr);
  AacDecoded d;
  std::vector<INT_PCM> frame(2048 * 8);
  std::vector<uint8_t> copy = data;
  size_t pos = 0;
  for (;;) {
    if (pos < copy.size()) {
      UCHAR* buf = copy.data() + pos;
      const UINT size = UINT(std::min<size_t>(4096, copy.size() - pos));
      UINT valid = size;
      REQUIRE(fill(dec, &buf, &size, &valid) == AAC_DEC_OK);
      pos += size - valid;
    }
    const AAC_DECODER_ERROR e = decode(dec, frame.data(), INT(frame.size()), 0);
    if (e == AAC_DEC_NOT_ENOUGH_BITS) {
      if (pos >= copy.size()) break;
      continue;
    }
    REQUIRE_MESSAGE(e == AAC_DEC_OK, "aacDecoder_DecodeFrame 0x" << std::hex << int(e));
    const CStreamInfo* si = info(dec);
    d.channels = si->numChannels;
    d.sample_rate = si->sampleRate;
    d.core_channels = si->aacNumChannels;
    d.core_sample_rate = si->aacSampleRate;
    d.output_delay = int(si->outputDelay);
    for (int i = 0; i < si->frameSize * si->numChannels; ++i) d.pcm.push_back(float(frame[size_t(i)]) / 32768.0f);
  }
  close(dec);
  return d;
}

// The codec delay fdk-aac reports for these settings (AACENC_InfoStruct::nDelay), from a
// separate encoder instance opened the same way as the one under test.
size_t fdk_encoder_delay(UINT aot, UINT rate, UINT bitrate) {
  std::string err;
  auto lib = bp::DynamicLibrary::open({BAM_FDKAAC_PATH}, &err);
  REQUIRE_MESSAGE(lib, err);
  auto open = reinterpret_cast<decltype(&aacEncOpen)>(lib->symbol("aacEncOpen"));
  auto set = reinterpret_cast<decltype(&aacEncoder_SetParam)>(lib->symbol("aacEncoder_SetParam"));
  auto encode = reinterpret_cast<decltype(&aacEncEncode)>(lib->symbol("aacEncEncode"));
  auto info = reinterpret_cast<decltype(&aacEncInfo)>(lib->symbol("aacEncInfo"));
  auto close = reinterpret_cast<decltype(&aacEncClose)>(lib->symbol("aacEncClose"));
  REQUIRE((open && set && encode && info && close));
  HANDLE_AACENCODER h = nullptr;
  REQUIRE(open(&h, 0, 2) == AACENC_OK);
  set(h, AACENC_AOT, aot);
  set(h, AACENC_SAMPLERATE, rate);
  set(h, AACENC_CHANNELMODE, MODE_2);
  set(h, AACENC_BITRATE, bitrate);
  set(h, AACENC_TRANSMUX, TT_MP4_ADTS);
  set(h, AACENC_AFTERBURNER, 1);
  REQUIRE(encode(h, nullptr, nullptr, nullptr, nullptr) == AACENC_OK);
  AACENC_InfoStruct i{};
  REQUIRE(info(h, &i) == AACENC_OK);
  close(&h);
  return i.nDelay;
}

// Finds the lag in [lo, hi] with the highest correlation.
size_t best_lag(const std::vector<double>& ref, const std::vector<double>& dec, size_t from, size_t n, size_t lo,
                size_t hi) {
  size_t best = lo;
  double best_c = -2;
  for (size_t lag = lo; lag <= hi && from + n + lag <= dec.size(); ++lag) {
    const double c = correlation(ref, dec, from, n, lag);
    if (c > best_c) {
      best_c = c;
      best = lag;
    }
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
TEST_CASE("encoders: codec table matches spec 5.2") {
  const std::vector<uint32_t> five = {48000, 44100, 32000, 24000, 22050};
  const std::vector<uint32_t> three = {48000, 44100, 32000};
  const auto& mp3 = be::codec_limits(be::Codec::Mp3);
  CHECK(mp3.min_kbps == 32);
  CHECK(mp3.max_kbps == 320);
  CHECK(mp3.rates == five);
  CHECK_FALSE(mp3.lossless);
  const auto& lc = be::codec_limits(be::Codec::AacLc);
  CHECK(lc.min_kbps == 32);
  CHECK(lc.max_kbps == 320);
  CHECK(lc.rates == five);
  CHECK_FALSE(lc.lossless);
  const auto& he = be::codec_limits(be::Codec::HeAac);
  CHECK(he.min_kbps == 24);
  CHECK(he.max_kbps == 128);
  CHECK(he.rates == three);
  CHECK_FALSE(he.lossless);
  const auto& he2 = be::codec_limits(be::Codec::HeAacV2);
  CHECK(he2.min_kbps == 16);
  CHECK(he2.max_kbps == 64);
  CHECK(he2.rates == three);
  CHECK_FALSE(he2.lossless);
  const auto& flac = be::codec_limits(be::Codec::Flac);
  CHECK(flac.rates == five);
  CHECK(flac.lossless);
}

TEST_CASE("encoders: codec names round trip, content types") {
  for (auto c : {be::Codec::Mp3, be::Codec::AacLc, be::Codec::HeAac, be::Codec::HeAacV2, be::Codec::Flac}) {
    auto parsed = be::parse_codec(be::codec_name(c));
    REQUIRE(parsed);
    CHECK(*parsed == c);
  }
  CHECK(std::string(be::codec_name(be::Codec::Mp3)) == "mp3");
  CHECK(std::string(be::codec_name(be::Codec::AacLc)) == "aac");
  CHECK(std::string(be::codec_name(be::Codec::HeAac)) == "he-aac");
  CHECK(std::string(be::codec_name(be::Codec::HeAacV2)) == "he-aac-v2");
  CHECK(std::string(be::codec_name(be::Codec::Flac)) == "flac");
  CHECK_FALSE(be::parse_codec("vorbis"));
  CHECK_FALSE(be::parse_codec(""));
  CHECK_FALSE(be::parse_codec("MP3 "));
  CHECK(std::string(be::codec_content_type(be::Codec::Mp3)) == "audio/mpeg");
  CHECK(std::string(be::codec_content_type(be::Codec::AacLc)) == "audio/aac");
  CHECK(std::string(be::codec_content_type(be::Codec::HeAac)) == "audio/aacp");
  CHECK(std::string(be::codec_content_type(be::Codec::HeAacV2)) == "audio/aacp");
  CHECK(std::string(be::codec_content_type(be::Codec::Flac)) == "audio/ogg");
}

TEST_CASE("encoders: settings validation follows the codec table, Polish messages") {
  std::string err;
  CHECK(be::validate_encoder_settings({be::Codec::Mp3, 128, 44100}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::Mp3, 320, 48000}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::Mp3, 160, 22050}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::AacLc, 320, 48000}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::AacLc, 288, 24000}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::AacLc, 264, 22050}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::HeAac, 24, 32000}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::HeAacV2, 16, 48000}, &err));
  CHECK(be::validate_encoder_settings({be::Codec::Flac, 0, 22050}, &err));

  struct Bad {
    be::EncoderSettings s;
    const char* word;  // expected in the message
  };
  const Bad bad[] = {
      {{be::Codec::Mp3, 128, 96000}, "częstotliwość"},
      {{be::Codec::Mp3, 400, 48000}, "bitrate"},
      {{be::Codec::Mp3, 100, 48000}, "bitrate"},  // not an MPEG step
      {{be::Codec::Mp3, 192, 24000}, "bitrate"},  // MPEG-2 tops out at 160
      {{be::Codec::Mp3, 16, 48000}, "bitrate"},
      {{be::Codec::AacLc, 330, 48000}, "bitrate"},
      {{be::Codec::AacLc, 128, 16000}, "częstotliwość"},
      {{be::Codec::AacLc, 320, 24000}, "bitrate"},  // above 6144 bits per channel and frame
      {{be::Codec::AacLc, 272, 22050}, "bitrate"},
      {{be::Codec::Mp3, 128, 44100, 1}, "stereo"},
      {{be::Codec::HeAac, 64, 22050}, "częstotliwość"},
      {{be::Codec::HeAac, 160, 48000}, "bitrate"},
      {{be::Codec::HeAacV2, 96, 48000}, "bitrate"},
      {{be::Codec::HeAacV2, 32, 48000, 1}, "stereo"},
      {{be::Codec::Flac, 0, 96000}, "częstotliwość"},
      {{be::Codec::Flac, 0, 48000, 3}, "kanał"},
  };
  for (const auto& b : bad) {
    err.clear();
    const std::string codec = be::codec_name(b.s.codec);
    CAPTURE(codec);
    CAPTURE(b.s.bitrate_kbps);
    CAPTURE(b.s.sample_rate);
    CHECK_FALSE(be::validate_encoder_settings(b.s, &err));
    CHECK_MESSAGE(err.find(b.word) != std::string::npos, err);
  }
}

TEST_CASE("encoders: the run-time codec libraries next to the test binary are found") {
  // The build writes libmp3lame and libfdk-aac next to bam_tests (the default search dir).
  CHECK(be::codec_available(be::Codec::Flac));
  CHECK(be::codec_available(be::Codec::Mp3));
  CHECK(be::codec_available(be::Codec::AacLc));
  CHECK(be::codec_available(be::Codec::HeAacV2));
  for (auto c : {be::Codec::Mp3, be::Codec::AacLc, be::Codec::HeAac, be::Codec::HeAacV2, be::Codec::Flac}) {
    auto enc = be::make_encoder(c);
    REQUIRE(enc);
    CHECK(std::string(enc->content_type()) == be::codec_content_type(c));
  }
  // nothing to find in a directory without the libraries, but the plain name may still
  // resolve through the system search path, so only check this does not crash
  be::set_codec_library_dirs({"/this/directory/does/not/exist"});
  (void)be::codec_available(be::Codec::Mp3);
  be::set_codec_library_dirs({});  // back to the default (executable directory)
  CHECK(be::codec_available(be::Codec::Mp3));
}

TEST_CASE("encoders: open rejects settings outside the table with a Polish error") {
  for (auto c : {be::Codec::Mp3, be::Codec::AacLc, be::Codec::Flac}) {
    if (!be::codec_available(c)) continue;
    auto enc = be::make_encoder(c);
    REQUIRE(enc);
    std::string err;
    CHECK_FALSE(enc->open({c, 128, 96000}, &err));
    CHECK_MESSAGE(err.find("częstotliwość") != std::string::npos, err);
    // a codec mismatch between the encoder and the settings is a programming error, also refused
    err.clear();
    CHECK_FALSE(enc->open({c == be::Codec::Flac ? be::Codec::Mp3 : be::Codec::Flac, 128, 48000}, &err));
    CHECK_FALSE(err.empty());
    // nothing is produced by a closed encoder
    std::vector<uint8_t> out;
    std::vector<float> pcm(2 * 4800, 0.1f);
    enc->encode(pcm.data(), 4800, out);
    enc->flush(out);
    CHECK(out.empty());
  }
  if (be::codec_available(be::Codec::Mp3)) {
    auto enc = be::make_encoder(be::Codec::Mp3);
    REQUIRE(enc);
    std::string err;
    CHECK_FALSE(enc->open({be::Codec::Mp3, 192, 22050}, &err));
    CHECK_MESSAGE(err.find("bitrate") != std::string::npos, err);
  }
  if (be::codec_available(be::Codec::HeAacV2)) {
    auto enc = be::make_encoder(be::Codec::HeAacV2);
    REQUIRE(enc);
    std::string err;
    CHECK_FALSE(enc->open({be::Codec::HeAacV2, 32, 48000, 1}, &err));
    CHECK_MESSAGE(err.find("stereo") != std::string::npos, err);
  }
}

TEST_CASE("encoders: every setting the table allows opens, and the stream carries that setting") {
  // One encoder per codec, reopened for each setting (as after a reconnect or config change).
  size_t opened = 0;
  for (auto c : {be::Codec::Mp3, be::Codec::AacLc, be::Codec::HeAac, be::Codec::HeAacV2, be::Codec::Flac}) {
    if (!be::codec_available(c)) {
      MESSAGE("codec library for " << std::string(be::codec_name(c)) << " missing, skipped");
      continue;
    }
    auto enc = be::make_encoder(c);
    REQUIRE(enc);
    const auto& lim = be::codec_limits(c);
    std::vector<uint32_t> bitrates = {0};
    if (!lim.lossless) {
      bitrates.clear();
      for (uint32_t b = lim.min_kbps; b <= lim.max_kbps; b += 8) bitrates.push_back(b);
      if (bitrates.back() != lim.max_kbps) bitrates.push_back(lim.max_kbps);
    }
    for (uint32_t rate : lim.rates) {
      const auto pcm = stereo_sine(rate, 0.25, 440.0, 660.0, 0.3);
      for (uint32_t kbps : bitrates) {
        const be::EncoderSettings s{c, kbps, rate};
        std::string err;
        if (!be::validate_encoder_settings(s, &err)) continue;
        const std::string codec = be::codec_name(c);
        CAPTURE(codec);
        CAPTURE(rate);
        CAPTURE(kbps);
        REQUIRE_MESSAGE(enc->open(s, &err), err);
        ++opened;
        const auto bytes = encode_all(*enc, pcm, 2);
        REQUIRE_FALSE(bytes.empty());
        if (c == be::Codec::Mp3) {
          const auto f = parse_mp3(bytes);
          CHECK(f.all_valid);
          CHECK(f.bitrates == std::set<uint32_t>{kbps});
          CHECK(f.rates == std::set<uint32_t>{rate});
        } else if (c == be::Codec::Flac) {
          CHECK(std::memcmp(bytes.data(), "OggS", 4) == 0);
        } else {
          CHECK(parse_adts(bytes).all_valid);
        }
      }
    }
  }
  MESSAGE(opened << " settings opened and encoded");
  CHECK(opened > 0);
}

// ---------------------------------------------------------------------------------------------
TEST_CASE("encoders: FLAC in Ogg decodes bit-exact at 48 and 44.1 kHz") {
  SKIP_UNLESS_AVAILABLE(be::Codec::Flac);
  for (uint32_t rate : {48000u, 44100u}) {
    CAPTURE(rate);
    const auto ref = noise16(size_t(rate) * 3, 2, rate);
    auto enc = be::make_encoder(be::Codec::Flac);
    REQUIRE(enc);
    std::string err;
    REQUIRE_MESSAGE(enc->open({be::Codec::Flac, 0, rate}, &err), err);
    const auto bytes = encode_all(*enc, to_float(ref), 2);
    REQUIRE(bytes.size() > 4);
    CHECK(std::memcmp(bytes.data(), "OggS", 4) == 0);
    MESSAGE("FLAC " << rate << " Hz: " << bytes.size() << " bytes for " << ref.size() * 2 << " bytes of PCM");
    const auto dec = decode_ogg_flac(bytes);
    CHECK_FALSE(dec.error);
    CHECK(dec.sample_rate == rate);
    CHECK(dec.channels == 2);
    CHECK(dec.bits == 16);
    REQUIRE(dec.samples.size() == ref.size());
    size_t mismatches = 0;
    for (size_t i = 0; i < ref.size(); ++i) mismatches += dec.samples[i] != ref[i];
    CHECK(mismatches == 0);
  }
}

TEST_CASE("encoders: FLAC at 22.05 kHz, saturation instead of wrap-around") {
  SKIP_UNLESS_AVAILABLE(be::Codec::Flac);
  auto enc = be::make_encoder(be::Codec::Flac);
  REQUIRE(enc);
  std::string err;
  REQUIRE_MESSAGE(enc->open({be::Codec::Flac, 0, 22050}, &err), err);
  std::vector<int16_t> expected = noise16(22050, 2, 7);
  std::vector<float> pcm = to_float(expected);
  // out-of-range input saturates at the 16-bit limits
  const float over[] = {1.5f, -1.5f, 1.0f, -1.0f, 3.0e9f, -3.0e9f};
  const int16_t clipped[] = {32767, -32768, 32767, -32768, 32767, -32768};
  for (size_t i = 0; i < 6; ++i) {
    pcm.push_back(over[i]);
    expected.push_back(clipped[i]);
  }
  const auto bytes = encode_all(*enc, pcm, 2);
  const auto dec = decode_ogg_flac(bytes);
  CHECK(dec.channels == 2);
  CHECK(dec.sample_rate == 22050);
  REQUIRE(dec.samples.size() == expected.size());
  size_t mismatches = 0;
  for (size_t i = 0; i < expected.size(); ++i) mismatches += dec.samples[i] != expected[i];
  CHECK(mismatches == 0);
}

TEST_CASE("encoders: FLAC open again starts a new Ogg logical stream") {
  SKIP_UNLESS_AVAILABLE(be::Codec::Flac);
  auto enc = be::make_encoder(be::Codec::Flac);
  REQUIRE(enc);
  std::string err;
  const auto ref = noise16(48000, 2, 99);
  const auto pcm = to_float(ref);

  REQUIRE(enc->open({be::Codec::Flac, 0, 48000}, &err));
  const auto first = encode_all(*enc, pcm, 2);
  // reconnect without a flush in between (the stream was cut), and once more after a flush
  REQUIRE(enc->open({be::Codec::Flac, 0, 48000}, &err));
  std::vector<uint8_t> cut;
  enc->encode(pcm.data(), 1000, cut);
  REQUIRE(enc->open({be::Codec::Flac, 0, 48000}, &err));
  const auto second = encode_all(*enc, pcm, 2);

  REQUIRE(std::memcmp(first.data(), "OggS", 4) == 0);
  REQUIRE(std::memcmp(second.data(), "OggS", 4) == 0);
  CHECK(second[5] == 0x02);  // header type: beginning of stream
  CHECK(ogg_serial(first) != ogg_serial(second));
  const auto dec = decode_ogg_flac(second);
  REQUIRE(dec.samples.size() == ref.size());
  CHECK(std::equal(ref.begin(), ref.end(), dec.samples.begin()));
}

// ---------------------------------------------------------------------------------------------
TEST_CASE("encoders: MP3 128 kb/s at 44.1 kHz, CBR frames, decoded sine level and correlation") {
  SKIP_UNLESS_AVAILABLE(be::Codec::Mp3);
  const uint32_t rate = 44100;
  const auto input = stereo_sine(rate, 3.0, 1000.0, 1500.0, 0.5);
  auto enc = be::make_encoder(be::Codec::Mp3);
  std::string err;
  REQUIRE_MESSAGE(enc->open({be::Codec::Mp3, 128, rate}, &err), err);
  const auto bytes = encode_all(*enc, input, 2);

  const auto frames = parse_mp3(bytes);
  CHECK(frames.all_valid);
  CHECK(frames.mpeg_version == 1);
  CHECK(frames.bitrates == std::set<uint32_t>{128});
  CHECK(frames.rates == std::set<uint32_t>{rate});
  // CBR: 144 * 128000 / 44100 = 417.96 -> 417 bytes, 418 with the padding slot
  CHECK(frames.min_size >= 417);
  CHECK(frames.max_size <= 418);
  // 3 s of audio = 114.8 frames of 1152 samples (+ the encoder delay and flush)
  CHECK(frames.count >= 115);
  CHECK(frames.count <= 118);
  const double kbps = double(bytes.size()) * 8 / (double(frames.count) * 1152 / rate) / 1000;
  MESSAGE("MP3: " << frames.count << " frames, " << bytes.size() << " bytes, " << kbps << " kb/s");
  CHECK(kbps == doctest::Approx(128).epsilon(0.002));

  const auto pcm = decode_mp3(bytes);
  // LAME: encoder delay ENCDELAY (576) + mpglib decoder delay (528 + 1), libmp3lame/encoder.h
  const size_t delay = 576 + 528 + 1;
  const size_t from = rate / 2, n = rate * 2;  // skip the start, 2 s window
  for (int ch = 0; ch < 2; ++ch) {
    CAPTURE(ch);
    const auto ref = channel(input, 2, ch);
    const auto dec = channel(pcm, 2, ch);
    REQUIRE(dec.size() >= from + n + delay);
    const double level_in = rms_db(ref, from, n), level_out = rms_db(dec, from + delay, n);
    const double corr = correlation(ref, dec, from, n, delay);
    MESSAGE("MP3 ch" << ch << ": level in " << level_in << " dB, out " << level_out << " dB, correlation " << corr
                     << " at delay " << delay << ", best lag in 0..3000 " << best_lag(ref, dec, from, n, 0, 3000));
    CHECK(std::fabs(level_out - level_in) < 1.0);
    CHECK(corr > 0.99);
  }
}

// ---------------------------------------------------------------------------------------------
namespace {
struct AacCase {
  be::Codec codec;
  uint32_t kbps;
  int adts_profile;        // audio object type - 1 in the ADTS header
  int adts_rate_index;     // sampling frequency index in the header
  int adts_channels;       // channel configuration in the header
  int core_sample_rate;    // decoder: AAC core rate (SBR doubles it)
  int core_channels;       // decoder: channels before PS
};

void check_aac(const AacCase& c) {
  const uint32_t rate = 48000;
  const bool ps = c.codec == be::Codec::HeAacV2;
  // HE-AACv2 codes stereo parametrically: the same tone in both channels, compared as mono sum
  const auto input = stereo_sine(rate, 3.0, 1000.0, ps ? 1000.0 : 1500.0, 0.5);
  auto enc = be::make_encoder(c.codec);
  REQUIRE(enc);
  std::string err;
  REQUIRE_MESSAGE(enc->open({c.codec, c.kbps, rate}, &err), err);
  const auto bytes = encode_all(*enc, input, 2);

  const auto adts = parse_adts(bytes);
  CHECK(adts.all_valid);
  CHECK(adts.profiles == std::set<int>{c.adts_profile});
  CHECK(adts.rate_indices == std::set<int>{c.adts_rate_index});
  CHECK(adts.channel_configs == std::set<int>{c.adts_channels});
  const double seconds = double(adts.count) * (c.codec == be::Codec::AacLc ? 1024.0 : 2048.0) / rate;
  const double kbps = double(bytes.size()) * 8 / seconds / 1000;
  const std::string name = be::codec_name(c.codec);
  MESSAGE(name << ": " << adts.count << " ADTS frames, " << bytes.size() << " bytes, " << kbps << " kb/s");
  CHECK(kbps == doctest::Approx(double(c.kbps)).epsilon(0.05));

  const auto d = decode_aac(bytes);
  CHECK(d.sample_rate == int(rate));
  CHECK(d.channels == 2);
  CHECK(d.core_sample_rate == c.core_sample_rate);
  CHECK(d.core_channels == c.core_channels);
  REQUIRE(d.channels == 2);
  // codec delay: encoder (fdk-aac nDelay, includes the SBR/PS delay) + what the decoder adds
  const UINT aot = c.codec == be::Codec::AacLc ? AOT_AAC_LC : c.codec == be::Codec::HeAac ? AOT_SBR : AOT_PS;
  const size_t delay = fdk_encoder_delay(aot, rate, c.kbps * 1000) + size_t(d.output_delay);
  std::vector<std::vector<double>> refs, decs;
  if (ps) {
    refs.push_back(mono_sum(input));
    decs.push_back(mono_sum(d.pcm));
  } else {
    for (int ch = 0; ch < 2; ++ch) {
      refs.push_back(channel(input, 2, ch));
      decs.push_back(channel(d.pcm, 2, ch));
    }
  }
  const size_t from = rate / 2, n = rate * 2;
  for (size_t k = 0; k < refs.size(); ++k) {
    CAPTURE(k);
    const auto& ref = refs[k];
    const auto& dec = decs[k];
    // SBR's QMF delay is not an integer number of output samples: the best lag within ±8
    const size_t lag = best_lag(ref, dec, from, n, delay - 8, delay + 8);
    REQUIRE(dec.size() >= from + n + lag);
    const double level_in = rms_db(ref, from, n), level_out = rms_db(dec, from + lag, n);
    const double corr = correlation(ref, dec, from, n, lag);
    const std::string what = ps ? "mono sum" : k ? "right" : "left";
    MESSAGE(name << " " << what << ": level in " << level_in << " dB, out "
                 << level_out << " dB, correlation " << corr << " at lag " << lag << " (expected delay " << delay
                 << " = encoder " << delay - size_t(d.output_delay) << " + decoder " << d.output_delay << ")");
    CHECK(std::labs(long(lag) - long(delay)) <= 4);
    CHECK(std::fabs(level_out - level_in) < 1.5);
    CHECK(corr > 0.95);
  }
}
}  // namespace

TEST_CASE("encoders: AAC-LC 128 kb/s at 48 kHz, ADTS, decoded level and correlation") {
  SKIP_UNLESS_AVAILABLE(be::Codec::AacLc);
  check_aac({be::Codec::AacLc, 128, 1, 3, 2, 48000, 2});
}

TEST_CASE("encoders: HE-AAC 64 kb/s at 48 kHz, ADTS, decoded level and correlation") {
  SKIP_UNLESS_AVAILABLE(be::Codec::HeAac);
  // implicit SBR signalling in ADTS: the header carries AAC-LC at the 24 kHz core rate
  check_aac({be::Codec::HeAac, 64, 1, 6, 2, 24000, 2});
}

TEST_CASE("encoders: HE-AACv2 32 kb/s at 48 kHz, ADTS, decoded mono sum level and correlation") {
  SKIP_UNLESS_AVAILABLE(be::Codec::HeAacV2);
  // implicit SBR + PS signalling in ADTS: AAC-LC, 24 kHz core, mono core channel
  check_aac({be::Codec::HeAacV2, 32, 1, 6, 1, 24000, 1});
}

#endif  // BAM_WITH_CODECS
