// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include "bam/asio_bridge.hpp"
#include "bam/platform/event.hpp"
#include "bam/platform/shm.hpp"

namespace bam {

// {6B1F3C2A-8E4D-4B7A-9C51-2A7E0D4B3F10}
inline constexpr CLSID kBounceAsioClsid = {0x6b1f3c2a, 0x8e4d, 0x4b7a, {0x9c, 0x51, 0x2a, 0x7e, 0x0d, 0x4b, 0x3f, 0x10}};
inline constexpr const wchar_t* kBounceAsioClsidString = L"{6B1F3C2A-8E4D-4B7A-9C51-2A7E0D4B3F10}";

extern std::atomic<long> g_object_count;

// IASIO implementation: a thin client of BounceAESService. All audio goes through the shared
// timeline (AsioBridge); the buffer switch is driven by the service's period event (PTP clock).
class BounceAsio final : public IASIO {
 public:
  BounceAsio();
  ~BounceAsio();

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IASIO
  ASIOBool init(void* sys_handle) override;
  void getDriverName(char* name) override;
  long getDriverVersion() override;
  void getErrorMessage(char* text) override;
  ASIOError start() override;
  ASIOError stop() override;
  ASIOError getChannels(long* inputs, long* outputs) override;
  ASIOError getLatencies(long* input_latency, long* output_latency) override;
  ASIOError getBufferSize(long* min_size, long* max_size, long* preferred, long* granularity) override;
  ASIOError canSampleRate(ASIOSampleRate rate) override;
  ASIOError getSampleRate(ASIOSampleRate* rate) override;
  ASIOError setSampleRate(ASIOSampleRate rate) override;
  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override;
  ASIOError setClockSource(long reference) override;
  ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* stamp) override;
  ASIOError getChannelInfo(ASIOChannelInfo* info) override;
  ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override;
  ASIOError disposeBuffers() override;
  ASIOError controlPanel() override;
  ASIOError future(long selector, void* opt) override;
  ASIOError outputReady() override;

 private:
  struct ChannelBuffers {
    std::vector<int32_t> data;  // two halves of `frames` samples
  };

  void audio_thread();
  void process(uint64_t period_start, bool from_service);
  void release_claim();
  ShmHeader& header() const { return view_->header(); }

  std::atomic<ULONG> refs_{1};
  std::string error_;
  std::optional<platform::SharedMemory> shm_;
  std::optional<platform::NamedEvent> event_;
  std::unique_ptr<ShmView> view_;
  std::unique_ptr<AsioBridge> bridge_;
  uint64_t generation_ = 0;

  ASIOCallbacks* callbacks_ = nullptr;
  bool time_info_ = false;
  long frames_ = 0;
  bool claimed_ = false;
  std::vector<ChannelBuffers> in_bufs_, out_bufs_;  // index = channel; empty when not created
  std::vector<int32_t*> in_ptrs_;
  std::vector<const int32_t*> out_ptrs_;
  int half_ = 0;

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uint64_t> position_{0};
  std::atomic<int64_t> position_time_ns_{0};
};

}  // namespace bam
