# BounceAES Master Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Open-source AES67 ASIO driver for Windows 10/11 x64 (32 stereo in / 32 stereo out, PTPv2 slave, SAP, Axia-style web panel, tray app, MSI) — a clean-room replacement for Axia IP-Audio Driver.

**Architecture:** A cross-platform `core` library holds all protocol and timing logic (SDP, RTP, PTP, SAP, timeline rings, rx/tx streams, ASIO buffer exchange, config, auth). A thin `platform` layer (Windows + POSIX) provides sockets, shared memory, events and clocks. `engine` wires threads + HTTP panel; `service` is the entry point (Windows service or `--console`). `BounceAESAsio.dll` is a thin IASIO COM object over the shared-memory timeline. Everything except the ASIO DLL, Windows service wrapper and tray app runs and is tested on macOS/Linux.

**Tech Stack:** C++20, CMake ≥ 3.24, Ninja; MSVC 2022 (release builds), MinGW-w64 (cross-compile check), Apple clang (local tests). Vendored: doctest 2.4.11 (MIT), nlohmann/json 3.11.3 (MIT), cpp-httplib 0.18.x (MIT), Steinberg ASIO SDK 2.3.4 headers (GPLv3 option). WiX v4 for MSI (CI only).

**Spec:** `docs/superpowers/specs/2026-10-30-bounceaes-master-design.md`

## Global Constraints

- Product name exactly `BounceAES Master`; binaries `BounceAESService.exe`, `BounceAESAsio.dll`, `BounceAESTray.exe`; Windows service name `BounceAESMaster`.
- License GPLv3 for all project code; every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Sample rate fixed 48000 Hz; 32 devices × 2 channels; ring 65536 frames, block 16 frames, `int32` left-aligned samples (`ASIOSTInt32LSB`).
- RTP port 5004, TTL 15; SAP 239.255.255.255:9875 every 30 s; PTP 224.0.1.129:319/320, E2E, domain default 0; HTTP default 0.0.0.0:8067.
- Default output addresses `239.69.100.1` … `239.69.100.32`; output ptime ∈ {1000, 1333, 4000} µs (frames 48, 64, 192), default 1000.
- Rx latency default 5000 µs, range 1000–1000000 µs, must be ≥ input ptime + 1000 µs (auto-raised with notice).
- PC nominal level default −20 dBFS (= 0 dB, bit-transparent), range −30…0; gain to app = level+20 dB, to network = −(level+20) dB.
- Config file: Windows `%ProgramData%\BounceAES Master\config.json`; POSIX path from `--config` (default `./bounceaes-config.json`).
- Shared memory name `BounceAESMaster.v1` (Windows: `Global\` prefix; POSIX: `/BounceAESMaster.v1`); period event `BounceAESMaster.v1.period`.
- Code must compile with MSVC, MinGW-w64 GCC and Apple clang: no `__int128`, no compiler-specific extensions outside `platform/`.
- UI text (panel, tray, logs shown to users) in Polish.

---

## File Structure

```
CMakeLists.txt                      top-level, options BAM_BUILD_TESTS/BAM_BUILD_WINDOWS_APPS
cmake/mingw-w64-x86_64.cmake        cross toolchain file
cmake/embed_files.cmake             generates panel_assets.cpp
third_party/doctest/doctest.h
third_party/nlohmann/json.hpp
third_party/httplib/httplib.h
third_party/asiosdk/{asio.h,asiosys.h,iasiodrv.h,LICENSE.txt}
core/include/bam/constants.hpp      product-wide constants
core/include/bam/format.hpp         Encoding enum, bytes_per_sample
core/include/bam/sdp.hpp            SdpStream, parse_sdp, generate_sdp
core/include/bam/rtp.hpp            RtpHeader, parse_rtp, PCM codecs, unwrap
core/include/bam/clock.hpp          MediaClock, ClockSource
core/include/bam/timeline.hpp       ChannelRing
core/include/bam/shm_layout.hpp     ShmHeader, ShmLayout, ShmView
core/include/bam/gain.hpp           db_to_gain, apply_gain
core/include/bam/rx_stream.hpp      RxStream, RxStats
core/include/bam/tx_stream.hpp      TxStream, TxStats
core/include/bam/ptp_messages.hpp   PTP wire format
core/include/bam/ptp_slave.hpp      PtpSlave, PtpStatus
core/include/bam/sap.hpp            SAP wire format, SapDirectory
core/include/bam/config.hpp         Config, DeviceConfig, JSON, validate/normalize
core/include/bam/auth.hpp           sha256, pbkdf2, password hash, SessionStore
core/include/bam/asio_bridge.hpp    AsioBridge, channel_label
core/include/bam/meters.hpp         peak_dbfs
core/include/bam/log.hpp            Logger
core/src/*.cpp                      one .cpp per header
platform/include/bam/platform/{time,udp,shm,event,process}.hpp
platform/src/{posix,win}/*.cpp
engine/include/bam/engine.hpp       Engine
engine/include/bam/http_server.hpp  HttpServer
engine/src/{engine.cpp,ptp_worker.cpp,rx_worker.cpp,tx_worker.cpp,period_worker.cpp,sap_worker.cpp,http_server.cpp,json_api.cpp}
service/main.cpp                    --console / Windows service
service/win_service.cpp             SCM + Event Log (Windows only)
asio/{asio_driver.hpp,asio_driver.cpp,dll_main.cpp,BounceAESAsio.def}
tray/tray_main.cpp
panel/{index.html,app.js,style.css}
tools/bounceaes_test.cpp, tools/ptp_sim.cpp
tests/test_*.cpp, tests/data/orban.sdp
installer/BounceAES.wxs, installer/install.ps1, installer/uninstall.ps1
.github/workflows/build.yml
README.md, docs/INSTALL.md, docs/TESTING.md, LICENSE
```

Verification commands used throughout:

- Host build & tests: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`
- Windows cross check: `cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DBAM_BUILD_TESTS=OFF && cmake --build build-win`

---

### Task 1: Scaffold, vendored dependencies, constants

**Files:**
- Create: `CMakeLists.txt`, `cmake/mingw-w64-x86_64.cmake`, `.gitignore`, `LICENSE` (GPLv3 text), `third_party/*`, `core/include/bam/constants.hpp`, `core/include/bam/format.hpp`, `core/src/format.cpp`, `tests/test_main.cpp`, `tests/test_format.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
inline constexpr uint32_t kSampleRate = 48000;
inline constexpr int kDeviceCount = 32, kChannelsPerDevice = 2, kChannelCount = 64;
inline constexpr uint32_t kRingFrames = 65536, kRingMask = kRingFrames - 1;
inline constexpr uint32_t kBlockFrames = 16, kBlocksPerRing = kRingFrames / kBlockFrames;
inline constexpr uint16_t kRtpPort = 5004, kSapPort = 9875, kPtpEventPort = 319, kPtpGeneralPort = 320, kDefaultHttpPort = 8067;
inline constexpr const char* kSapGroup = "239.255.255.255";
inline constexpr const char* kPtpGroup = "224.0.1.129";
inline constexpr const char* kProductName = "BounceAES Master";
inline constexpr const char* kVersion = "1.0.0";
enum class Encoding { L16, L24 };
size_t bytes_per_sample(Encoding e);           // 2 or 3
const char* encoding_name(Encoding e);          // "L16" / "L24"
}
```

- [ ] Step 1: Vendor deps: download doctest.h, json.hpp, httplib.h at pinned tags; copy `asio.h`, `asiosys.h`, `iasiodrv.h`, `LICENSE.txt` from the ASIO SDK 2.3.4 zip into `third_party/asiosdk/`.
- [ ] Step 2: Write `tests/test_format.cpp`:
```cpp
#include <doctest/doctest.h>
#include "bam/format.hpp"
TEST_CASE("bytes per sample") {
  CHECK(bam::bytes_per_sample(bam::Encoding::L16) == 2);
  CHECK(bam::bytes_per_sample(bam::Encoding::L24) == 3);
  CHECK(std::string(bam::encoding_name(bam::Encoding::L24)) == "L24");
}
```
- [ ] Step 3: Build → fails (no format.cpp). Implement. Build + ctest → PASS.
- [ ] Step 4: Commit `chore: scaffold project, vendor dependencies`.

### Task 2: SDP parse/generate

**Files:** Create `core/include/bam/sdp.hpp`, `core/src/sdp.cpp`, `tests/test_sdp.cpp`, `tests/data/orban.sdp` (exact SDP from spec §3).

**Interfaces — Produces:**
```cpp
namespace bam {
struct SdpStream {
  std::string session_name, origin_address, multicast_address;
  int ttl = 0; uint16_t port = 0; int payload_type = 0;
  Encoding encoding = Encoding::L24; uint32_t sample_rate = 0; int channels = 0;
  double ptime_ms = 1.0; uint32_t frames_per_packet = 48;   // derived: round(ptime_ms*rate/1000)
  std::optional<uint32_t> framecount;
  std::string source_filter;      // source IP from a=source-filter incl, may be empty
  std::string ptp_gmid; int ptp_domain = 0;  // from ts-refclk
  uint32_t mediaclk_offset = 0;
};
struct SdpParseResult { std::optional<SdpStream> stream; std::string error; };
SdpParseResult parse_sdp(std::string_view text);          // accepts \n or \r\n
std::string generate_sdp(const SdpStream& s, uint64_t session_id, uint64_t version);
}
```
Rules: first `m=audio` section only; media-level `c=` overrides session-level; `a=ptime` wins over `a=framecount`; missing rtpmap for dynamic PT → error "brak a=rtpmap"; encodings other than L16/L24 → error "nieobsługiwane kodowanie: X"; rate ≠ 48000 → error "nieobsługiwana częstotliwość: N Hz"; channels not 1–2 → error; non-multicast address → error "adres nie jest multicastem".

- [ ] Step 1: Tests:
```cpp
TEST_CASE("parse Orban SDP") {
  auto r = bam::parse_sdp(read_file(BAM_TEST_DATA "/orban.sdp"));
  REQUIRE(r.stream);
  auto& s = *r.stream;
  CHECK(s.session_name == "Optimod_114969_2");
  CHECK(s.multicast_address == "239.1.14.100"); CHECK(s.ttl == 15); CHECK(s.port == 5004);
  CHECK(s.payload_type == 98); CHECK(s.encoding == bam::Encoding::L24);
  CHECK(s.sample_rate == 48000); CHECK(s.channels == 2);
  CHECK(s.frames_per_packet == 64); CHECK(s.ptime_ms == doctest::Approx(1.3333).epsilon(0.001));
  CHECK(s.source_filter == "10.80.14.100"); CHECK(s.origin_address == "10.80.14.100");
  CHECK(s.ptp_gmid == "30-D6-59-FF-FE-01-C1-19"); CHECK(s.ptp_domain == 0);
}
TEST_CASE("framecount used when no ptime") { /* SDP with a=framecount:48, no ptime → frames 48 */ }
TEST_CASE("rejects 44.1k") { /* rtpmap L24/44100/2 → error contains "44100" */ }
TEST_CASE("rejects unicast") { /* c=IN IP4 10.0.0.1 → error */ }
TEST_CASE("generate round-trips") {
  bam::SdpStream s; s.session_name="BounceAES 01"; s.origin_address="10.80.14.50";
  s.multicast_address="239.69.100.1"; s.ttl=15; s.port=5004; s.payload_type=98;
  s.encoding=bam::Encoding::L24; s.sample_rate=48000; s.channels=2; s.ptime_ms=1; s.frames_per_packet=48;
  s.ptp_gmid="00-11-22-FF-FE-33-44-55"; s.ptp_domain=0;
  auto text = bam::generate_sdp(s, 1234, 1);
  auto r = bam::parse_sdp(text); REQUIRE(r.stream);
  CHECK(r.stream->multicast_address == "239.69.100.1"); CHECK(r.stream->frames_per_packet == 48);
  CHECK(text.find("a=mediaclk:direct=0") != std::string::npos);
  CHECK(text.find("a=ts-refclk:ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0") != std::string::npos);
}
```
- [ ] Step 2: Run → FAIL. Step 3: implement line-based parser. Step 4: PASS. Step 5: commit `feat(core): SDP parser and generator`.

### Task 3: RTP header and PCM codecs

**Files:** `core/include/bam/rtp.hpp`, `core/src/rtp.cpp`, `tests/test_rtp.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
inline constexpr size_t kRtpHeaderSize = 12;
struct RtpHeader { uint8_t payload_type = 0; bool marker = false; uint16_t sequence = 0; uint32_t timestamp = 0; uint32_t ssrc = 0; };
struct RtpPacketView { RtpHeader header; std::span<const uint8_t> payload; };
std::optional<RtpPacketView> parse_rtp(std::span<const uint8_t> packet); // v2 only; skips CSRC/extension; strips padding
void write_rtp_header(const RtpHeader& h, uint8_t* out);                  // 12 bytes
// interleaved big-endian payload -> planar int32 left-aligned (L24: s<<8, L16: s<<16)
void decode_pcm(Encoding e, const uint8_t* payload, size_t frames, int channels, int32_t* const* planar_out);
// planar int32 -> interleaved big-endian L24 (top 24 bits)
void encode_l24(const int32_t* const* planar_in, int channels, size_t frames, uint8_t* out);
uint64_t unwrap_rtp_timestamp(uint32_t ts, uint64_t reference_sample); // 64-bit value nearest to reference with low 32 bits == ts
}
```
- [ ] Tests: header round-trip; parse rejects version≠2 and too-short; CSRC count 2 skipped; padding stripped; L24 decode of `{0x7F,0xFF,0xFF, 0x80,0x00,0x00}` mono → `{0x7FFFFF00, (int32_t)0x80000000}`; L16 decode `{0x12,0x34}` → `0x12340000`; encode/decode L24 round-trip with low byte cleared; unwrap: ref=0x1'0000'0010, ts=0xFFFFFFF0 → 0x0'FFFF'FFF0; ref=0x0'FFFF'FFF0, ts=0x10 → 0x1'0000'0010; ref=1000, ts=900 → 900.
- [ ] Implement, PASS, commit `feat(core): RTP header and L16/L24 codecs`.

### Task 4: MediaClock and ClockSource

**Files:** `core/include/bam/clock.hpp`, `core/src/clock.cpp`, `tests/test_clock.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
class MediaClock {           // ptp_ns = base_ptp + (local_ns - base_local) * rate
public:
  void set_mapping(int64_t local_ns, int64_t ptp_ns, double rate);
  int64_t to_ptp(int64_t local_ns) const;
  int64_t to_local(int64_t ptp_ns) const;
  double rate() const;
  int64_t base_local() const; int64_t base_ptp() const;
  static uint64_t sample_from_ptp(int64_t ptp_ns);   // floor(ptp_ns * 48000 / 1e9), ptp_ns >= 0
  static int64_t ptp_from_sample(uint64_t sample);   // smallest ns with sample_from_ptp(ns) == sample
  uint64_t sample_at(int64_t local_ns) const;
  int64_t local_for_sample(uint64_t sample) const;
};
class ClockSource {          // thread-safe holder; generation increments on step
public:
  MediaClock get() const; void set(const MediaClock& c, bool stepped);
  uint64_t step_generation() const;
};
}
```
- [ ] Tests: identity mapping; `sample_from_ptp(1'000'000'000) == 48000`; property loop over 10k random samples up to 2^47: `sample_from_ptp(ptp_from_sample(s)) == s` and `sample_from_ptp(ptp_from_sample(s)-1) == s-1`; rate 1.0001 mapping to_local(to_ptp(x)) ≈ x ±1 ns; ClockSource step generation increments only on stepped=true.
- [ ] Implement using split seconds/remainder arithmetic (no 128-bit). PASS. Commit `feat(core): media clock`.

### Task 5: Timeline rings and shared memory layout

**Files:** `core/include/bam/timeline.hpp`, `core/src/timeline.cpp`, `core/include/bam/shm_layout.hpp`, `core/src/shm_layout.cpp`, `tests/test_timeline.cpp`, `tests/test_shm_layout.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
uint32_t lap_tag(uint64_t sample);    // uint32(sample >> 16) + 1, never 0
class ChannelRing {
public:
  ChannelRing() = default;
  ChannelRing(int32_t* samples, uint32_t* tags);   // kRingFrames samples, kBlocksPerRing tags
  bool valid() const;
  // src==nullptr writes silence; blocks entered in a new lap are zeroed before writing; tag stored with release
  void write(uint64_t start, const int32_t* src, size_t count);
  void read(uint64_t start, int32_t* dst, size_t count) const;   // blocks whose tag != lap_tag → zeros
};
inline constexpr uint32_t kShmMagic = 0x42414D31, kShmVersion = 1;
inline constexpr const char* kShmName = "BounceAESMaster.v1";
inline constexpr const char* kPeriodEventName = "BounceAESMaster.v1.period";
enum class ShmPtpState : uint32_t { Listening = 0, Locked = 1, Holdover = 2, FreeRun = 3 };
struct ShmHeader {
  uint32_t magic, version, sample_rate, ring_frames, block_frames, channels_in, channels_out, header_size;
  std::atomic<uint64_t> service_generation;   // changes on every service start
  std::atomic<uint64_t> heartbeat;            // service increments ≥ every 10 ms
  std::atomic<uint64_t> period_counter;
  std::atomic<uint64_t> period_start_sample;
  std::atomic<uint32_t> ptp_state;
  std::atomic<uint32_t> buffer_frames;        // set by client before start
  std::atomic<uint32_t> client_pid;           // 0 = free; claim via compare_exchange
  std::atomic<uint32_t> client_running;
  std::atomic<uint64_t> client_heartbeat;
  std::atomic<uint32_t> output_latency_frames;
  std::atomic<uint32_t> device_active_mask;
  std::atomic<uint32_t> rx_latency_frames[32];
  std::atomic<uint32_t> names_version;        // odd while writing (seqlock)
  char device_names[32][48];
};
struct ShmLayout {
  static size_t header_bytes();     // sizeof(ShmHeader) rounded up to 4096
  static size_t total_bytes();
  static size_t in_samples_offset(int ch), out_samples_offset(int ch), in_tags_offset(int ch), out_tags_offset(int ch);
};
class ShmView {
public:
  explicit ShmView(void* base);
  static void initialize(void* base, uint64_t generation);   // placement-new header, zero rings/tags
  bool compatible() const;                                   // magic/version/sizes match
  ShmHeader& header() const;
  ChannelRing input(int ch) const; ChannelRing output(int ch) const;
  void set_device_name(int dev, std::string_view name);      // seqlock write
  std::string device_name(int dev) const;                    // seqlock read
};
}
```
- [ ] Tests: write 100 samples at 65530 (wraps) then read back; read unwritten region → zeros; write at lap L, read same index at lap L+1 → zeros; partial block write in new lap zeroes the rest of the block; `write(nullptr)` → zeros but valid; ShmView over `std::vector<std::byte>(ShmLayout::total_bytes())` initializes, `compatible()`, input(63)/output(0) don't overlap (pointer ranges), name seqlock round-trip with UTF-8 "Kraków"; `static_assert(std::atomic<uint64_t>::is_always_lock_free)`.
- [ ] Implement, PASS, commit `feat(core): timeline rings and shared memory layout`.

### Task 6: Gain, meters, RxStream

**Files:** `core/include/bam/gain.hpp`, `core/src/gain.cpp`, `core/include/bam/meters.hpp`, `core/src/meters.cpp`, `core/include/bam/rx_stream.hpp`, `core/src/rx_stream.cpp`, `tests/test_gain_meters.cpp`, `tests/test_rx_stream.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
double db_to_gain(double db);
void apply_gain(int32_t* s, size_t n, double gain);   // no-op when gain == 1.0; saturating
float peak_dbfs(const int32_t* s, size_t n);           // max |s| / 2^31 in dBFS, floor -120
struct RxStats { uint64_t packets = 0, bytes = 0, underruns = 0, overruns = 0, seq_errors = 0; double jitter_ms = 0; };
class RxStream {
public:
  RxStream(const SdpStream& sdp, ChannelRing left, ChannelRing right, uint32_t latency_frames, double gain);
  void on_packet(std::span<const uint8_t> bytes, uint64_t now_sample, int64_t now_local_ns);
  void on_tick(uint64_t now_sample);            // underrun accounting at deadline = now - latency
  RxStats stats() const;                        // thread-safe snapshot (atomics)
  void reset_stats();
  bool has_signal(int64_t now_local_ns) const;  // last packet within 1 s
  uint32_t latency_frames() const;
  const SdpStream& sdp() const;
};
}
```
Rules: PT mismatch → ignored (not counted); `ts` unwrapped against `now_sample`; packet end ≤ `now - latency` → overruns++ (late), start > `now + 24000` → overruns++ (future); sequence gap → seq_errors++ (not for first packet); jitter per RFC 3550 in ms; mono written to both rings; underruns: on tick, frames in `[max(highest_end, checked_until), now - latency)` missing while stream active counted as `ceil(missing / frames_per_packet)`; never counted before first packet.
- [ ] Tests (build packets with `write_rtp_header` + `encode_l24`): in-time packet lands at its timestamp in both rings; mono stream duplicates; late packet rejected → overruns 1 and ring untouched; sequence gap counts 1; underrun: packets for [0,480) then tick at now=480+240+96 with latency 240 → underruns == 2 (frames_per_packet 48); gain 2.0 (≈+6 dB) doubles sample and saturates at INT32_MAX; `peak_dbfs` of full-scale = 0, of half = −6.02, of zeros = −120.
- [ ] Implement, PASS, commit `feat(core): RX stream`.

### Task 7: TxStream

**Files:** `core/include/bam/tx_stream.hpp`, `core/src/tx_stream.cpp`, `tests/test_tx_stream.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
struct TxConfig { uint32_t frames_per_packet = 48; uint8_t payload_type = 98; uint32_t ssrc = 0; double gain = 1.0; };
struct TxStats { uint64_t packets = 0, bytes = 0, errors = 0; };
using PacketSink = std::function<void(std::span<const uint8_t>)>;
class TxStream {
public:
  TxStream(TxConfig cfg, ChannelRing left, ChannelRing right);
  size_t pump(uint64_t now_sample, const PacketSink& send);   // sends every packet with T + P <= now
  void reset();                       // forget position (after clock step) — next pump re-aligns
  void count_send_error();
  TxStats stats() const; void reset_stats();
  const TxConfig& config() const;
};
}
```
Rules: first pump aligns `next = floor(now/P)*P - P`; if `now - (next + P) > 2P` → skip to re-aligned position, errors += skipped packets; packet = 12-byte header + L24 stereo payload; timestamp = uint32(next); sequence increments by 1; gain applied to a copy.
- [ ] Tests: ring filled with ramp; pump(now=480) after construction sends exactly 1 packet with ts 432 and payload equal to ramp[432..480); subsequent pump(now=576) sends 2 packets ts 480, 528 with seq +1, +2; pump jump +10 packets → errors == 7 (10 − 1 − 2 allowed) and only 1 sent; reset() then pump continues without errors; P=64 alignment ts multiple of 64.
- [ ] Implement, PASS, commit `feat(core): TX stream`.

### Task 8: PTP messages

**Files:** `core/include/bam/ptp_messages.hpp`, `core/src/ptp_messages.cpp`, `tests/test_ptp_messages.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
struct ClockIdentity { std::array<uint8_t, 8> bytes{}; std::string to_string() const; /* "30-D6-59-FF-FE-01-C1-19" */
  bool operator==(const ClockIdentity&) const = default; bool is_zero() const; };
struct PortIdentity { ClockIdentity clock; uint16_t port = 0; bool operator==(const PortIdentity&) const = default; };
enum class PtpMsgType : uint8_t { Sync = 0x0, DelayReq = 0x1, FollowUp = 0x8, DelayResp = 0x9, Announce = 0xB, Other = 0xFF };
struct PtpTimestamp { uint64_t seconds = 0; uint32_t nanoseconds = 0; int64_t to_ns() const; static PtpTimestamp from_ns(int64_t); };
struct PtpHeader { PtpMsgType type; uint8_t version = 2; uint16_t length = 0; uint8_t domain = 0; uint16_t flags = 0;
  int64_t correction_scaled = 0; PortIdentity source; uint16_t sequence = 0; int8_t log_interval = 0;
  bool two_step() const; int64_t correction_ns() const; };
struct AnnounceBody { int16_t utc_offset = 37; uint8_t priority1 = 128, clock_class = 248, clock_accuracy = 0xFE;
  uint16_t variance = 0xFFFF; uint8_t priority2 = 128; ClockIdentity gm_identity; uint16_t steps_removed = 0; uint8_t time_source = 0xA0; };
struct PtpMessage { PtpHeader header; PtpTimestamp timestamp; PortIdentity requesting; AnnounceBody announce; };
std::optional<PtpMessage> parse_ptp(std::span<const uint8_t> data);
std::vector<uint8_t> build_delay_req(uint8_t domain, const PortIdentity& self, uint16_t seq);
std::vector<uint8_t> build_sync(uint8_t domain, const PortIdentity& src, uint16_t seq, bool two_step, PtpTimestamp origin);
std::vector<uint8_t> build_follow_up(uint8_t domain, const PortIdentity& src, uint16_t seq, PtpTimestamp precise);
std::vector<uint8_t> build_delay_resp(uint8_t domain, const PortIdentity& src, uint16_t seq, PtpTimestamp rx, const PortIdentity& requesting);
std::vector<uint8_t> build_announce(uint8_t domain, const PortIdentity& src, uint16_t seq, const AnnounceBody& body);
int compare_announce(const AnnounceBody& a, const AnnounceBody& b); // <0 if a is better (IEEE 1588 dataset comparison)
}
```
- [ ] Tests: each builder round-trips through `parse_ptp`; Sync length 44, Follow_Up 44, Delay_Req 44, Delay_Resp 54, Announce 64; two-step flag 0x0200; correction 0x0000'0001'0000 scaled → 1 ns; version≠2 rejected; ClockIdentity string format; compare_announce prefers lower priority1, then clock class, then identity.
- [ ] Implement, PASS, commit `feat(core): PTP message codec`.

### Task 9: PTP slave and servo

**Files:** `core/include/bam/ptp_slave.hpp`, `core/src/ptp_slave.cpp`, `tests/test_ptp_slave.cpp`

**Interfaces — Consumes:** Task 4 `MediaClock`, Task 8 codec. **Produces:**
```cpp
namespace bam {
enum class PtpState { Listening, Locked, Holdover };
struct PtpStatus { PtpState state = PtpState::Listening; bool has_master = false; ClockIdentity grandmaster; PortIdentity master;
  int64_t offset_ns = 0; int64_t path_delay_ns = 0; uint64_t sync_lost = 0; uint64_t steps = 0; uint8_t domain = 0; double rate_ppm = 0; };
class PtpSlave {
public:
  PtpSlave(uint8_t domain, ClockIdentity self, MediaClock initial);
  void on_message(std::span<const uint8_t> bytes, int64_t rx_local_ns);
  std::optional<std::vector<uint8_t>> poll_delay_req(int64_t now_local_ns);   // ~1/s once a master is selected
  void on_delay_req_sent(int64_t tx_local_ns);
  void tick(int64_t now_local_ns);       // master/sync timeouts
  const MediaClock& clock() const;
  bool consume_step();                   // true once after each clock step
  PtpStatus status() const;
};
}
```
Algorithm: BMCA over Announce (foreign master expires after 4 × announce interval, min 6 s); only messages from selected master port used; two-step Sync paired with Follow_Up by sequence; `t1 = origin + corrections`, `t2s = clock.to_ptp(t2_local)`; delay from Delay_Resp: `mpd = ((t2s−t1) + (t4−t3s))/2`, median of last 9; offset = median-of-5 of `(t2s − t1 − mpd)`; first measurement steps clock (rate kept); later: |offset| > 1 ms on 3 consecutive → step (steps++); otherwise PI: `freq += -Ki*offset/T`, `rate = freq − Kp*offset/T`, `Kp = 0.3, Ki = 0.05`, rebase mapping at t2 to keep continuity; freq clamped ±500 ppm; Locked after 4 consecutive |offset| < 500 µs; no sync for 3 s or master expired → Holdover (sync_lost++ when leaving Locked), mapping/frequency kept.
- [ ] Tests (simulation helper: master clock = true time; slave local = true × (1 + drift) + offset; network delay 100 µs + uniform jitter 0–50 µs; Sync every 125 ms two-step, Announce every 1 s, Delay_Resp to each Delay_Req):
  - locks within 20 s with drift +100 ppm and initial offset 3 s; then for 60 s |true_ptp − clock.to_ptp(local)| < 50 µs at every sync, steps == 1.
  - drift −80 ppm also converges.
  - master silent for 5 s → Holdover, sync_lost == 1; error after 5 s holdover < 1 ms; master returns → Locked again, no extra step.
  - better master Announce appears → switches master (status.master changes).
  - messages from other domain ignored.
- [ ] Implement, PASS, commit `feat(core): PTP slave with PI servo`.

### Task 10: SAP

**Files:** `core/include/bam/sap.hpp`, `core/src/sap.cpp`, `tests/test_sap.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
struct SapPacket { bool deletion = false; uint16_t hash = 0; std::array<uint8_t, 4> origin{}; std::string payload_type; std::string sdp; };
std::optional<SapPacket> parse_sap(std::span<const uint8_t> data);        // v1, IPv4, no encryption/compression
std::vector<uint8_t> build_sap(bool deletion, uint16_t hash, std::array<uint8_t, 4> origin, std::string_view sdp);
uint16_t sap_hash(std::string_view text);                                   // FNV-1a 32 folded to 16, never 0
struct DiscoveredStream { std::string id; std::string name; std::string origin; std::string sdp; SdpStream parsed; int64_t last_seen_ns = 0; };
class SapDirectory {
public:
  void on_packet(std::span<const uint8_t> data, int64_t now_ns);     // only parseable audio SDPs kept
  void expire(int64_t now_ns);                                        // drop after 300 s unseen
  std::vector<DiscoveredStream> list() const;                         // sorted by name
  std::optional<DiscoveredStream> find(std::string_view id) const;
};
}
```
- [ ] Tests: build/parse round-trip with "application/sdp"; deletion removes entry; announcement with Orban SDP listed with name "Optimod_114969_2", id = "10.80.14.100/<hash>"; expire after 300 s; garbage ignored.
- [ ] Implement, PASS, commit `feat(core): SAP announce/discovery`.

### Task 11: Config model, JSON, validation

**Files:** `core/include/bam/config.hpp`, `core/src/config.cpp`, `tests/test_config.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
struct DeviceConfig {
  bool enabled = false; std::string name;
  std::string tx_address; uint32_t tx_ptime_us = 1000; bool keep_active = true;
  std::string rx_sdp; uint32_t rx_latency_us = 5000;
};
struct Config {
  std::string interface_ip;                 // empty = first non-loopback IPv4
  uint8_t ptp_domain = 0;
  int pc_nominal_level_dbfs = -20;
  std::string http_bind = "0.0.0.0"; uint16_t http_port = 8067;
  std::string password_hash;                // empty = not set
  std::array<DeviceConfig, 32> devices;
};
Config default_config();                    // names "BounceAES 01".., tx 239.69.100.N, device 1 enabled
std::string config_to_json(const Config& c);           // pretty, stable key order
std::optional<Config> config_from_json(std::string_view text, std::string* error);  // missing keys → defaults
struct ConfigIssue { int device = 0; std::string field; std::string message; };      // device 0 = global, 1..32
std::vector<ConfigIssue> validate_config(const Config& c);
std::vector<std::string> normalize_config(Config& c);  // raises rx latency to ptime+1ms; returns Polish notices
uint32_t ptime_us_to_frames(uint32_t us);              // 1000→48, 1333→64, 4000→192
double gain_to_app(const Config& c); double gain_to_network(const Config& c);
}
```
Validation: tx_address must be IPv4 multicast 224.0.0.0/4 excluding 224.0.0.0/24; tx_ptime_us ∈ {1000,1333,4000}; duplicate tx addresses among enabled devices; rx_sdp must parse when non-empty; rx_latency_us in range; level in −30…0; http_port ≠ 0; name ≤ 40 bytes.
- [ ] Tests: default validates clean; JSON round-trip equality; unknown keys ignored; bad ptime flagged on device 3; duplicate address flagged; Orban SDP with latency 1500 µs normalized to 2334 µs (ceil(1333.33)+1000) with notice; gains: −20 → 1.0/1.0, −8 → ≈3.98/≈0.251.
- [ ] Implement, PASS, commit `feat(core): configuration model`.

### Task 12: Auth (SHA-256, PBKDF2, sessions)

**Files:** `core/include/bam/auth.hpp`, `core/src/auth.cpp`, `tests/test_auth.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> msg);
std::vector<uint8_t> pbkdf2_sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations, size_t len);
std::string to_hex(std::span<const uint8_t>);
std::string random_hex(size_t bytes);
std::string make_password_hash(std::string_view password);          // "pbkdf2-sha256$100000$<salt>$<hash>"
bool verify_password(std::string_view password, std::string_view stored);  // constant-time compare
class SessionStore {
public:
  std::string create(int64_t now_ns);                 // 12 h lifetime
  bool valid(std::string_view token, int64_t now_ns);  // sliding expiry
  void remove(std::string_view token);
};
}
```
- [ ] Tests: SHA-256("abc") = `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`; HMAC RFC 4231 case 2; PBKDF2-HMAC-SHA256("password","salt",1,32) = `120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b`; hash/verify round-trip, wrong password false, malformed stored false; session expiry.
- [ ] Implement, PASS, commit `feat(core): password hashing and sessions`.

### Task 13: AsioBridge and logger

**Files:** `core/include/bam/asio_bridge.hpp`, `core/src/asio_bridge.cpp`, `core/include/bam/log.hpp`, `core/src/log.cpp`, `tests/test_asio_bridge.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
class AsioBridge {
public:
  explicit AsioBridge(ShmView view);
  // inputs for the period starting at S: per device d reads [S - B - Lrx_d, S - Lrx_d)
  void read_inputs(uint64_t period_start, uint32_t frames, int32_t* const* in_bufs) const;   // 64 ptrs, nullptr = skip
  // outputs written to [S + B, S + 2B); nullptr channel writes silence
  void write_outputs(uint64_t period_start, uint32_t frames, const int32_t* const* out_bufs);
};
std::string channel_label(int channel, std::string_view device_name);  // "BounceAES 01 L" or "BounceAES 01 L (Kraków)", ≤31 bytes UTF-8 safe
enum class LogLevel { Info, Warning, Error };
struct LogEntry { int64_t unix_ms; LogLevel level; std::string message; };
class Logger {
public:
  static Logger& instance();
  void write(LogLevel level, std::string message);
  std::vector<LogEntry> recent(size_t max = 500) const;
  void add_sink(std::function<void(const LogEntry&)> sink);
};
void log_info(std::string m); void log_warning(std::string m); void log_error(std::string m);
}
```
- [ ] Tests: rx latency 240 for device 0, ring input(0) holds ramp; `read_inputs(S=4800,B=128)` returns ramp[4432..4560); write_outputs places data at [S+128, S+256) of output(1); nullptr outputs → zeros valid; channel_label(0,"BounceAES 01")=="BounceAES 01 L", channel_label(3,"Kraków")=="BounceAES 02 R (Kraków)", very long name truncated to ≤31 bytes without splitting a UTF-8 sequence; Logger keeps last 500.
- [ ] Implement, PASS, commit `feat(core): ASIO bridge and logger`.

### Task 14: Platform layer

**Files:** `platform/include/bam/platform/{time,udp,shm,event,process}.hpp`, `platform/src/posix/*.cpp`, `platform/src/win/*.cpp`, `tests/test_platform.cpp`

**Interfaces — Produces:**
```cpp
namespace bam::platform {
int64_t monotonic_ns();
int64_t realtime_ns();                      // Unix epoch ns
void sleep_until_ns(int64_t monotonic_target);   // high-resolution
void set_realtime_priority(const char* mmcss_task);   // "Pro Audio" on Windows; best-effort elsewhere
struct NetInterface { std::string name, description, ipv4; bool up = false; bool loopback = false; };
std::vector<NetInterface> list_interfaces();
std::string default_interface_ip();
bool net_init();   // WSAStartup on Windows
class UdpSocket {
public:
  UdpSocket() = default; UdpSocket(UdpSocket&&) noexcept; UdpSocket& operator=(UdpSocket&&) noexcept; ~UdpSocket();
  static std::optional<UdpSocket> open(uint16_t port, std::string* error);   // bind 0.0.0.0:port, SO_REUSEADDR(+PORT)
  bool join(const std::string& group, const std::string& iface_ip, const std::string& source, std::string* error);
  void leave_all();
  bool set_multicast_out(const std::string& iface_ip, int ttl, bool loopback);
  bool send_to(std::span<const uint8_t> data, const std::string& ip, uint16_t port);
  int recv(std::span<uint8_t> buf, std::string* from_ip);       // non-blocking; -1 none/error
  intptr_t native() const; bool valid() const;
};
int wait_readable(std::span<UdpSocket* const> sockets, int timeout_ms, std::vector<uint8_t>& ready);  // count ready
class SharedMemory {
public:
  static std::optional<SharedMemory> create(const std::string& name, size_t size, bool* existed, std::string* error);
  static std::optional<SharedMemory> open(const std::string& name, size_t size, std::string* error);
  void* data() const; size_t size() const;  // move-only, unmaps in dtor
};
class NamedEvent {
public:
  static std::optional<NamedEvent> create(const std::string& name, std::string* error);
  static std::optional<NamedEvent> open(const std::string& name, std::string* error);
  void signal(); bool wait(int timeout_ms);   // move-only
};
uint32_t current_pid(); bool process_alive(uint32_t pid);
}
```
Windows: `Global\` names with SDDL `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)`; falls back to `Local\` if creating in Global fails (console mode without privilege). POSIX: `shm_open("/name")`, event = `sem_open` + `sem_trywait` polling (100 µs) for timeouts.
- [ ] Tests (POSIX host): two sockets on port 15004 join 239.69.200.1 / 239.69.200.2 on loopback-capable interface; send to group 1 → only socket 1 receives (checks per-socket membership filtering); SharedMemory create+open see same bytes; NamedEvent signal → wait true, wait(10) without signal → false; `sleep_until_ns` accuracy < 2 ms.
- [ ] Implement POSIX + Windows; host tests PASS; MinGW cross build of platform library succeeds. Commit `feat(platform): sockets, shared memory, events, clocks`.

### Task 15: Engine (threads, config apply, status)

**Files:** `engine/include/bam/engine.hpp`, `engine/src/engine.cpp`, `engine/src/{ptp_worker,rx_worker,tx_worker,period_worker,sap_worker}.cpp`, `engine/src/engine_internal.hpp`, `tests/test_engine.cpp`

**Interfaces — Consumes:** all core + platform. **Produces:**
```cpp
namespace bam {
struct DeviceStatus { int index; bool enabled; std::string name; std::string tx_address; bool tx_active;
  bool rx_configured; bool rx_signal; std::string rx_source_name; std::string rx_error; std::string tx_conflict;
  RxStats rx; TxStats tx; float in_peak_db[2]; float out_peak_db[2]; uint32_t rx_latency_us; uint32_t tx_ptime_us; };
struct EngineStatus { PtpStatus ptp; bool ptp_free_run; bool asio_connected; uint32_t asio_buffer_frames;
  int64_t uptime_s; std::string interface_ip; std::string version; std::vector<DeviceStatus> devices; };
struct ApplyResult { bool ok; std::vector<ConfigIssue> issues; std::vector<std::string> notices; };
struct EngineOptions { std::string config_path; std::string shm_name = kShmName; std::string event_name = kPeriodEventName; bool enable_ptp = true; };
class Engine {
public:
  explicit Engine(EngineOptions opts);
  ~Engine();
  bool start(std::string* error);   // loads/creates config, shm, sockets, threads
  void stop();
  Config config() const;
  ApplyResult apply_config(Config next);     // validate → normalize → save atomically → apply diff live
  EngineStatus status() const;
  std::vector<DiscoveredStream> discovered_streams() const;
  std::string output_sdp(int device) const;  // device 1..32
  void reset_stats();
};
}
```
Threads: ptp (sockets 319/320, 10 ms poll), rx (wait_readable 2 ms, ticks + meters every 50 ms), tx (250 µs high-res loop, pumps all TxStreams, sends only if `keep_active || client_running`, resets on clock step), period (when `client_running && buffer_frames`: sleep to `local_for_sample(next k·B)`, publish `period_start_sample`, `period_counter++`, signal event; heartbeat every ≤10 ms; client liveness: `client_heartbeat` stale for 2 s or process dead → clear client), sap (announce enabled outputs every 30 s and on change, deletion on disable, listen & expire). Config is shared via `std::shared_ptr<const RuntimeSet>` swapped under mutex; streams whose parameters didn't change are kept (stats preserved). Interface IP change (polled every 5 s) → rejoin groups.
- [ ] Test (POSIX, loopback, PTP disabled → free-run): temp config with device 1 tx 239.69.201.1 ptime 1000, rx_sdp = engine.output_sdp(1) applied after start; a fake client claims shm (`client_pid`, `buffer_frames=128`, `client_running=1`) and writes a ramp into output ring 0/1 ahead of time via AsioBridge each period for 1 s; afterwards input ring 0 contains the ramp delayed by exactly the constant transport delay (verify continuity of 10 000 consecutive samples) and DeviceStatus rx.packets ≈ 1000 ± 50, underruns == 0; `apply_config` with invalid ptime returns ok=false and does not change running config.
- [ ] Implement, PASS, commit `feat(engine): service engine with rx/tx/ptp/period/sap workers`.

### Task 16: HTTP API and panel

**Files:** `engine/include/bam/http_server.hpp`, `engine/src/http_server.cpp`, `engine/src/json_api.cpp`, `panel/index.html`, `panel/app.js`, `panel/style.css`, `cmake/embed_files.cmake`, `tests/test_http.cpp`

**Interfaces — Produces:**
```cpp
namespace bam {
struct PanelAsset { const char* path; const char* mime; const unsigned char* data; size_t size; };
extern const PanelAsset kPanelAssets[]; extern const size_t kPanelAssetCount;
class HttpServer {
public:
  explicit HttpServer(Engine& engine);
  bool start(const std::string& bind, uint16_t port, std::string* error);   // own thread
  void stop();
};
}
```
Endpoints per spec §6.5 + `/api/health`, `/api/interfaces`, `/api/login`, `/api/logout`, `/api/setup`. Auth: cookie `bam_session` (HttpOnly, SameSite=Strict); 401 JSON `{"error":"..."}` when missing; `/api/setup` only from 127.0.0.1/::1 and only when no password; `/api/live` SSE `event: live` every 100 ms with `{ptp, devices:[{i, in:[l,r], out:[l,r], rx:{...}, tx:{...}, sig}]}`. PUT bodies validated through `Engine::apply_config`, response `{ok, issues, notices}`.
Panel (Polish, Axia-like): login/setup screen; "Konfiguracja" tab (global settings box + table of 32 rows: Aktywne, Nazwa, Wyjście adres, ptime, Utrzymuj strumień, Pokaż SDP, Wejście źródło + Przeglądaj… (modal with SAP list) + Wklej SDP (modal), Opóźnienie ms, mierniki L/R in/out); "Statystyki" tab (TxPkts, TxErr, RxPkts, RxBytes, RxUnderrun, RxOverrun, RxSeqErr, Jitter per device; PTP box with Synchronization Lost; Zeruj liczniki); "Logi" tab. Save button applies changes, shows issues inline.
- [ ] Tests (httplib client against Engine on POSIX): `/api/health` 200 without auth; `/api/config` 401 without cookie; `/api/setup` from 127.0.0.1 sets password then second call 409; login wrong → 401, right → cookie; GET `/api/config` returns 32 devices; PUT device 2 with ptime 999 → 400 with issue; `/api/devices/1/sdp` contains `a=rtpmap:98 L24/48000/2`; `/` serves HTML containing "BounceAES Master"; `/api/live` first event arrives within 500 ms.
- [ ] Implement, PASS; manual check: run `BounceAESService --console`, open panel in a browser (playwright screenshot). Commit `feat(engine): HTTP API and Axia-style web panel`.

### Task 17: Service entry point

**Files:** `service/main.cpp`, `service/win_service.cpp`, `service/win_service.hpp`

Behaviour: `BounceAESService --console [--config path]` runs engine + HTTP until Ctrl+C (all platforms); no args on Windows → `StartServiceCtrlDispatcherW` with service name `BounceAESMaster`, reports SERVICE_RUNNING, stops on SERVICE_CONTROL_STOP/SHUTDOWN; Windows logger sink → Event Log source "BounceAES Master" (warnings/errors) + rotating file `%ProgramData%\BounceAES Master\logs\bounceaes.log` (5 × 10 MB) on all platforms (`logs/` next to config). `--version` prints version.
- [ ] Verify: host `--console` starts, `curl /api/health` → `{"service":"ok",...}`, Ctrl+C exits 0; MinGW cross build produces `BounceAESService.exe`. Commit `feat(service): console and Windows service entry point`.

### Task 18: ASIO driver DLL

**Files:** `asio/asio_driver.hpp`, `asio/asio_driver.cpp`, `asio/dll_main.cpp`, `asio/BounceAESAsio.def`

**Interfaces — Consumes:** `AsioBridge`, `ShmView`, `platform::SharedMemory/NamedEvent`, `channel_label`.
Behaviour (IASIO): CLSID `{6B1F3C2A-8E4D-4B7A-9C51-2A7E0D4B3F10}`; `init` opens shm (`Global\` then `Local\`), checks `compatible()`, else error "BounceAES Master: usługa nie działa"; `getChannels` 64/64; `getBufferSize` min 64 max 512 pref 128 granularity −1 (powers of two); `canSampleRate` only 48000; `getClockSources` 1 "PTP (AES67)"; `getChannelInfo` type `ASIOSTInt32LSB`, name `channel_label`; `createBuffers` allocates double buffers, claims `client_pid` via CAS (busy → `ASE_NotPresent` + message "urządzenie zajęte"), writes `buffer_frames`; `start` sets `client_running=1`, starts thread (MMCSS "Pro Audio"): wait event (timeout 2×period) → if `period_counter` advanced: `read_inputs` into half `idx`, `bufferSwitchTimeInfo` (or `bufferSwitch`), `write_outputs` from half `idx`, `client_heartbeat++`, toggle idx; on service heartbeat stale > 100 ms → zero inputs and self-clock with sleep; if `service_generation` changed → `asioMessage(kAsioResetRequest)`; `getSamplePosition` from `period_start_sample` + `timeGetTime()*1e6`; `getLatencies` input `B + min Lrx`, output `B + output_latency_frames`; `controlPanel` opens `http://127.0.0.1:8067`; `stop`/`disposeBuffers` release claim. DllRegisterServer writes `HKCR\CLSID\{..}\InprocServer32` (ThreadingModel Apartment) and `HKLM\SOFTWARE\ASIO\BounceAES Master` (CLSID, Description).
- [ ] Verify: MinGW cross build produces `BounceAESAsio.dll` exporting `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer` (`x86_64-w64-mingw32-objdump -p`). Commit `feat(asio): ASIO driver DLL`.

### Task 19: Loopback test tool and PTP simulator

**Files:** `tools/bounceaes_test.cpp`, `tools/ptp_sim.cpp`, `docs/TESTING.md`

`bounceaes-test --loopback --seconds N [--devices 32] [--buffer 128]`: attaches to shm as client (same claim protocol as the DLL), writes per channel `((n * 31 + ch) & 0xFFFFFF) << 8`, reads inputs, after warm-up (1 s) verifies each input channel decodes consecutive values differing by 31 (mod 2^24); prints per-channel OK/errors and exits 0 when no discontinuities. `--configure-loopback` uses the HTTP API (with `--password`) to set devices 1..N rx_sdp = own output SDP. `ptp-sim --domain 0 --iface IP`: two-step master (Sync 8/s, Announce 1/s, answers Delay_Req).
- [ ] Verify on macOS: service `--console` + `bounceaes-test --configure-loopback --devices 32` + `bounceaes-test --loopback --seconds 60 --devices 32` → exit 0, zero discontinuities; with `ptp-sim` running service status shows Locked. Commit `feat(tools): loopback tester and PTP simulator`.

### Task 20: Tray application

**Files:** `tray/tray_main.cpp`, `tray/tray.rc` (manifest only)

Behaviour: single instance (named mutex); `Shell_NotifyIconW` icon drawn at runtime (16×16 colored circle: green Locked, yellow Holdover/Listening/FreeRun, red no service); polls `GET http://127.0.0.1:<port>/api/health` every 2 s via WinHTTP (port from config file, default 8067); tooltip "BounceAES Master — PTP: zsynchronizowany"; menu: "Otwórz panel", "Statystyki" (`#stats`), "Zakończ"; double-click opens panel: `ShellExecuteW("msedge", "--app=URL")`, fallback `ShellExecuteW(url)`.
- [ ] Verify: MinGW cross build produces `BounceAESTray.exe`. Commit `feat(tray): system tray status app`.

### Task 21: Installer, CI, documentation

**Files:** `installer/BounceAES.wxs`, `installer/install.ps1`, `installer/uninstall.ps1`, `.github/workflows/build.yml`, `README.md`, `docs/INSTALL.md`, `docs/TESTING.md`

- WiX v4: per-machine x64, `ProgramFiles64Folder\BounceAES Master`; files service/dll/tray/LICENSE/docs; `ServiceInstall` (BounceAESMaster, auto, LocalSystem) + `util:ServiceConfig` restart 5 s ×3; registry for ASIO (HKCR CLSID + HKLM\SOFTWARE\ASIO); `fire:FirewallException` UDP 5004/319/320/9875, TCP 8067; HKLM Run `BounceAESTray`; MajorUpgrade.
- `install.ps1` (admin): copies to Program Files, `sc.exe create/failure/start`, `regsvr32 /s`, `netsh advfirewall` rules, Run key, checks `w32time` PTP provider and warns. `uninstall.ps1` reverses.
- CI: `linux` job (configure, build, ctest, loopback test 20 s); `windows` job (MSVC x64 Release, ctest, `dotnet tool install --global wix`, `wix extension add WixToolset.Util.wixext WixToolset.Firewall.wixext`, `wix build`, upload MSI + zip artifact).
- Docs (Polish): README (what it is, features, Axia comparison, build), INSTALL (install, firewall, NIC tuning: disable interrupt moderation/EEE, power plan, PTP check, Orban SDP import, mAirList ASIO setup), TESTING (unit, loopback, 72 h soak procedure and acceptance criteria).
- [ ] Verify: full host build + ctest PASS; MinGW full Windows build PASS; workflow YAML lints (`python3 -c "import yaml"` parse). Commit `build: installer, CI and documentation`.

---

## Self-review

- Spec coverage: §3 signal (T2, T3, T6, T7), §4 components (T14–T20), §5 timing (T4, T5, T9, T13, T15), §5.6 nominal level (T6, T11), §6 panel/stats/auth/API (T11, T12, T16), §7 errors (T9 holdover, T15 liveness/rejoin, T18 heartbeat/reset, T16 validation), §8 tests (all tasks, T19, T21 CI), §9 risks (T15 MMCSS/timer, T21 docs NIC tuning/w32time).
- Names consistent: `ChannelRing`, `ShmView`, `AsioBridge`, `RxStream`, `TxStream`, `PtpSlave`, `SapDirectory`, `Engine`, `HttpServer` used identically across tasks.
- Implementation code is authored during execution against the interfaces and tests above (plan kept at interface level because of project size); every task ends with a host test run or a cross-compile check.
