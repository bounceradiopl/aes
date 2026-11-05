// SPDX-License-Identifier: GPL-3.0-or-later
// FLAC in Ogg (libFLAC + libogg, linked in): level 5, 16 bit. Each open() starts a new Ogg
// logical stream (new random serial number, new headers), e.g. after an Icecast reconnect.
//
// libFLAC encodes the native stream; the Ogg FLAC mapping (https://xiph.org/flac/ogg_mapping.html)
// is done here with libogg, because libFLAC's own Ogg layer only emits a page once ~4 KB are
// pending: digital silence (a FLAC frame of ~10 bytes) would then leave about every 22 s, longer
// than Icecast's default 10 s source timeout (ruling E8). Here a page is forced as soon as 250 ms
// of audio wait in the Ogg stream.
//   - first packet: 0x7F "FLAC" 0x01 0x00 <header packets, u16 BE> "fLaC" + STREAMINFO block;
//   - each further metadata block (libFLAC writes one VORBIS_COMMENT, flagged last) is a packet;
//     the header pages are flushed before any audio;
//   - one packet per FLAC frame, granulepos = samples at the end of that frame; the frame
//     written by flush() ends the stream (e_o_s).
#include <FLAC/stream_encoder.h>
#include <ogg/ogg.h>

#include <cstring>
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

// The metadata blocks libFLAC writes after STREAMINFO when given none: one VORBIS_COMMENT.
constexpr uint16_t kHeaderPackets = 1;

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
    if (ogg_stream_init(&os_, int(serial_)) != 0) {
      FLAC__stream_encoder_delete(enc);
      if (error) *error = "nie udało się utworzyć strumienia Ogg";
      return false;
    }
    ogg_open_ = true;
    seen_magic_ = false;
    first_packet_ = true;
    finishing_ = false;
    packetno_ = 0;
    samples_ = 0;
    paged_samples_ = 0;
    page_every_ = s.sample_rate / 4;  // 250 ms
    FLAC__stream_encoder_set_channels(enc, unsigned(s.channels));
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, s.sample_rate);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_streamable_subset(enc, true);
    pending_.clear();
    const FLAC__StreamEncoderInitStatus st =
        FLAC__stream_encoder_init_stream(enc, &FlacEncoder::write_cb, nullptr, nullptr, nullptr, this);
    if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
      FLAC__stream_encoder_delete(enc);
      close();
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
    finishing_ = true;  // the frame libFLAC writes now is the last one
    if (!FLAC__stream_encoder_finish(enc_)) return fail_encode();
    if (!finished_stream_) {
      // nothing was left to encode: end the stream with an empty last packet
      ogg_packet p{};
      p.e_o_s = 1;
      p.granulepos = ogg_int64_t(samples_);
      p.packetno = packetno_++;
      if (ogg_stream_packetin(&os_, &p) != 0) return fail_encode();
    }
    drain(true);
    take(out);
    close();
  }

  const char* content_type() const override { return codec_content_type(Codec::Flac); }

 private:
  static FLAC__StreamEncoderWriteStatus write_cb(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
                                                 uint32_t samples, uint32_t, void* client) {
    auto* self = static_cast<FlacEncoder*>(client);
    if (self->take_injected_failure()) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    const bool ok = samples == 0 ? self->on_metadata(buffer, bytes) : self->on_frame(buffer, bytes, samples);
    return ok ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
  }

  // The "fLaC" marker, then one metadata block per call (header byte, 3 length bytes, body).
  bool on_metadata(const FLAC__byte* buffer, size_t bytes) {
    if (!seen_magic_) {
      seen_magic_ = bytes == 4 && std::memcmp(buffer, "fLaC", 4) == 0;
      return seen_magic_;
    }
    ogg_packet p{};
    if (first_packet_) {
      if ((buffer[0] & 0x7F) != FLAC__METADATA_TYPE_STREAMINFO) return false;
      first_.assign({0x7F, 'F', 'L', 'A', 'C', 1, 0, uint8_t(kHeaderPackets >> 8), uint8_t(kHeaderPackets & 0xFF), 'f', 'L', 'a', 'C'});
      first_.insert(first_.end(), buffer, buffer + bytes);
      p.packet = first_.data();
      p.bytes = long(first_.size());
      p.b_o_s = 1;
      first_packet_ = false;
    } else {
      p.packet = const_cast<unsigned char*>(buffer);
      p.bytes = long(bytes);
    }
    p.granulepos = 0;
    p.packetno = packetno_++;
    if (ogg_stream_packetin(&os_, &p) != 0) return false;
    drain(true);  // header pages go out at once, before any audio
    return true;
  }

  bool on_frame(const FLAC__byte* buffer, size_t bytes, uint32_t samples) {
    samples_ += samples;
    ogg_packet p{};
    p.packet = const_cast<unsigned char*>(buffer);
    p.bytes = long(bytes);
    p.e_o_s = finishing_ ? 1 : 0;
    p.granulepos = ogg_int64_t(samples_);
    p.packetno = packetno_++;
    if (ogg_stream_packetin(&os_, &p) != 0) return false;
    if (finishing_) finished_stream_ = true;
    drain(false);
    if (samples_ - paged_samples_ >= page_every_) drain(true);
    return true;
  }

  // Moves finished pages (all pending data when `force`) to pending_.
  void drain(bool force) {
    ogg_page page;
    while (force ? ogg_stream_flush(&os_, &page) != 0 : ogg_stream_pageout(&os_, &page) != 0) {
      pending_.insert(pending_.end(), page.header, page.header + page.header_len);
      pending_.insert(pending_.end(), page.body, page.body + page.body_len);
      const ogg_int64_t g = ogg_page_granulepos(&page);
      if (g > 0) paged_samples_ = uint64_t(g);
    }
  }

  // Drops the pending bytes and the encoder; failed() until the next open().
  void fail_encode() {
    const std::string state = FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(enc_)];
    close();
    set_failed("błąd kodowania FLAC (" + state + ")");
  }

  size_t buffered_bytes() const override { return pending_.size(); }

  // Bytes of finished pages (the header pages already during open) go out with the next call.
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
    if (ogg_open_) {
      ogg_stream_clear(&os_);
      ogg_open_ = false;
    }
    finished_stream_ = false;
    pending_.clear();
  }

  FLAC__StreamEncoder* enc_ = nullptr;
  int channels_ = 2;
  long serial_ = 0;
  std::vector<FLAC__int32> pcm32_;
  std::vector<uint8_t> pending_;  // finished Ogg pages not handed out yet

  ogg_stream_state os_{};
  bool ogg_open_ = false;
  bool seen_magic_ = false;
  bool first_packet_ = true;
  bool finishing_ = false;
  bool finished_stream_ = false;  // the e_o_s packet is in
  ogg_int64_t packetno_ = 0;
  uint64_t samples_ = 0;        // samples per channel in the frames so far
  uint64_t paged_samples_ = 0;  // ... of which are in pages handed to pending_
  uint64_t page_every_ = 12000;
  std::vector<uint8_t> first_;  // the first packet's bytes (libogg copies them on packetin)
};

}  // namespace

std::unique_ptr<AudioEncoder> make_flac_encoder() { return std::make_unique<FlacEncoder>(); }

}  // namespace bam::encoder::detail
