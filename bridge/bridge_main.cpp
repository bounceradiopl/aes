// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAESAudioBridge: runs in the user's session (started by BounceAESTray) and connects Windows
// audio to the BounceAES timeline: captures applications, playback devices (loopback) and
// recording devices into win_src rings, and plays routed sources on Windows playback devices.
// The service in session 0 has no access to a user's audio, which is why this is a separate process.
#include <windows.h>
// windows.h first
#include <objbase.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bam/bridge_protocol.hpp"
#include "bam/platform/process.hpp"
#include "bam/platform/shm.hpp"
#include "bam/platform/time.hpp"
#include "bam/render_layout.hpp"
#include "bam/routing.hpp"
#include "bam/shm_layout.hpp"
#include "endpoints.hpp"
#include "wasapi_stream.hpp"

namespace bp = bam::platform;
using namespace bam;
using namespace bam::bridge;

namespace {
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kRetryNs = 2000 * kMs;
constexpr int64_t kForeignStaleNs = 3000 * kMs;
constexpr uint32_t kWinSourceDelay = 960;

struct Slot {
  std::string key;  // capture: the source id reported for the slot
  std::optional<DeviceLayout> layout;  // render: what the device plays (destination ids for the report)
  std::unique_ptr<AudioStream> stream;
  int64_t failed_at = 0;
};

class Bridge {
 public:
  explicit Bridge(ShmView view) : view_(view), service_generation_(view.header().service_generation.load()) {}

  // Returns false when the service restarted (shared memory re-initialised) and the bridge must reconnect.
  bool tick(int64_t now) {
    auto& h = view_.header();
    if (h.service_generation.load() != service_generation_ || !view_.compatible()) return false;
    if (h.bridge_pid.load() != bp::current_pid()) return false;  // someone took over
    h.bridge_heartbeat.fetch_add(1, std::memory_order_release);
    h.win_src_delay_frames.store(kWinSourceDelay);

    if (now - last_request_ >= 500 * kMs) {
      last_request_ = now;
      uint32_t version = 0;
      const std::string text = view_.read_request(&version);
      if (version != request_version_) {
        request_version_ = version;
        if (auto req = request_from_json(text)) request_ = *req;
      }
      reconcile(now);
    }
    if (now - last_report_ >= 1000 * kMs) {
      last_report_ = now;
      report();
    }
    return true;
  }

  void release() {
    slots_.clear();
    uint32_t self = bp::current_pid();
    view_.header().bridge_pid.compare_exchange_strong(self, 0);
  }

 private:
  void reconcile(int64_t now) {
    std::map<std::string, bool> wanted;
    for (const auto& s : request_.sources) {
      const std::string key = "src|" + std::to_string(s.slot) + "|" + s.id;
      wanted[key] = true;
      auto parsed = parse_source(s.id);
      if (!parsed) continue;
      Slot& slot = slots_[key];
      slot.key = s.id;
      if (should_start(slot, now)) {
        const CaptureKind kind = parsed->kind == SourceKind::WinApp    ? CaptureKind::App
                                 : parsed->kind == SourceKind::WinLoop ? CaptureKind::Loopback
                                                                       : CaptureKind::Input;
        // a slot changing owner: the previous stream must be gone before the ring gets a new writer
        for (auto it = slots_.begin(); it != slots_.end(); ++it)
          if (it->first != key && it->first.rfind("src|" + std::to_string(s.slot) + "|", 0) == 0) it->second.stream.reset();
        slot.stream = std::make_unique<CaptureStream>(view_, s.slot, kind, parsed->key);
      }
    }
    // one RenderStream per playback device, playing all of its routed channel pairs. A changed
    // pair goes to the running stream (update_feeds), so the device's other pairs play on; only
    // a new timeline (max_delay) reopens it. Devices whose layout did not change stay untouched.
    for (DeviceLayout& next : device_layouts(request_.dests)) {
      const std::string key = "dst|" + next.endpoint;
      wanted[key] = true;
      Slot& slot = slots_[key];
      switch (plan_device(slot.layout ? &*slot.layout : nullptr, &next)) {
        case DeviceAction::Restart:  // the old stream goes first, so no pair is played twice
          slot.stream.reset();
          slot.failed_at = 0;
          break;
        case DeviceAction::UpdateFeeds:  // a stream waiting for its retry starts with the new layout
          if (auto* render = dynamic_cast<RenderStream*>(slot.stream.get()); render && !render->finished()) render->update_feeds(next.feeds);
          break;
        case DeviceAction::Keep:
        case DeviceAction::Start:  // no stream yet: should_start below
        case DeviceAction::Stop:   // not for a device in the request
          break;
      }
      slot.layout = std::move(next);
      if (should_start(slot, now)) slot.stream = std::make_unique<RenderStream>(view_, slot.layout->endpoint, slot.layout->feeds);
    }
    // a device no longer in the request (DeviceAction::Stop) loses its slot and stream here
    for (auto it = slots_.begin(); it != slots_.end();) {
      if (!wanted.count(it->first)) {
        it = slots_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // A stream that ended (device gone, application closed) is retried after kRetryNs.
  static bool should_start(Slot& slot, int64_t now) {
    if (!slot.stream) return true;
    if (!slot.stream->finished()) return false;
    if (!slot.failed_at) {
      slot.failed_at = now;
      return false;
    }
    if (now - slot.failed_at < kRetryNs) return false;
    slot.failed_at = 0;
    return true;
  }

  void report() {
    BridgeReport r;
    r.generation = request_.generation;
    r.os_build = std::to_string(windows_build());
    r.endpoints = list_endpoints();
    r.apps = list_apps();
    for (const auto& [key, slot] : slots_) {
      if (!slot.layout) {
        r.slots.push_back(slot_state(slot.key, slot, 0));
        continue;
      }
      for (size_t i = 0; i < slot.layout->feeds.size(); ++i)
        r.slots.push_back(slot_state(slot.layout->dest_ids[i], slot, slot.layout->feeds[i].channel_pair));
    }
    view_.write_report(to_json(r));
  }

  // channel_pair 0: a capture slot (meters 0/1); otherwise the render device's pair
  static BridgeSlotState slot_state(const std::string& id, const Slot& slot, int channel_pair) {
    BridgeSlotState st;
    st.id = id;
    if (!slot.stream) {
      st.reason = "uruchamianie";
      return st;
    }
    st.ok = slot.stream->ok();
    st.reason = st.ok ? std::string() : slot.stream->reason();
    const int first = channel_pair > 0 ? 2 * (channel_pair - 1) : 0;
    if (const auto* render = dynamic_cast<const RenderStream*>(slot.stream.get()); render && st.ok) {
      // a mono device plays pair 1 only (downmixed)
      const int channels = std::max(render->device_channels(), 2);
      if (render->device_channels() > 0 && first >= channels) {
        st.ok = false;
        st.reason = "urządzenie nie ma kanałów " + std::to_string(first + 1) + "-" + std::to_string(first + 2);
      }
    }
    if (st.ok) {
      st.peak_db[0] = slot.stream->peak_db(first);
      st.peak_db[1] = slot.stream->peak_db(first + 1);
    }
    return st;
  }

  ShmView view_;
  uint64_t service_generation_;
  uint32_t request_version_ = 0;
  BridgeRequest request_;
  std::map<std::string, Slot> slots_;
  int64_t last_request_ = 0, last_report_ = 0;
};

// Claims the bridge role on this computer: free, or held by a process that is gone or silent.
bool claim(ShmView& view) {
  auto& h = view.header();
  const uint32_t self = bp::current_pid();
  uint32_t holder = h.bridge_pid.load();
  if (holder == self) return true;
  if (holder != 0 && bp::process_alive(holder)) {
    const uint64_t hb = h.bridge_heartbeat.load();
    const int64_t until = bp::monotonic_ns() + kForeignStaleNs;
    while (bp::monotonic_ns() < until) {
      if (h.bridge_heartbeat.load() != hb) return false;  // a live bridge (another user session)
      bp::sleep_ns(100 * kMs);
    }
  }
  return h.bridge_pid.compare_exchange_strong(holder, self);
}

HANDLE g_quit = nullptr;

BOOL WINAPI on_console(DWORD) {
  SetEvent(g_quit);
  return TRUE;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // one bridge per user session; across sessions the shared memory claim decides
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BounceAESAudioBridge");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  g_quit = CreateEventW(nullptr, TRUE, FALSE, L"Local\\BounceAESAudioBridge.quit");
  SetConsoleCtrlHandler(on_console, TRUE);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  while (WaitForSingleObject(g_quit, 0) != WAIT_OBJECT_0) {
    std::string err;
    auto shm = bp::SharedMemory::open(kShmName, ShmLayout::total_bytes(), &err);
    if (!shm) {  // service not running yet
      WaitForSingleObject(g_quit, 2000);
      continue;
    }
    ShmView view(shm->data());
    if (!view.compatible() || !claim(view)) {
      WaitForSingleObject(g_quit, 5000);
      continue;
    }
    Bridge bridge(view);
    while (WaitForSingleObject(g_quit, 100) != WAIT_OBJECT_0) {
      if (!bridge.tick(bp::monotonic_ns())) break;
    }
    bridge.release();
  }
  CoUninitialize();
  if (single) CloseHandle(single);
  return 0;
}
