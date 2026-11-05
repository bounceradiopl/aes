// SPDX-License-Identifier: GPL-3.0-or-later
// FLAC in Ogg (libFLAC + libogg, linked in): level 5, 16 bit. Each open() starts a new Ogg
// logical stream (new random serial number, new headers), e.g. after an Icecast reconnect.
#include <FLAC/stream_encoder.h>

#include <mutex>
#include <random>

#include "codec_impl.hpp"

namespace bam::encoder::detail {

namespace {

long new_serial(long previous) {
  static std::mutex mutex;
  static std::mt19937 rng{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  std::uniform_int_distribution<long> dist(1, 0x7FFFFFFF);
  long s = dist(rng);
  while (s == previous) s = dist(rng);
  return s;
}

class FlacEncoder final : public AudioEncoder {
 public:
  ~FlacEncoder() override { close(); }

  bool open(const EncoderSettings& s, std::string* error) override {
    close();
    clear_failed();
    if (!check_open_settings(Codec::Flac, s, error)) return false;
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc) {
      if (error) *error = "nie udało się utworzyć kodera FLAC";
      return false;
    }
    serial_ = new_serial(serial_);
    FLAC__stream_encoder_set_channels(enc, unsigned(s.channels));
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, s.sample_rate);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_streamable_subset(enc, true);
    FLAC__stream_encoder_set_ogg_serial_number(enc, serial_);
    pending_.clear();
    const FLAC__StreamEncoderInitStatus st =
        FLAC__stream_encoder_init_ogg_stream(enc, nullptr, &FlacEncoder::write_cb, nullptr, nullptr, nullptr, this);
    if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
      FLAC__stream_encoder_delete(enc);
      pending_.clear();
      if (error)
        *error = std::string("nie udało się uruchomić kodera FLAC (") + FLAC__StreamEncoderInitStatusString[st] + ")";
      return false;
    }
    enc_ = enc;
    channels_ = s.channels;
    return true;
  }

  void encode(const float* pcm, size_t frames, std::vector<uint8_t>& out) override {
    if (!enc_) return;
    pcm32_.resize(frames * size_t(channels_));
    for (size_t i = 0; i < pcm32_.size(); ++i) pcm32_[i] = float_to_pcm16(pcm[i]);
    if (frames > 0 && !FLAC__stream_encoder_process_interleaved(enc_, pcm32_.data(), unsigned(frames)))
      return fail_encode();
    take(out);
  }

  void flush(std::vector<uint8_t>& out) override {
    if (!enc_) return;
    if (!FLAC__stream_encoder_finish(enc_)) return fail_encode();  // last frame and the end-of-stream page
    take(out);
    close();
  }

  const char* content_type() const override { return codec_content_type(Codec::Flac); }

 private:
  static FLAC__StreamEncoderWriteStatus write_cb(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
                                                 uint32_t, uint32_t, void* client) {
    auto* self = static_cast<FlacEncoder*>(client);
    if (self->take_injected_failure()) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    self->pending_.insert(self->pending_.end(), buffer, buffer + bytes);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }

  // Drops the pending bytes and the encoder; failed() until the next open().
  void fail_encode() {
    const std::string state = FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(enc_)];
    close();
    set_failed("błąd kodowania FLAC (" + state + ")");
  }

  size_t buffered_bytes() const override { return pending_.size(); }

  // Bytes written by libFLAC (the headers already during open) go out with the next call.
  void take(std::vector<uint8_t>& out) {
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
  }

  void close() {
    if (enc_) {
      // an unfinished stream is simply dropped (the connection it belonged to is gone)
      FLAC__stream_encoder_delete(enc_);
      enc_ = nullptr;
    }
    pending_.clear();
  }

  FLAC__StreamEncoder* enc_ = nullptr;
  int channels_ = 2;
  long serial_ = 0;
  std::vector<FLAC__int32> pcm32_;
  std::vector<uint8_t> pending_;
};

}  // namespace

std::unique_ptr<AudioEncoder> make_flac_encoder() { return std::make_unique<FlacEncoder>(); }

}  // namespace bam::encoder::detail
