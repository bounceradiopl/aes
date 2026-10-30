// SPDX-License-Identifier: GPL-3.0-or-later
#include "asio_driver.hpp"

#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "bam/constants.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/time.hpp"

namespace bam {

std::atomic<long> g_object_count{0};

namespace {
constexpr long kMinBuffer = 64;
constexpr long kMaxBuffer = 512;
constexpr long kPreferredBuffer = 128;
constexpr int64_t kServiceStaleNs = 100'000'000;  // heartbeat older than 100 ms: service gone
constexpr const char* kDriverName = "BounceAES Master";

void copy_string(char* dst, size_t size, const std::string& src) {
  const size_t n = std::min(size - 1, src.size());
  std::memcpy(dst, src.data(), n);
  dst[n] = 0;
}

void split64(uint64_t v, unsigned long& hi, unsigned long& lo) {
  hi = static_cast<unsigned long>(v >> 32);
  lo = static_cast<unsigned long>(v & 0xFFFFFFFF);
}

uint16_t panel_port() {
  std::ifstream f(platform::program_data_dir() + "\\config.json", std::ios::binary);
  if (!f) return kDefaultHttpPort;
  try {
    std::stringstream ss;
    ss << f.rdbuf();
    return nlohmann::json::parse(ss.str()).value("http_port", kDefaultHttpPort);
  } catch (...) {
    return kDefaultHttpPort;
  }
}
}  // namespace

BounceAsio::BounceAsio() { ++g_object_count; }

BounceAsio::~BounceAsio() {
  disposeBuffers();
  --g_object_count;
}

HRESULT STDMETHODCALLTYPE BounceAsio::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  // ASIO hosts ask for the driver's CLSID as the interface id.
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kBounceAsioClsid)) {
    *ppv = static_cast<IASIO*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE BounceAsio::AddRef() { return ++refs_; }

ULONG STDMETHODCALLTYPE BounceAsio::Release() {
  const ULONG r = --refs_;
  if (r == 0) delete this;
  return r;
}

ASIOBool BounceAsio::init(void*) {
  if (view_) return ASIOTrue;
  std::string err;
  shm_ = platform::SharedMemory::open(kShmName, ShmLayout::total_bytes(), &err);
  if (!shm_) {
    error_ = "BounceAES Master: usługa nie działa (uruchom usługę BounceAESMaster)";
    return ASIOFalse;
  }
  auto view = std::make_unique<ShmView>(shm_->data());
  if (!view->compatible()) {
    error_ = "BounceAES Master: niezgodna wersja usługi i sterownika ASIO";
    shm_.reset();
    return ASIOFalse;
  }
  event_ = platform::NamedEvent::open(kPeriodEventName, &err);
  if (!event_) {
    error_ = "BounceAES Master: brak zdarzenia zegara (" + err + ")";
    shm_.reset();
    return ASIOFalse;
  }
  view_ = std::move(view);
  bridge_ = std::make_unique<AsioBridge>(*view_);
  generation_ = header().service_generation.load();
  in_bufs_.assign(kChannelCount, {});
  out_bufs_.assign(kChannelCount, {});
  in_ptrs_.assign(kChannelCount, nullptr);
  out_ptrs_.assign(kChannelCount, nullptr);
  return ASIOTrue;
}

void BounceAsio::getDriverName(char* name) { copy_string(name, 32, kDriverName); }

long BounceAsio::getDriverVersion() { return 1; }

void BounceAsio::getErrorMessage(char* text) { copy_string(text, 124, error_); }

ASIOError BounceAsio::getChannels(long* inputs, long* outputs) {
  if (!view_) return ASE_NotPresent;
  if (inputs) *inputs = kChannelCount;
  if (outputs) *outputs = kChannelCount;
  return ASE_OK;
}

ASIOError BounceAsio::getLatencies(long* input_latency, long* output_latency) {
  if (!view_) return ASE_NotPresent;
  const long b = frames_ ? frames_ : kPreferredBuffer;
  // Input latency: one buffer plus the smallest D_src among the routed input pairs.
  uint32_t min_rx = 0xFFFFFFFF;
  for (int p = 0; p < kDeviceCount; ++p) {
    const auto route = view_->asio_route(p);
    if (route.kind != ShmSource::None) min_rx = std::min(min_rx, route.delay);
  }
  if (min_rx == 0xFFFFFFFF) min_rx = 240;
  if (input_latency) *input_latency = b + static_cast<long>(min_rx);
  if (output_latency) *output_latency = static_cast<long>(kOutputLeadBuffers) * b + static_cast<long>(header().output_latency_frames.load());
  return ASE_OK;
}

ASIOError BounceAsio::getBufferSize(long* min_size, long* max_size, long* preferred, long* granularity) {
  if (!view_) return ASE_NotPresent;
  if (min_size) *min_size = kMinBuffer;
  if (max_size) *max_size = kMaxBuffer;
  if (preferred) *preferred = kPreferredBuffer;
  if (granularity) *granularity = -1;  // powers of two
  return ASE_OK;
}

ASIOError BounceAsio::canSampleRate(ASIOSampleRate rate) {
  if (!view_) return ASE_NotPresent;
  return rate == kSampleRate ? ASE_OK : ASE_NoClock;
}

ASIOError BounceAsio::getSampleRate(ASIOSampleRate* rate) {
  if (!view_) return ASE_NotPresent;
  if (rate) *rate = kSampleRate;
  return ASE_OK;
}

ASIOError BounceAsio::setSampleRate(ASIOSampleRate rate) {
  if (!view_) return ASE_NotPresent;
  return (rate == kSampleRate || rate == 0) ? ASE_OK : ASE_NoClock;
}

ASIOError BounceAsio::getClockSources(ASIOClockSource* clocks, long* count) {
  if (!clocks || !count) return ASE_InvalidParameter;
  clocks[0].index = 0;
  clocks[0].associatedChannel = -1;
  clocks[0].associatedGroup = -1;
  clocks[0].isCurrentSource = ASIOTrue;
  copy_string(clocks[0].name, 32, "PTP (AES67)");
  *count = 1;
  return ASE_OK;
}

ASIOError BounceAsio::setClockSource(long reference) { return reference == 0 ? ASE_OK : ASE_InvalidParameter; }

ASIOError BounceAsio::getSamplePosition(ASIOSamples* position, ASIOTimeStamp* stamp) {
  if (!running_) return ASE_SPNotAdvancing;
  if (position) split64(position_.load(), position->hi, position->lo);
  if (stamp) split64(static_cast<uint64_t>(position_time_ns_.load()), stamp->hi, stamp->lo);
  return ASE_OK;
}

ASIOError BounceAsio::getChannelInfo(ASIOChannelInfo* info) {
  if (!view_) return ASE_NotPresent;
  if (!info || info->channel < 0 || info->channel >= kChannelCount) return ASE_InvalidParameter;
  const int ch = static_cast<int>(info->channel);
  info->channelGroup = 0;
  info->type = ASIOSTInt32LSB;
  info->isActive = (info->isInput ? !in_bufs_[ch].data.empty() : !out_bufs_[ch].data.empty()) ? ASIOTrue : ASIOFalse;
  copy_string(info->name, 32, channel_label(ch, view_->device_name(ch / 2)));
  return ASE_OK;
}

ASIOError BounceAsio::createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) {
  if (!view_) return ASE_NotPresent;
  if (!infos || !callbacks || count <= 0 || size < 16 || size > 4096) return ASE_InvalidParameter;
  disposeBuffers();

  // Claim the device: one ASIO application at a time.
  const uint32_t me = platform::current_pid();
  uint32_t owner = header().client_pid.load();
  if (owner != 0 && owner != me && platform::process_alive(owner)) {
    error_ = "BounceAES Master: urządzenie zajęte przez inną aplikację (PID " + std::to_string(owner) + ")";
    return ASE_NotPresent;
  }
  if (!header().client_pid.compare_exchange_strong(owner, me) && owner != me) {
    error_ = "BounceAES Master: urządzenie zajęte";
    return ASE_NotPresent;
  }
  claimed_ = true;

  for (long i = 0; i < count; ++i) {
    auto& bi = infos[i];
    if (bi.channelNum < 0 || bi.channelNum >= kChannelCount) {
      disposeBuffers();
      return ASE_InvalidParameter;
    }
    auto& slot = bi.isInput ? in_bufs_[bi.channelNum] : out_bufs_[bi.channelNum];
    slot.data.assign(static_cast<size_t>(size) * 2, 0);
    bi.buffers[0] = slot.data.data();
    bi.buffers[1] = slot.data.data() + size;
  }
  frames_ = size;
  callbacks_ = callbacks;
  time_info_ = callbacks_->asioMessage && callbacks_->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
               callbacks_->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
  header().buffer_frames.store(static_cast<uint32_t>(size));
  half_ = 0;
  return ASE_OK;
}

ASIOError BounceAsio::disposeBuffers() {
  stop();
  for (auto& b : in_bufs_) b.data.clear();
  for (auto& b : out_bufs_) b.data.clear();
  callbacks_ = nullptr;
  frames_ = 0;
  release_claim();
  return ASE_OK;
}

void BounceAsio::release_claim() {
  if (!claimed_ || !view_) return;
  uint32_t me = platform::current_pid();
  header().client_running.store(0);
  if (header().client_pid.compare_exchange_strong(me, 0)) header().buffer_frames.store(0);
  claimed_ = false;
}

ASIOError BounceAsio::start() {
  if (!view_ || !callbacks_ || !frames_) return ASE_InvalidMode;
  if (running_) return ASE_OK;
  header().buffer_frames.store(static_cast<uint32_t>(frames_));
  header().client_running.store(1);
  running_ = true;
  thread_ = std::thread([this] { audio_thread(); });
  return ASE_OK;
}

ASIOError BounceAsio::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (view_ && claimed_) header().client_running.store(0);
  return ASE_OK;
}

void BounceAsio::process(uint64_t period_start, bool from_service) {
  for (int ch = 0; ch < kChannelCount; ++ch) {
    in_ptrs_[ch] = in_bufs_[ch].data.empty() ? nullptr : in_bufs_[ch].data.data() + half_ * frames_;
    out_ptrs_[ch] = out_bufs_[ch].data.empty() ? nullptr : out_bufs_[ch].data.data() + half_ * frames_;
  }
  if (from_service) {
    bridge_->read_inputs(period_start, static_cast<uint32_t>(frames_), in_ptrs_.data());
  } else {
    for (auto* p : in_ptrs_)
      if (p) std::memset(p, 0, frames_ * sizeof(int32_t));  // service gone: deliver silence
  }

  const int64_t stamp = static_cast<int64_t>(timeGetTime()) * 1'000'000;
  position_ = period_start;
  position_time_ns_ = stamp;
  if (time_info_ && callbacks_->bufferSwitchTimeInfo) {
    ASIOTime t{};
    t.timeInfo.speed = 1.0;
    split64(static_cast<uint64_t>(stamp), t.timeInfo.systemTime.hi, t.timeInfo.systemTime.lo);
    split64(period_start, t.timeInfo.samplePosition.hi, t.timeInfo.samplePosition.lo);
    t.timeInfo.sampleRate = kSampleRate;
    t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
    callbacks_->bufferSwitchTimeInfo(&t, half_, ASIOTrue);
  } else {
    callbacks_->bufferSwitch(half_, ASIOTrue);
  }

  if (from_service) {
    bridge_->write_outputs(period_start, static_cast<uint32_t>(frames_), out_ptrs_.data());
    header().client_heartbeat.fetch_add(1, std::memory_order_relaxed);
  }
  half_ ^= 1;
}

void BounceAsio::audio_thread() {
  platform::set_realtime_priority("Pro Audio");
  const int64_t period_ns = frames_ * 1'000'000'000LL / kSampleRate;
  const int timeout_ms = static_cast<int>(std::max<int64_t>(5, 2 * period_ns / 1'000'000 + 1));
  uint64_t last_counter = header().period_counter.load();
  uint64_t last_hb = header().heartbeat.load();
  int64_t hb_seen = platform::monotonic_ns();
  uint64_t self_position = header().period_start_sample.load();
  int64_t next_self = 0;
  bool reset_requested = false;

  while (running_) {
    event_->wait(timeout_ms);
    const int64_t now = platform::monotonic_ns();
    const uint64_t hb = header().heartbeat.load(std::memory_order_acquire);
    if (hb != last_hb) {
      last_hb = hb;
      hb_seen = now;
    }

    const uint64_t gen = header().service_generation.load();
    if (gen != generation_ && !reset_requested && hb != 0) {
      // The service restarted and re-initialised the shared timeline: ask the host to re-open us.
      reset_requested = true;
      generation_ = gen;
      if (callbacks_->asioMessage && callbacks_->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1)
        callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

    if (now - hb_seen < kServiceStaleNs) {
      const uint64_t counter = header().period_counter.load(std::memory_order_acquire);
      if (counter == last_counter) continue;
      last_counter = counter;
      self_position = header().period_start_sample.load(std::memory_order_acquire);
      next_self = now + period_ns;
      process(self_position, true);
    } else {
      // Keep the host running on our own clock so it doesn't stall while the service is down.
      if (next_self == 0) next_self = now;
      if (now < next_self) {
        platform::sleep_until_ns(next_self);
        continue;
      }
      next_self += period_ns;
      self_position += static_cast<uint64_t>(frames_);
      process(self_position, false);
    }
  }
}

ASIOError BounceAsio::controlPanel() {
  const std::string url = "http://127.0.0.1:" + std::to_string(panel_port()) + "/";
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return ASE_OK;
}

ASIOError BounceAsio::future(long selector, void*) {
  switch (selector) {
    case kAsioCanTimeInfo:
    case kAsioCanReportOverload:
      return ASE_SUCCESS;
    default:
      return ASE_InvalidParameter;
  }
}

ASIOError BounceAsio::outputReady() { return ASE_NotPresent; }

}  // namespace bam
