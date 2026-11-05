// SPDX-License-Identifier: GPL-3.0-or-later
// AAC-LC / HE-AAC / HE-AACv2 through fdk-aac, loaded at run time (spec §5.3): CBR, ADTS,
// afterburner on, 16-bit PCM input converted from float with saturation.
#include <aacenc_lib.h>

#include <algorithm>

#include "bam/platform/dynlib.hpp"
#include "codec_impl.hpp"

namespace bam::encoder::detail {

struct FdkApi {
  platform::DynamicLibrary lib;
  decltype(&::aacEncOpen) open = nullptr;
  decltype(&::aacEncClose) close = nullptr;
  decltype(&::aacEncEncode) encode = nullptr;
  decltype(&::aacEncInfo) info = nullptr;
  decltype(&::aacEncoder_SetParam) set_param = nullptr;
  decltype(&::aacEncoder_GetParam) get_param = nullptr;
};

namespace {

template <typename Fn>
bool resolve(const platform::DynamicLibrary& lib, const char* name, Fn& fn) {
  fn = reinterpret_cast<Fn>(lib.symbol(name));
  return fn != nullptr;
}

std::string hex(unsigned v) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  do {
    s.insert(s.begin(), digits[v & 0xF]);
    v >>= 4;
  } while (v);
  return "0x" + s;
}

class FdkEncoder final : public AudioEncoder {
 public:
  FdkEncoder(Codec codec, std::shared_ptr<const FdkApi> api) : codec_(codec), api_(std::move(api)) {}
  ~FdkEncoder() override { close(); }

  bool open(const EncoderSettings& s, std::string* error) override {
    close();
    clear_failed();
    if (!check_open_settings(codec_, s, error)) return false;
    const std::string label = codec_label(codec_);
    HANDLE_AACENCODER h = nullptr;
    if (api_->open(&h, 0, UINT(s.channels)) != AACENC_OK || !h) {
      if (error) *error = "nie udało się utworzyć kodera " + label + " (fdk-aac)";
      return false;
    }
    const UINT aot = codec_ == Codec::AacLc ? AOT_AAC_LC : codec_ == Codec::HeAac ? AOT_SBR : AOT_PS;
    const struct {
      AACENC_PARAM param;
      UINT value;
    } params[] = {
        {AACENC_AOT, aot},
        {AACENC_SAMPLERATE, s.sample_rate},
        {AACENC_CHANNELMODE, MODE_2},
        {AACENC_CHANNELORDER, 1},  // WAV order: L, R
        {AACENC_BITRATEMODE, 0},   // CBR
        {AACENC_BITRATE, s.bitrate_kbps * 1000},
        {AACENC_TRANSMUX, TT_MP4_ADTS},
        {AACENC_AFTERBURNER, 1},
    };
    for (const auto& p : params) {
      const AACENC_ERROR e = api_->set_param(h, p.param, p.value);
      if (e != AACENC_OK) {
        api_->close(&h);
        if (error) *error = "fdk-aac odrzucił ustawienia " + label + " (parametr " + hex(unsigned(p.param)) +
                            ", kod " + hex(unsigned(e)) + ")";
        return false;
      }
    }
    // a call without buffers applies the parameters
    const AACENC_ERROR e = api_->encode(h, nullptr, nullptr, nullptr, nullptr);
    AACENC_InfoStruct info{};
    if (e != AACENC_OK || api_->info(h, &info) != AACENC_OK) {
      api_->close(&h);
      if (error) *error = "fdk-aac odrzucił ustawienia " + label + " (kod " + hex(unsigned(e)) + ")";
      return false;
    }
    // fdk-aac lowers a bitrate it cannot reach instead of failing; the stream must carry the
    // configured one (validate_encoder_settings already keeps to what fdk-aac reaches)
    const UINT bitrate = api_->get_param(h, AACENC_BITRATE);
    if (bitrate != s.bitrate_kbps * 1000) {
      api_->close(&h);
      if (error)
        *error = "fdk-aac nie obsługuje bitrate " + std::to_string(s.bitrate_kbps) + " kb/s dla " + label + " przy " +
                 std::to_string(s.sample_rate) + " Hz (proponuje " + std::to_string(bitrate / 1000) + " kb/s)";
      return false;
    }
    h_ = h;
    channels_ = s.channels;
    out_buf_.resize(std::max<size_t>(info.maxOutBufBytes, 8192));
    pcm_.clear();
    return true;
  }

  void encode(const float* pcm, size_t frames, std::vector<uint8_t>& out) override {
    if (!h_) return;
    const size_t n = frames * size_t(channels_);
    pcm_.reserve(pcm_.size() + n);
    for (size_t i = 0; i < n; ++i) pcm_.push_back(INT_PCM(float_to_pcm16(pcm[i])));
    run(false, out);
  }

  void flush(std::vector<uint8_t>& out) override {
    if (!h_) return;
    run(true, out);
    close();
  }

  const char* content_type() const override { return codec_content_type(codec_); }

 private:
  // Feeds the pending PCM; fdk-aac takes what fits in its frame buffer and returns at most one
  // access unit per call. With `eof` the encoder is drained until it reports the end.
  void run(bool eof, std::vector<uint8_t>& out) {
    size_t offset = 0;
    for (;;) {
      const size_t remaining = pcm_.size() - offset;
      if (!eof && remaining == 0) break;
      void* in_ptr = pcm_.data() + offset;
      INT in_id = IN_AUDIO_DATA, in_size = INT(remaining * sizeof(INT_PCM)), in_el = INT(sizeof(INT_PCM));
      void* out_ptr = out_buf_.data();
      INT out_id = OUT_BITSTREAM_DATA, out_size = INT(out_buf_.size()), out_el = 1;
      AACENC_BufDesc in_desc{1, &in_ptr, &in_id, &in_size, &in_el};
      AACENC_BufDesc out_desc{1, &out_ptr, &out_id, &out_size, &out_el};
      AACENC_InArgs in_args{eof ? -1 : INT(remaining), 0};
      AACENC_OutArgs out_args{};
      const AACENC_ERROR e = take_injected_failure() ? AACENC_ENCODE_ERROR
                                                     : api_->encode(h_, &in_desc, &out_desc, &in_args, &out_args);
      if (eof && e == AACENC_ENCODE_EOF) break;  // drained
      if (e != AACENC_OK) return fail_encode("kod " + hex(unsigned(e)));
      offset += size_t(out_args.numInSamples);
      if (out_args.numOutBytes > 0) out.insert(out.end(), out_buf_.begin(), out_buf_.begin() + out_args.numOutBytes);
      if (!eof && out_args.numInSamples == 0 && out_args.numOutBytes == 0)
        return fail_encode("koder nie przyjmuje danych");  // would otherwise buffer without bound
    }
    pcm_.erase(pcm_.begin(), pcm_.begin() + std::ptrdiff_t(offset));
  }

  // Drops the buffered PCM and the encoder; failed() until the next open().
  void fail_encode(const std::string& detail) {
    close();
    set_failed("błąd kodowania " + std::string(codec_label(codec_)) + " (fdk-aac, " + detail + ")");
  }

  size_t buffered_bytes() const override { return pcm_.size() * sizeof(INT_PCM); }

  void close() {
    if (h_) api_->close(&h_);
    h_ = nullptr;
    pcm_.clear();
  }

  Codec codec_;
  std::shared_ptr<const FdkApi> api_;
  HANDLE_AACENCODER h_ = nullptr;
  int channels_ = 2;
  std::vector<INT_PCM> pcm_;  // converted input not yet taken by the encoder
  std::vector<uint8_t> out_buf_;
};

}  // namespace

std::shared_ptr<const FdkApi> load_fdk(const std::vector<std::string>& dirs) {
#if defined(_WIN32)
  const std::vector<std::string> names = {"libfdk-aac-2.dll"};
#elif defined(__APPLE__)
  const std::vector<std::string> names = {"libfdk-aac.dylib", "libfdk-aac.2.dylib"};
#else
  const std::vector<std::string> names = {"libfdk-aac.so", "libfdk-aac.so.2"};
#endif
  auto lib = platform::DynamicLibrary::open(library_candidates(dirs, names), nullptr);
  if (!lib) return nullptr;
  auto api = std::make_shared<FdkApi>();
  api->lib = std::move(*lib);
  const auto& l = api->lib;
  const bool ok = resolve(l, "aacEncOpen", api->open) && resolve(l, "aacEncClose", api->close) &&
                  resolve(l, "aacEncEncode", api->encode) && resolve(l, "aacEncInfo", api->info) &&
                  resolve(l, "aacEncoder_SetParam", api->set_param) &&
                  resolve(l, "aacEncoder_GetParam", api->get_param);
  if (!ok) return nullptr;
  return api;
}

std::unique_ptr<AudioEncoder> make_fdk_encoder(Codec codec, std::shared_ptr<const FdkApi> api) {
  return std::make_unique<FdkEncoder>(codec, std::move(api));
}

}  // namespace bam::encoder::detail
