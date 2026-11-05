// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bam/constants.hpp"
#include "bam/render_layout.hpp"
#include "bam/shm_layout.hpp"

namespace bam::bridge {

// One WASAPI stream on its own MMCSS "Pro Audio" thread. The thread opens the device itself and
// ends on any error; the owner sees finished() and recreates the stream after a pause.
class AudioStream {
 public:
  virtual ~AudioStream();
  AudioStream(const AudioStream&) = delete;
  AudioStream& operator=(const AudioStream&) = delete;

  bool ok() const { return ok_.load(); }
  bool finished() const { return finished_.load(); }
  std::string reason() const;
  // Peak of one metered channel (capture: 0/1; render: device channel), -120 when not metered.
  float peak_db(int channel) const;

 protected:
  AudioStream(const ShmView& view, int meter_channels = 2);
  void start();  // launches run() on the audio thread
  void stop();   // derived destructors call this before their members go away
  virtual void run() = 0;
  void fail(const std::string& reason);
  void set_ok();
  void set_peaks(const float* interleaved, size_t frames, int channels);
  bool stopping() const { return stop_.load(std::memory_order_relaxed); }

  ShmView view_;

 private:
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> ok_{false};
  std::atomic<bool> finished_{false};
  std::vector<std::atomic<float>> peak_;
  mutable std::mutex reason_mutex_;
  std::string reason_ = "uruchamianie";
};

enum class CaptureKind { App, Loopback, Input };

// Captures an application (process loopback, Windows 11), a playback device (loopback) or a
// recording device into win_src pair `slot`, on the PTP media clock.
class CaptureStream : public AudioStream {
 public:
  CaptureStream(const ShmView& view, int slot, CaptureKind kind, std::string key);  // key: exe or endpoint id
  ~CaptureStream() override;

 private:
  void run() override;
  int slot_;
  CaptureKind kind_;
  std::string key_;
};

// Plays timeline sources (any ring, mix buses included) on the channel pairs of one Windows
// playback device, from a single WASAPI stream so all pairs share the device clock and one
// resampler. Every pair is heard at "playback time - its own delay", as a stream of its own would be.
class RenderStream : public AudioStream {
 public:
  // max_delay of `feeds` is the stream's timeline for its whole life (see plan_device).
  RenderStream(const ShmView& view, std::string endpoint, const std::vector<RenderFeed>& feeds);
  ~RenderStream() override;

  // Channel count of the device's mix format; 0 until the device is open.
  int device_channels() const { return device_channels_.load(std::memory_order_relaxed); }

  // Replaces the pairs the device plays without reopening it; the render thread takes them at
  // the start of its next pass and channels no longer fed play silence from then on. Bridge
  // thread only (one writer). Feeds above kDeviceCount pairs are dropped.
  void update_feeds(const std::vector<RenderFeed>& feeds);

 private:
  // Fixed-size feed table handed to the render thread without allocation or locks: a triple
  // buffer. Each of the three tables is owned by exactly one side at a time: the writer's back
  // table, the reader's front table and the hand-over table in `handover_`. Ownership moves only
  // by atomic exchange, so neither side ever waits for the other or reads a table being written.
  struct FeedTable {
    int count = 0;
    std::array<RenderFeed, kDeviceCount> feeds{};
  };
  static constexpr uint32_t kFresh = 4;  // hand-over table holds feeds the reader has not taken
  static void fill(FeedTable& t, const std::vector<RenderFeed>& feeds);

  void run() override;
  std::string endpoint_;
  uint32_t max_delay_ = 0;
  std::array<FeedTable, 3> tables_{};
  std::atomic<uint32_t> handover_{2};  // index of the hand-over table | kFresh
  uint32_t back_ = 1;                  // writer's table (bridge thread)
  uint32_t front_ = 0;                 // reader's table (render thread)
  std::atomic<int> device_channels_{0};
};

}  // namespace bam::bridge
