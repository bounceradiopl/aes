// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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
  float peak_db(int side) const { return peak_[side].load(std::memory_order_relaxed); }

 protected:
  explicit AudioStream(const ShmView& view) : view_(view) {}
  void start();  // launches run() on the audio thread
  void stop();   // derived destructors call this before their members go away
  virtual void run() = 0;
  void fail(const std::string& reason);
  void set_ok();
  void set_peaks(const float* interleaved, size_t frames);
  bool stopping() const { return stop_.load(std::memory_order_relaxed); }

  ShmView view_;

 private:
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> ok_{false};
  std::atomic<bool> finished_{false};
  std::atomic<float> peak_[2] = {-120.0f, -120.0f};
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

// Plays a timeline source (any ring) on a Windows playback device.
class RenderStream : public AudioStream {
 public:
  RenderStream(const ShmView& view, std::string endpoint, ShmSource kind, int pair, uint32_t delay_frames);
  ~RenderStream() override;

 private:
  void run() override;
  std::string endpoint_;
  ShmSource kind_;
  int pair_;
  uint32_t delay_;
};

}  // namespace bam::bridge
