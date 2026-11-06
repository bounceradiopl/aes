// SPDX-License-Identifier: GPL-3.0-or-later
// MP3 through LAME, loaded at run time (LGPL, spec §5.3): CBR, quality 2, joint stereo.
#include <lame.h>

#include <algorithm>
#include <cstdarg>

#include "bam/platform/dynlib.hpp"
#include "codec_impl.hpp"

namespace bam::encoder::detail {

struct LameApi {
  platform::DynamicLibrary lib;
  decltype(&::lame_init) init = nullptr;
  decltype(&::lame_close) close = nullptr;
  decltype(&::lame_set_errorf) set_errorf = nullptr;
  decltype(&::lame_set_debugf) set_debugf = nullptr;
  decltype(&::lame_set_msgf) set_msgf = nullptr;
  decltype(&::lame_set_in_samplerate) set_in_samplerate = nullptr;
  decltype(&::lame_set_out_samplerate) set_out_samplerate = nullptr;
  decltype(&::lame_set_num_channels) set_num_channels = nullptr;
  decltype(&::lame_set_mode) set_mode = nullptr;
  decltype(&::lame_set_VBR) set_vbr = nullptr;
  decltype(&::lame_set_brate) set_brate = nullptr;
  decltype(&::lame_set_quality) set_quality = nullptr;
  decltype(&::lame_set_bWriteVbrTag) set_write_vbr_tag = nullptr;
  decltype(&::lame_init_params) init_params = nullptr;
  decltype(&::lame_encode_buffer_interleaved_ieee_float) encode_interleaved = nullptr;
  decltype(&::lame_encode_flush) flush = nullptr;
};

namespace {

template <typename Fn>
bool resolve(const platform::DynamicLibrary& lib, const char* name, Fn& fn) {
  fn = reinterpret_cast<Fn>(lib.symbol(name));
  return fn != nullptr;
}

void quiet(const char*, va_list) {}

constexpr size_t kChunkFrames = 1152 * 4;

class LameEncoder final : public AudioEncoder {
 public:
  explicit LameEncoder(std::shared_ptr<const LameApi> api) : api_(std::move(api)) {}
  ~LameEncoder() override { close(); }

  bool open(const EncoderSettings& s, std::string* error) override {
    close();
    clear_failed();
    if (!check_open_settings(Codec::Mp3, s, error)) return false;
    lame_t gf = api_->init();
    if (!gf) {
      if (error) *error = "nie udało się utworzyć kodera MP3 (LAME)";
      return false;
    }
    api_->set_errorf(gf, quiet);
    api_->set_debugf(gf, quiet);
    api_->set_msgf(gf, quiet);
    api_->set_in_samplerate(gf, int(s.sample_rate));
    api_->set_out_samplerate(gf, int(s.sample_rate));  // never resample inside LAME
    api_->set_num_channels(gf, s.channels);
    api_->set_mode(gf, JOINT_STEREO);
    api_->set_vbr(gf, vbr_off);
    api_->set_brate(gf, int(s.bitrate_kbps));
    api_->set_quality(gf, 2);
    api_->set_write_vbr_tag(gf, 0);  // a stream has no place for the Xing/Info frame
    const int rc = api_->init_params(gf);
    if (rc < 0) {
      api_->close(gf);
      if (error) *error = "LAME odrzucił ustawienia MP3 (kod " + std::to_string(rc) + ")";
      return false;
    }
    gf_ = gf;
    return true;
  }

  void encode(const float* pcm, size_t frames, std::vector<uint8_t>& out) override {
    if (!gf_) return;
    while (frames > 0) {
      const size_t n = std::min(frames, kChunkFrames);
      // saturate like the 16-bit codecs: LAME would otherwise encode the overshoot
      clipped_.resize(n * 2);
      for (size_t i = 0; i < clipped_.size(); ++i)
        clipped_[i] = pcm[i] != pcm[i] ? 0.0f : std::clamp(pcm[i], -1.0f, 1.0f);  // NaN -> silence
      mp3_.resize(n * 5 / 4 + 7200);  // worst case from lame.h
      const int got = take_injected_failure()
                          ? -1
                          : api_->encode_interleaved(gf_, clipped_.data(), int(n), mp3_.data(), int(mp3_.size()));
      if (got < 0) return fail_encode(got);
      out.insert(out.end(), mp3_.begin(), mp3_.begin() + got);
      pcm += n * 2;
      frames -= n;
    }
  }

  void flush(std::vector<uint8_t>& out) override {
    if (!gf_) return;
    mp3_.resize(7200);
    const int got = take_injected_failure() ? -1 : api_->flush(gf_, mp3_.data(), int(mp3_.size()));
    if (got < 0) return fail_encode(got);
    out.insert(out.end(), mp3_.begin(), mp3_.begin() + got);
    close();
  }

  const char* content_type() const override { return codec_content_type(Codec::Mp3); }

 private:
  void fail_encode(int code) {
    close();
    set_failed("błąd kodowania MP3 (LAME, kod " + std::to_string(code) + ")");
  }

  void close() {
    if (gf_) api_->close(gf_);
    gf_ = nullptr;
  }

  std::shared_ptr<const LameApi> api_;
  lame_t gf_ = nullptr;
  std::vector<float> clipped_;
  std::vector<unsigned char> mp3_;
};

}  // namespace

const std::string& library_path(const LameApi& api) { return api.lib.path(); }

std::shared_ptr<const LameApi> load_lame(const std::vector<std::string>& dirs) {
#if defined(_WIN32)
  const std::vector<std::string> names = {"libmp3lame.dll", "libmp3lame-0.dll"};
#elif defined(__APPLE__)
  const std::vector<std::string> names = {"libmp3lame.dylib", "libmp3lame.0.dylib"};
#else
  const std::vector<std::string> names = {"libmp3lame.so", "libmp3lame.so.0"};
#endif
  auto lib = platform::DynamicLibrary::open(library_candidates(dirs, names), nullptr);
  if (!lib) return nullptr;
  auto api = std::make_shared<LameApi>();
  api->lib = std::move(*lib);
  const auto& l = api->lib;
  const bool ok = resolve(l, "lame_init", api->init) && resolve(l, "lame_close", api->close) &&
                  resolve(l, "lame_set_errorf", api->set_errorf) && resolve(l, "lame_set_debugf", api->set_debugf) &&
                  resolve(l, "lame_set_msgf", api->set_msgf) &&
                  resolve(l, "lame_set_in_samplerate", api->set_in_samplerate) &&
                  resolve(l, "lame_set_out_samplerate", api->set_out_samplerate) &&
                  resolve(l, "lame_set_num_channels", api->set_num_channels) &&
                  resolve(l, "lame_set_mode", api->set_mode) && resolve(l, "lame_set_VBR", api->set_vbr) &&
                  resolve(l, "lame_set_brate", api->set_brate) && resolve(l, "lame_set_quality", api->set_quality) &&
                  resolve(l, "lame_set_bWriteVbrTag", api->set_write_vbr_tag) &&
                  resolve(l, "lame_init_params", api->init_params) &&
                  resolve(l, "lame_encode_buffer_interleaved_ieee_float", api->encode_interleaved) &&
                  resolve(l, "lame_encode_flush", api->flush);
  if (!ok) return nullptr;
  return api;
}

std::unique_ptr<AudioEncoder> make_lame_encoder(std::shared_ptr<const LameApi> api) {
  return std::make_unique<LameEncoder>(std::move(api));
}

}  // namespace bam::encoder::detail
