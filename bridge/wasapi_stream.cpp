// SPDX-License-Identifier: GPL-3.0-or-later
#include "wasapi_stream.hpp"

#include <windows.h>
// windows.h first
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// Windows SDK 10.0.20348+ (audioclientactivationparams.h); not shipped with MinGW-w64.
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
  AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
  AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum PROCESS_LOOPBACK_MODE {
  PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
  PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
  DWORD TargetProcessId;
  PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
  AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
  union {
    AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
  };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#include "bam/meters.hpp"
#include "bam/platform/time.hpp"
#include "bam/rate_control.hpp"
#include "com_ptr.hpp"
#include "endpoints.hpp"

namespace bam::bridge {

namespace {
constexpr REFERENCE_TIME kBufferHns = 200000;  // 20 ms shared-mode buffer, 10 ms periods
constexpr uint32_t kFifoTarget = 480;          // 10 ms cushion at 48 kHz
constexpr uint64_t kReanchorFrames = 4800;     // 100 ms off the media clock: start over
constexpr uint32_t kChunk = 48;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string hr_text(const char* what, HRESULT hr) {
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return "urządzenie odłączone";
  if (hr == AUDCLNT_E_DEVICE_IN_USE) return "urządzenie zajęte (tryb wyłączny innej aplikacji)";
  char b[96];
  std::snprintf(b, sizeof(b), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
  return b;
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined here so no ksmedia GUID library is needed
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct Format {
  int channels = 2;
  uint32_t rate = 48000;
  int bytes = 4;  // container size per sample
  bool is_float = true;
};

Format describe(const WAVEFORMATEX* wf) {
  Format f;
  f.channels = wf->nChannels;
  f.rate = wf->nSamplesPerSec;
  f.bytes = wf->wBitsPerSample / 8;
  f.is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    f.is_float = IsEqualGUID(ext->SubFormat, kSubtypeFloat);
  }
  return f;
}

float read_sample(const BYTE* p, const Format& f) {
  if (f.is_float) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
  }
  switch (f.bytes) {
    case 2: {
      int16_t v;
      std::memcpy(&v, p, 2);
      return v / 32768.0f;
    }
    case 3:
      return static_cast<float>(static_cast<int32_t>((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) / 2147483648.0);
    case 4: {
      int32_t v;
      std::memcpy(&v, p, 4);
      return static_cast<float>(v / 2147483648.0);
    }
  }
  return 0;
}

void write_sample(BYTE* p, float v, const Format& f) {
  v = std::clamp(v, -1.0f, 1.0f);
  if (f.is_float) {
    std::memcpy(p, &v, 4);
    return;
  }
  const int32_t s = from_float(v);
  switch (f.bytes) {
    case 2: {
      const int16_t x = static_cast<int16_t>(s >> 16);
      std::memcpy(p, &x, 2);
      break;
    }
    case 3:
      p[0] = static_cast<BYTE>(s >> 8);
      p[1] = static_cast<BYTE>(s >> 16);
      p[2] = static_cast<BYTE>(s >> 24);
      break;
    case 4:
      std::memcpy(p, &s, 4);
      break;
  }
}

// Device frames -> interleaved stereo float (mono duplicated, channels above 2 dropped).
void to_stereo(const BYTE* data, size_t frames, const Format& f, std::vector<float>& out) {
  const size_t stride = static_cast<size_t>(f.channels) * f.bytes;
  const size_t base = out.size();
  out.resize(base + frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const BYTE* fr = data + i * stride;
    const float l = read_sample(fr, f);
    const float r = f.channels > 1 ? read_sample(fr + f.bytes, f) : l;
    out[base + i * 2] = l;
    out[base + i * 2 + 1] = r;
  }
}

void from_stereo(const float* in, size_t frames, const Format& f, BYTE* data) {
  const size_t stride = static_cast<size_t>(f.channels) * f.bytes;
  std::memset(data, 0, frames * stride);
  for (size_t i = 0; i < frames; ++i) {
    BYTE* fr = data + i * stride;
    if (f.channels == 1) {
      write_sample(fr, 0.5f * (in[i * 2] + in[i * 2 + 1]), f);
    } else {
      write_sample(fr, in[i * 2], f);
      write_sample(fr + f.bytes, in[i * 2 + 1], f);
    }
  }
}

// Resamples all of `in` (interleaved stereo) and appends to `out`.
void resample_all(DriftResampler& rs, std::vector<float>& in, std::vector<float>& out, double mult) {
  size_t offset = 0;
  const size_t frames = in.size() / 2;
  std::vector<float> tmp(frames * 2 + 256);
  while (offset < frames) {
    size_t used = 0;
    const size_t n = rs.process(in.data() + offset * 2, frames - offset, tmp.data(), tmp.size() / 2, mult, &used);
    out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n * 2));
    offset += used;
    if (!used && !n) break;
  }
  in.clear();
}

// ---- process loopback activation ----
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
 public:
  ActivationHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~ActivationHandler() { CloseHandle(event_); }
  HANDLE event() const { return event_; }

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    // IAgileObject {94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90}: the callback may arrive on any thread
    static const GUID kAgile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IActivateAudioInterfaceCompletionHandler)) || IsEqualIID(iid, kAgile)) {
      *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
    SetEvent(event_);
    return S_OK;
  }

 private:
  LONG refs_ = 1;
  HANDLE event_;
};

HRESULT activate_process_loopback(DWORD pid, IAudioClient** client) {
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof(params);
  pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  auto* handler = new ActivationHandler();
  ComPtr<IActivateAudioInterfaceAsyncOperation> op;
  HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, op.put());
  if (SUCCEEDED(hr)) {
    if (WaitForSingleObject(handler->event(), 5000) != WAIT_OBJECT_0) {
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    } else {
      HRESULT result = E_FAIL;
      ComPtr<IUnknown> unk;
      hr = op->GetActivateResult(&result, unk.put());
      if (SUCCEEDED(hr)) hr = result;
      if (SUCCEEDED(hr)) hr = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(client));
    }
  }
  handler->Release();
  return hr;
}

HRESULT activate_endpoint(const std::string& id, IAudioClient** client) {
  ComPtr<IMMDeviceEnumerator> e;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(e.put()));
  if (FAILED(hr)) return hr;
  ComPtr<IMMDevice> dev;
  hr = e->GetDevice(widen(id).c_str(), dev.put());
  if (FAILED(hr)) return hr;
  return dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client));
}

struct ComScope {
  ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  ~ComScope() { CoUninitialize(); }
};

struct MmcssScope {
  HANDLE h = nullptr;
  MmcssScope() {
    DWORD task = 0;
    h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  }
  ~MmcssScope() {
    if (h) AvRevertMmThreadCharacteristics(h);
  }
};

struct EventHandle {
  HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ~EventHandle() { CloseHandle(h); }
};

uint64_t media_now(const ShmView& view, uint64_t* generation) {
  const MediaClock c = view.read_clock(generation);
  return c.sample_at(platform::monotonic_ns());
}
}  // namespace

// ---------------------------------------------------------------- AudioStream
AudioStream::~AudioStream() { stop(); }

void AudioStream::start() {
  thread_ = std::thread([this] {
    run();
    ok_ = false;
    finished_ = true;
  });
}

void AudioStream::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

std::string AudioStream::reason() const {
  std::lock_guard lock(reason_mutex_);
  return reason_;
}

void AudioStream::fail(const std::string& reason) {
  {
    std::lock_guard lock(reason_mutex_);
    reason_ = reason;
  }
  ok_ = false;
  peak_[0] = peak_[1] = -120.0f;
}

void AudioStream::set_ok() {
  {
    std::lock_guard lock(reason_mutex_);
    reason_.clear();
  }
  ok_ = true;
}

void AudioStream::set_peaks(const float* in, size_t frames) {
  float m[2] = {0, 0};
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < 2; ++c) m[c] = std::max(m[c], std::abs(in[i * 2 + c]));
  for (int c = 0; c < 2; ++c) {
    const float db = m[c] > 1e-6f ? 20.0f * std::log10(m[c]) : -120.0f;
    // simple decay so the meter is readable at the report rate
    const float prev = peak_[c].load(std::memory_order_relaxed);
    peak_[c].store(std::max(db, prev - 1.5f), std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------- capture
CaptureStream::CaptureStream(const ShmView& view, int slot, CaptureKind kind, std::string key)
    : AudioStream(view), slot_(slot), kind_(kind), key_(std::move(key)) {
  start();
}

CaptureStream::~CaptureStream() { stop(); }

void CaptureStream::run() {
  ComScope com;
  MmcssScope mmcss;
  EventHandle ev;
  ComPtr<IAudioClient> client;
  HANDLE process = nullptr;
  Format fmt;
  HRESULT hr;

  if (kind_ == CaptureKind::App) {
    if (windows_build() < 22000) return fail("wymaga Windows 11");
    const uint32_t pid = find_process(key_);
    if (!pid) return fail("aplikacja nie działa");
    process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    hr = activate_process_loopback(pid, client.put());
    if (FAILED(hr)) {
      if (process) CloseHandle(process);
      return fail(hr_text("przechwytywanie aplikacji niedostępne", hr));
    }
    // process loopback has no mix format: ask for 48 kHz float stereo
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = 48000 * 8;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, kBufferHns, 0,
                            &wf, nullptr);
    fmt = describe(&wf);
  } else {
    hr = activate_endpoint(key_, client.put());
    if (FAILED(hr)) return fail(hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? "urządzenie niedostępne" : hr_text("urządzenie niedostępne", hr));
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return fail(hr_text("format urządzenia", hr));
    fmt = describe(mix);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (kind_ == CaptureKind::Loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferHns, 0, mix, nullptr);
    CoTaskMemFree(mix);
  }
  if (FAILED(hr)) {
    if (process) CloseHandle(process);
    return fail(hr_text("inicjalizacja WASAPI", hr));
  }
  client->SetEventHandle(ev.h);
  ComPtr<IAudioCaptureClient> cap;
  hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cap.put()));
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    if (process) CloseHandle(process);
    return fail(hr_text("start WASAPI", hr));
  }
  set_ok();

  DriftResampler rs(48000.0 / fmt.rate);
  RateController rc(kFifoTarget);
  std::vector<float> raw, fifo, chunk_l, chunk_r;
  std::vector<int32_t> l, r;
  double mult = 1.0;
  bool anchored = false;
  uint64_t write_pos = 0, generation = 0;
  int64_t last_update = platform::monotonic_ns();
  ChannelRing ring_l = view_.win_src(slot_ * 2), ring_r = view_.win_src(slot_ * 2 + 1);

  while (!stopping()) {
    WaitForSingleObject(ev.h, 20);
    if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
      fail("aplikacja zakończyła działanie");
      break;
    }
    // ---- drain the device ----
    for (;;) {
      UINT32 next = 0;
      hr = cap->GetNextPacketSize(&next);
      if (FAILED(hr) || !next) break;
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) break;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        raw.resize(raw.size() + frames * 2, 0.0f);
      } else {
        to_stereo(data, frames, fmt, raw);
      }
      cap->ReleaseBuffer(frames);
    }
    if (FAILED(hr)) {
      fail(hr_text("przechwytywanie", hr));
      break;
    }
    if (!raw.empty()) resample_all(rs, raw, fifo, mult);

    // ---- place on the media clock ----
    uint64_t g = 0;
    const uint64_t now_s = media_now(view_, &g);
    const size_t fill = fifo.size() / 2;
    if (!anchored || g != generation || write_pos + kReanchorFrames < now_s || write_pos > now_s + kReanchorFrames) {
      if (fill < kFifoTarget && anchored == false) continue;  // collect the cushion first
      write_pos = now_s;
      generation = g;
      anchored = true;
      rc.reset();
      if (fill > kFifoTarget) fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>((fill - kFifoTarget) * 2));
    }
    if (now_s > write_pos) {
      const size_t due = static_cast<size_t>(now_s - write_pos);
      const size_t have = std::min(due, fifo.size() / 2);
      l.assign(due, 0);
      r.assign(due, 0);
      for (size_t i = 0; i < have; ++i) {
        l[i] = from_float(fifo[i * 2]);
        r[i] = from_float(fifo[i * 2 + 1]);
      }
      if (have) set_peaks(fifo.data(), have);
      fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(have * 2));
      ring_l.write(write_pos, l.data(), due);  // a shortfall becomes silence, the timeline stays continuous
      ring_r.write(write_pos, r.data(), due);
      write_pos = now_s;
    }
    const int64_t t = platform::monotonic_ns();
    mult = rc.update(static_cast<double>(fifo.size() / 2), (t - last_update) / 1e9);
    last_update = t;
  }
  client->Stop();
  if (process) CloseHandle(process);
}

// ---------------------------------------------------------------- render
RenderStream::RenderStream(const ShmView& view, std::string endpoint, ShmSource kind, int pair, uint32_t delay_frames)
    : AudioStream(view), endpoint_(std::move(endpoint)), kind_(kind), pair_(pair), delay_(delay_frames) {
  start();
}

RenderStream::~RenderStream() { stop(); }

void RenderStream::run() {
  ComScope com;
  MmcssScope mmcss;
  EventHandle ev;
  ComPtr<IAudioClient> client;
  HRESULT hr = activate_endpoint(endpoint_, client.put());
  if (FAILED(hr)) return fail("urządzenie niedostępne");
  WAVEFORMATEX* mix = nullptr;
  hr = client->GetMixFormat(&mix);
  if (FAILED(hr)) return fail(hr_text("format urządzenia", hr));
  const Format fmt = describe(mix);
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferHns, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) return fail(hr_text("inicjalizacja WASAPI", hr));
  client->SetEventHandle(ev.h);
  UINT32 buffer_frames = 0;
  client->GetBufferSize(&buffer_frames);
  ComPtr<IAudioRenderClient> render;
  hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.put()));
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) return fail(hr_text("start WASAPI", hr));
  set_ok();

  const ChannelRing src_l = view_.source_ring(kind_, pair_ * 2), src_r = view_.source_ring(kind_, pair_ * 2 + 1);
  DriftResampler rs(fmt.rate / 48000.0);
  const double target = fmt.rate / 100.0;  // 10 ms of device frames
  RateController rc(target);
  std::vector<float> staging, out;
  std::vector<int32_t> l(kChunk), r(kChunk);
  double mult = 1.0;
  bool anchored = false;
  uint64_t read_pos = 0, generation = 0;
  int64_t last_update = platform::monotonic_ns();

  while (!stopping()) {
    WaitForSingleObject(ev.h, 20);
    UINT32 padding = 0;
    hr = client->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      fail(hr_text("odtwarzanie", hr));
      break;
    }
    // ---- pull the source up to "now - D_src" ----
    uint64_t g = 0;
    const uint64_t now_s = media_now(view_, &g);
    const uint64_t ready = now_s > delay_ ? now_s - delay_ : 0;
    if (!anchored || g != generation || read_pos + kReanchorFrames < ready || read_pos > ready + kReanchorFrames) {
      read_pos = ready > kFifoTarget ? ready - kFifoTarget : 0;
      generation = g;
      anchored = true;
      out.clear();
      rc.reset();
    }
    while (read_pos + kChunk <= ready) {
      src_l.read(read_pos, l.data(), kChunk);
      src_r.read(read_pos, r.data(), kChunk);
      for (uint32_t i = 0; i < kChunk; ++i) {
        staging.push_back(to_float(l[i]));
        staging.push_back(to_float(r[i]));
      }
      read_pos += kChunk;
    }
    if (!staging.empty()) resample_all(rs, staging, out, mult);

    // ---- feed the device ----
    const size_t space = buffer_frames > padding ? buffer_frames - padding : 0;
    const size_t n = std::min(space, out.size() / 2);
    if (n) {
      BYTE* data = nullptr;
      hr = render->GetBuffer(static_cast<UINT32>(n), &data);
      if (FAILED(hr)) {
        fail(hr_text("odtwarzanie", hr));
        break;
      }
      from_stereo(out.data(), n, fmt, data);
      render->ReleaseBuffer(static_cast<UINT32>(n), 0);
      set_peaks(out.data(), n);
      out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n * 2));
    }
    const int64_t t = platform::monotonic_ns();
    mult = rc.update(static_cast<double>(padding + n + out.size() / 2), (t - last_update) / 1e9);
    last_update = t;
  }
  client->Stop();
}

}  // namespace bam::bridge
