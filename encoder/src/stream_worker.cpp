// SPDX-License-Identifier: GPL-3.0-or-later
// One Icecast stream of a station (spec §4.4): the bounded queue from the station thread, the
// worker thread (stream gain + resampling, encoder) and the IcecastClient. Stream-start contract
// (ruling E6): MP3/AAC mark every push stream_start; FLAC/Ogg marks the header push after each
// encoder (re)open. When the client needs a new stream (on_reconnect, client thread) it only sets
// a flag; the worker reopens its encoder on its own thread.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "bam/gain.hpp"
#include "bam/log.hpp"
#include "bam/platform/time.hpp"
#include "codec_impl.hpp"
#include "engine_internal.hpp"

namespace bam::encoder::detail {

namespace {
constexpr size_t kChunkFrames = 2400;               // 50 ms of the mix per encoder call at most
constexpr int kPopWaitMs = 20;                      // the worker looks at its flags at least this often
constexpr int64_t kReopenAfterErrorNs = 1'000'000'000;  // a failed encoder is tried again after 1 s
constexpr size_t kMinIcecastQueue = 64 * 1024;      // ruling E6: a start piece must always fit

const char* library_name(Codec c) {
  return c == Codec::Mp3 ? "libmp3lame" : "libfdk-aac";
}
}  // namespace

// ---- AudioQueue ----

AudioQueue::AudioQueue(size_t capacity_frames) : ring_(capacity_frames * 2), capacity_(capacity_frames) {}

void AudioQueue::push(const float* stereo, size_t frames) {
  if (capacity_ == 0 || frames == 0) return;
  {
    std::lock_guard lock(m_);
    if (frames > capacity_) {  // only the newest capacity_ frames can stay
      dropped_.fetch_add(frames - capacity_, std::memory_order_relaxed);
      stereo += (frames - capacity_) * 2;
      frames = capacity_;
    }
    if (size_ + frames > capacity_) {  // the stream fell behind: drop the oldest
      const size_t over = size_ + frames - capacity_;
      head_ = (head_ + over) % capacity_;
      size_ -= over;
      dropped_.fetch_add(over, std::memory_order_relaxed);
    }
    size_t tail = (head_ + size_) % capacity_;
    const size_t first = std::min(frames, capacity_ - tail);
    std::memcpy(ring_.data() + tail * 2, stereo, first * 2 * sizeof(float));
    if (first < frames) std::memcpy(ring_.data(), stereo + first * 2, (frames - first) * 2 * sizeof(float));
    size_ += frames;
  }
  cv_.notify_one();
}

size_t AudioQueue::pop(float* out, size_t max_frames, int wait_ms) {
  std::unique_lock lock(m_);
  cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return size_ > 0 || woken_; });
  const size_t n = std::min(max_frames, size_);
  const size_t first = std::min(n, capacity_ - head_);
  std::memcpy(out, ring_.data() + head_ * 2, first * 2 * sizeof(float));
  if (first < n) std::memcpy(out + first * 2, ring_.data(), (n - first) * 2 * sizeof(float));
  head_ = n == 0 ? head_ : (head_ + n) % capacity_;
  size_ -= n;
  return n;
}

size_t AudioQueue::size_frames() const {
  std::lock_guard lock(m_);
  return size_;
}

void AudioQueue::wake() {
  {
    std::lock_guard lock(m_);
    woken_ = true;
  }
  cv_.notify_all();
}

// ---- StreamWorker ----

std::string stream_key(const StreamConfig& s, const std::string& station_name) {
  // everything but the gain (applied live); a lossless stream has no bitrate
  const bool lossless = codec_limits(s.codec).lossless;
  return std::string(codec_name(s.codec)) + '|' + (lossless ? std::string("-") : std::to_string(s.bitrate_kbps)) + '|' +
         std::to_string(s.sample_rate) + '|' + s.host + '|' + std::to_string(s.port) + '|' + s.mount + '|' + s.user + '|' +
         s.password + '|' + station_name;
}

size_t icecast_queue_limit(const StreamConfig& s) {
  const size_t five_s = codec_limits(s.codec).lossless ? size_t(s.sample_rate) * 2 * 2 * 5  // 16-bit stereo, uncompressed
                                                       : size_t(s.bitrate_kbps) * 1000 / 8 * 5;
  return std::max(kMinIcecastQueue, five_s);
}

StreamWorker::StreamWorker(const StreamConfig& cfg, const std::string& station_name)
    : cfg_(cfg),
      key_(stream_key(cfg, station_name)),
      started_ns_(platform::monotonic_ns()),
      queue_(kQueueFrames),
      gain_db_(cfg.gain_db) {
  encoder_ = make_encoder(cfg.codec);
  if (!encoder_) {
    error_ = std::string("kodek ") + codec_label(cfg.codec) + " niedostępny: brak biblioteki " + library_name(cfg.codec) +
             " obok programu";
    return;
  }
  std::string err;
  if (!encoder_->open(EncoderSettings{cfg.codec, cfg.bitrate_kbps, cfg.sample_rate, 2}, &err)) {
    error_ = err;
    encoder_.reset();
    return;
  }
  try {
    conditioner_ = std::make_unique<StreamConditioner>(cfg.sample_rate, db_to_gain(cfg.gain_db));
  } catch (const std::invalid_argument&) {
    error_ = "nieobsługiwana częstotliwość próbkowania " + std::to_string(cfg.sample_rate) + " Hz";
    encoder_.reset();
    return;
  }
  IcecastTarget t;
  t.host = cfg.host;
  t.port = static_cast<uint16_t>(cfg.port);
  t.mount = cfg.mount;
  t.user = cfg.user.empty() ? "source" : cfg.user;
  t.password = cfg.password;
  t.station_name = station_name;
  t.content_type = codec_content_type(cfg.codec);
  client_ = std::make_unique<IcecastClient>(std::move(t), icecast_queue_limit(cfg));
  client_->on_reconnect([this] { restart_.store(true); });  // client thread: only the flag
  active_ = true;
  thread_ = std::thread([this] { run(); });
}

StreamWorker::~StreamWorker() { stop(); }

void StreamWorker::set_gain_db(double db) { gain_db_.store(db); }

void StreamWorker::stop() {
  running_ = false;
  queue_.wake();
  if (thread_.joinable()) thread_.join();
  if (client_) client_->stop();
}

void StreamWorker::set_error(std::string e) {
  std::lock_guard lock(error_m_);
  error_ = std::move(e);
}

bool StreamWorker::reopen() {
  std::string err;
  // open() clears the encoder's own error first: its return value is what counts
  if (encoder_->open(EncoderSettings{cfg_.codec, cfg_.bitrate_kbps, cfg_.sample_rate, 2}, &err)) {
    set_error("");
    return true;
  }
  set_error(err.empty() ? std::string("nie udało się uruchomić kodera") : err);
  return false;
}

void StreamWorker::run() {
  std::vector<float> in(kChunkFrames * 2);
  const size_t out_frames = conditioner_->max_out_frames(kChunkFrames);
  std::vector<float> out(out_frames * 2);
  std::vector<uint8_t> bytes;
  bytes.reserve(64 * 1024);
  const bool ogg = cfg_.codec == Codec::Flac;  // the only codec whose stream needs its header first
  bool encoder_ok = true;                      // opened by the constructor
  bool start_next = false;                     // the next push begins a stream (Ogg after a reopen)
  int64_t failed_at = 0;
  double gain_applied = cfg_.gain_db;
  std::array<float, 2> peak{0.0f, 0.0f};
  size_t peak_frames = 0;
  const size_t peak_window = cfg_.sample_rate / 10;  // ~100 ms

  while (running_) {
    const int64_t now = platform::monotonic_ns();
    if (restart_.exchange(false) || (!encoder_ok && now - failed_at >= kReopenAfterErrorNs)) {
      encoder_ok = reopen();
      if (!encoder_ok) {
        failed_at = now;
      } else {
        start_next = true;
        if (ogg) {  // the header pages libFLAC wrote while opening
          bytes.clear();
          encoder_->encode(in.data(), 0, bytes);
          if (!bytes.empty()) {
            client_->push(bytes, true);
            start_next = false;
          }
        }
      }
    }
    if (const double g = gain_db_.load(); g != gain_applied) {
      conditioner_->set_gain(db_to_gain(g));
      gain_applied = g;
    }

    const size_t n = queue_.pop(in.data(), kChunkFrames, kPopWaitMs);
    if (n == 0) continue;
    const size_t m = conditioner_->process(in.data(), n, out.data(), out_frames);
    conditioner_dropped_.store(conditioner_->dropped_frames(), std::memory_order_relaxed);
    for (size_t f = 0; f < m; ++f) {
      peak[0] = std::max(peak[0], std::abs(out[f * 2]));
      peak[1] = std::max(peak[1], std::abs(out[f * 2 + 1]));
    }
    peak_frames += m;
    if (peak_frames >= peak_window) {
      for (int s = 0; s < 2; ++s) peak_[s].store(peak[s], std::memory_order_relaxed);
      peak = {0.0f, 0.0f};
      peak_frames = 0;
    }

    if (!encoder_ok) continue;  // the audio of a failed encoder is dropped until it reopens
    bytes.clear();
    encoder_->encode(out.data(), m, bytes);
    if (encoder_->failed()) {
      set_error(encoder_->error());
      log_warning("Strumień " + cfg_.mount + ": " + encoder_->error());
      encoder_ok = false;
      failed_at = now;
      continue;
    }
    if (!bytes.empty()) {
      client_->push(bytes, !ogg || start_next);
      start_next = false;
    }
  }
}

StreamStatus StreamWorker::status() const {
  StreamStatus s;
  s.id = cfg_.id;
  s.enabled = true;
  s.codec = cfg_.codec;
  s.bitrate_kbps = cfg_.bitrate_kbps;
  s.sample_rate = cfg_.sample_rate;
  s.host = cfg_.host;
  s.port = cfg_.port;
  s.mount = cfg_.mount;
  s.codec_available = codec_available(cfg_.codec);
  s.codec_library = codec_library_path(cfg_.codec);
  if (client_) s.icecast = client_->status();
  {
    std::lock_guard lock(error_m_);
    s.encoder_error = error_;
  }
  s.queue_dropped_frames = queue_.dropped_frames();
  s.conditioner_dropped_frames = conditioner_dropped_.load(std::memory_order_relaxed);
  for (int c = 0; c < 2; ++c) s.peak_db[c] = level_db(peak_[c].load(std::memory_order_relaxed));
  s.started_ns = started_ns_;
  return s;
}

}  // namespace bam::encoder::detail
