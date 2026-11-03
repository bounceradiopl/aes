// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// One channel pair of a playback device and the timeline pair it plays.
struct RenderPair {
  int channel_pair = 1;  // device channels 2p-1, 2p (1-based)
  ShmSource kind = ShmSource::None;
  int pair = 0;          // ring pair of `kind`
  uint32_t delay = 0;    // D_src: the rings are complete up to "now - delay"
};

// Plays timeline sources (any ring, mix buses included) on the channel pairs of one Windows
// playback device, from a single WASAPI stream so all pairs share the device clock and one
// resampler. Every pair is heard at "playback time - its own delay", as a stream of its own would be.
class RenderStream : public AudioStream {
 public:
  RenderStream(const ShmView& view, std::string endpoint, std::vector<RenderPair> pairs);
  ~RenderStream() override;

  // Channel count of the device's mix format; 0 until the device is open.
  int device_channels() const { return device_channels_.load(std::memory_order_relaxed); }

 private:
  void run() override;
  std::string endpoint_;
  std::vector<RenderPair> pairs_;
  std::atomic<int> device_channels_{0};
};

}  // namespace bam::bridge
