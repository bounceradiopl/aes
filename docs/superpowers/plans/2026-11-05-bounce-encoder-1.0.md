# Bounce Encoder 1.0 — plan implementacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A separate Windows service + web panel "Bounce Encoder": stations of up to 3 AES67 inputs (SDP, gain, manual delay 0–60 s), summed, sent as up to 8 Icecast streams per station in MP3 / AAC-LC / HE-AAC / HE-AACv2 / FLAC.

**Architecture:** New `encoder/` tree in the BounceAES repo reusing `core/` (RTP, SDP, `RxStream`, `ChannelRing`, `PtpPort`, `MediaClock`, `rate_control` resampler, `auth`, `log`) and `platform/` (UDP, time, process). New platform pieces: TCP client/listener and dynamic library loading. Codec libraries are fetched from pinned source tarballs and built by CMake: LAME and fdk-aac as shared libraries loaded at run time (licence separation), libFLAC + libogg linked statically. BounceAES Master behaviour stays unchanged.

**Spec:** `docs/superpowers/specs/2026-11-05-bounce-encoder-design.md`

## Global Constraints

- Limits: 16 stations, 3 inputs per station, 8 streams per station; input/stream gain −60…+12 dB (0.5 dB steps); input delay 0–60 s (0.1 s steps); 5 ms (240-frame) ramps for every gain/mute/join/leave/delay change.
- Codec table (spec §5.2) is binding: MP3 32–320 kb/s @ 48000/44100/32000/24000/22050; AAC-LC 32–320 @ same; HE-AAC 24–128 @ 48000/44100/32000; HE-AACv2 16–64 @ 48000/44100/32000 (stereo); FLAC level 5, 16 bit, Ogg, @ any of the five rates. Content types: `audio/mpeg`, `audio/aac` (LC), `audio/aacp` (HE, HEv2), `audio/ogg` (FLAC).
- Icecast: `PUT /<mount>` with Basic auth, `Content-Type`, `Ice-Public: 0`, `Ice-Name`, `Expect: 100-continue`; fallback `SOURCE /<mount> ICE/1.0` on 400/405; retry 1, 2, 5, 10 s then every 10 s; 401/403/unreachable mapped to Polish messages.
- No processing, no RDS, no ICY metadata. Panel port 8070. Config file `%ProgramData%\Bounce Encoder\config.json` (`"version": 1`); passwords never returned by the API (empty = unchanged).
- Tests and demos only on 127.0.0.1. Never send multicast or connect to non-loopback hosts from tests.
- User text Polish, code comments English, style of the surrounding code (C++20, doctest, nlohmann/json, cpp-httplib).
- After each task: `cmake --build build -j8 && ./build/bam_tests` green; `build-win` and `build-win32` build without warnings (build-win32 may skip the encoder if a codec does not build for 32-bit — the encoder ships 64-bit only). Commits end with a blank line and `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Do not push.

---

### Task 1: Platform — TCP and dynamic libraries

**Files:** `platform/include/bam/platform/tcp.hpp`, `platform/include/bam/platform/dynlib.hpp`, `platform/src/posix/tcp_posix.cpp`, `platform/src/win/tcp_win.cpp`, `platform/src/posix/dynlib_posix.cpp`, `platform/src/win/dynlib_win.cpp`, `tests/test_tcp.cpp`, `tests/test_dynlib.cpp`.

Interfaces:
```cpp
namespace bam::platform {
class TcpStream {  // client connection
 public:
  static std::optional<TcpStream> connect(const std::string& host, uint16_t port, int timeout_ms, std::string* error);  // resolves names (getaddrinfo), IPv4
  bool send_all(std::span<const uint8_t> data, int timeout_ms);  // false on error/timeout
  int recv(std::span<uint8_t> buf, int timeout_ms);              // bytes, 0 = closed, -1 = timeout/error
  void close(); bool valid() const;
};
class TcpListener {  // for tests and local tools
 public:
  static std::optional<TcpListener> listen(const std::string& ip, uint16_t port, std::string* error);  // port 0 = any
  uint16_t port() const;
  std::optional<TcpStream> accept(int timeout_ms);
};
class DynamicLibrary {
 public:
  static std::optional<DynamicLibrary> open(const std::vector<std::string>& candidates, std::string* error);  // first that loads
  void* symbol(const char* name) const;
  const std::string& path() const;
};
}
```
- [ ] RED tests: listener on 127.0.0.1 port 0, client connects, 1 MB sent both ways intact, recv timeout returns −1, connect to a closed port fails with an error, connect to an unresolvable name fails; dynlib opens a system library (`libc` / `kernel32.dll`) and finds a known symbol, missing library returns an error listing the candidates.
- [ ] Implement (POSIX sockets / Winsock, `dlopen` / `LoadLibraryW` with UTF-8→UTF-16). Windows: `WSAStartup` once (reuse the UDP init if present).
- [ ] Tests green on host, MinGW 64/32 build clean; commit `platform: TCP streams and dynamic libraries`.

### Task 2: Codec libraries in the build

**Files:** `cmake/codecs.cmake`, `cmake/lame/CMakeLists.txt` (LAME has no CMake), `CMakeLists.txt` (option `BAM_WITH_CODECS`, default ON), `tests/test_codec_libs.cpp`, `.gitignore` if needed.

- [ ] `cmake/codecs.cmake` with `FetchContent` (or `ExternalProject`) and **URL_HASH SHA256** for: LAME 3.100 (sourceforge tarball), fdk-aac 2.0.3 (github release tarball), libogg 1.3.5, flac 1.4.3 (xiph tarballs). Verify the hashes yourself (`shasum -a 256` of the downloaded files) and pin them.
  - LAME: own `cmake/lame/CMakeLists.txt` building `libmp3lame` + `mpglib` (decoder, for tests) as a **shared** library `mp3lame` (exports from `include/libmp3lame.sym`/`.def`; `HAVE_MPGLIB`, `config.h` generated minimal). 
  - fdk-aac: its own CMake, `BUILD_SHARED_LIBS ON` for that subproject only, output `fdk-aac` (Windows: `libfdk-aac-2.dll` naming via `OUTPUT_NAME`).
  - libogg, libFLAC: static, no programs/tests/docs (`BUILD_PROGRAMS OFF`, `BUILD_EXAMPLES OFF`, `BUILD_TESTING OFF`, `INSTALL_MANPAGES OFF`, `WITH_OGG ON`).
  - Shared outputs land next to the executables (`CMAKE_RUNTIME_OUTPUT_DIRECTORY` for the build tree) so run-time loading finds them.
  - Must work for: host macOS (clang), MinGW 64 cross (`build-win`), MSVC x64 (CI). For `build-win32` set `BAM_WITH_CODECS OFF` if a codec fails there (document).
- [ ] Test `tests/test_codec_libs.cpp`: `DynamicLibrary` loads mp3lame and fdk-aac from the build directory (path passed by CMake as a compile definition), symbols `lame_init`, `get_lame_version`, `aacEncOpen`, `aacDecoder_Open` exist; libFLAC `FLAC__VERSION_STRING` is non-empty.
- [ ] Commit `build: fetch and build LAME, fdk-aac, libogg, libFLAC (pinned)`.

### Task 3: Encoders

**Files:** `encoder/include/bam/encoder/audio_encoder.hpp`, `encoder/src/codec_lame.cpp`, `encoder/src/codec_fdk.cpp`, `encoder/src/codec_flac.cpp`, `encoder/src/codecs.cpp` (factory, availability, settings table), `tests/test_encoders.cpp`; CMake target `bam_encoder` (static lib).

Interfaces (spec §5.1), plus:
```cpp
enum class Codec { Mp3, AacLc, HeAac, HeAacV2, Flac };
struct CodecLimits { uint32_t min_kbps, max_kbps; std::vector<uint32_t> rates; bool lossless; };
const CodecLimits& codec_limits(Codec);
std::optional<Codec> parse_codec(std::string_view);  // "mp3","aac","he-aac","he-aac-v2","flac"
const char* codec_name(Codec);
// optional search dirs for run-time libraries (default: directory of the executable)
void set_codec_library_dirs(std::vector<std::string> dirs);
```
- LAME via function pointers from `DynamicLibrary` (`lame_init`, `lame_set_*`, `lame_init_params`, `lame_encode_buffer_interleaved_ieee_float`, `lame_encode_flush`, `lame_close`), CBR, quality 2, stereo.
- fdk-aac via pointers (`aacEncOpen`, `aacEncoder_SetParam` AOT 2/5/29, `AACENC_TRANSMUX` = ADTS(2), `AACENC_BITRATE`, `AACENC_SAMPLERATE`, `AACENC_CHANNELMODE` 2, `AACENC_AFTERBURNER` 1, `aacEncEncode`), feed 16-bit PCM converted from float with saturation.
- FLAC: `FLAC__stream_encoder_init_ogg_stream` with write callback appending to the output vector, level 5, 16 bit, serial number random; `flush` finishes the stream; a new `open` after reconnect starts a new logical stream.
- [ ] RED tests (each codec skipped with a doctest MESSAGE when its library is unavailable):
  - FLAC 48 k and 44.1 k: encode 3 s of stereo 16-bit-representable noise, decode with libFLAC Ogg decoder → **bit-exact**; output starts with `OggS`.
  - MP3 128 kb/s @ 44.1 k: frames sync `0xFFE`, CBR frame sizes consistent with the bitrate, decoded (LAME `hip_decode`) 1 kHz sine RMS within 1 dB and correlation > 0.99 after aligning by the decoder delay.
  - AAC-LC 128, HE-AAC 64, HE-AACv2 32 @ 48 k: ADTS sync `0xFFF`, profile/rate/channel fields correct, decoded with `aacDecoder_*` → sine level within 1.5 dB, correlation > 0.95 (HEv2: compare the mono sum, since PS is parametric).
  - `codec_limits` matches spec §5.2; `parse_codec` round trip; unsupported rate/bitrate rejected by `open` with a Polish error.
- [ ] Implement, tests green, MinGW 64 build clean; commit `encoder: MP3, AAC-LC/HE-AAC/HE-AACv2 and Ogg FLAC encoders`.

### Task 4: Delay line, station mixer, stream conditioning

**Files:** `encoder/include/bam/encoder/dsp.hpp`, `encoder/src/delay_line.cpp`, `encoder/src/station_mixer.cpp`, `encoder/src/stream_conditioner.cpp`, `tests/test_encoder_dsp.cpp`.

- `DelayLine(max_seconds = 60.5)`: `write(const float* stereo, frames)`, `read(float* out, frames)` = samples written `delay_frames` ago; `set_delay_seconds(s)` with a 240-frame fade-out/fade-in around the jump (no step larger than the ramp bound); growing the delay inserts silence of the difference; capacity check.
- `StationMixer`: up to 3 inputs, per-input linear gain with 240-frame ramps (gain change, mute, enable/disable), float sum, peaks per input and for the mix.
- `StreamConditioner(out_rate)`: gain ramp + fixed-ratio resampling 48000 → out_rate (reuse `DriftResampler` with multiplier 1.0, or libsamplerate directly), output float stereo; saturation is left to the encoder's PCM conversion.
- [ ] RED tests: delay exact to the sample for 0, 0.1, 2.5, 60 s; change 1 s → 3 s mid-stream: no step > full scale/240 + 1 LSB, then exact 3 s alignment; mixer sum and ramp bounds; conditioner: 1 kHz sine level kept within 0.1 dB for each output rate, output length = input × ratio ± 2 frames.
- [ ] Commit `encoder: delay line, station mixer, stream gain and resampling`.

### Task 5: Icecast client

**Files:** `encoder/include/bam/encoder/icecast.hpp`, `encoder/src/icecast_client.cpp`, `tests/test_icecast.cpp` (with a fake server built on `TcpListener`).

Interface:
```cpp
struct IcecastTarget { std::string host; uint16_t port; std::string mount, user, password, station_name, content_type; };
enum class IcecastState { Idle, Connecting, Streaming, Error };
struct IcecastStatus { IcecastState state; std::string error; uint64_t bytes_sent, reconnects, dropped_bytes; double kbps; };
class IcecastClient {   // own thread; never blocks the caller
 public:
  explicit IcecastClient(IcecastTarget t, size_t queue_limit_bytes);
  void push(std::span<const uint8_t> data);  // queue; on overflow drop oldest, count dropped_bytes
  void on_reconnect(std::function<void()> cb);  // e.g. encoder restarts the stream (new Ogg/FLAC header)
  IcecastStatus status() const; void stop();
};
std::vector<int> icecast_retry_schedule_ms();  // 1000, 2000, 5000, 10000 then 10000
```
- [ ] RED tests with the fake server on 127.0.0.1: correct `PUT` request line, `Authorization: Basic <base64(user:pass)>`, `Content-Type`, `Ice-Name`; 100-continue handled; body bytes arrive in order; 401 → state Error "złe hasło lub użytkownik", 403 → "mountpoint zajęty lub zabroniony", closed port → "serwer nieosiągalny"; 405 → fallback `SOURCE /mount ICE/1.0` accepted; server drops the connection → client reconnects following the schedule (use a test hook to shorten delays) and `on_reconnect` fires; queue overflow drops oldest and counts.
- [ ] Commit `encoder: Icecast source client with PUT/SOURCE and reconnect`.

### Task 6: Encoder configuration

**Files:** `encoder/include/bam/encoder/config.hpp`, `encoder/src/config.cpp`, `tests/test_encoder_config.cpp`.

- Structs per spec §7 (`EncoderConfig`, `StationConfig`, `InputConfig`, `StreamConfig`), JSON round trip, `validate()` (limits, codec table, mount starts with `/`, port 1–65535, host non-empty for enabled streams, SDP parses via `parse_sdp` for enabled inputs, station ids unique), `normalize()` (0.5 dB / 0.1 s steps, latency ≥ ptime + 1 ms like BounceAES), `mask_passwords()` for the API and `merge_passwords(previous)` (empty = keep).
- [ ] RED tests for each rule and for password masking/merging; commit `encoder: configuration, validation and password masking`.

### Task 7: Encoder engine (inputs, PTP, stations, streams)

**Files:** `encoder/include/bam/encoder/engine.hpp`, `encoder/src/engine.cpp`, `encoder/src/inputs.cpp` (RX sockets + `RxStream` + rings in process memory), `encoder/src/ptp_client.cpp` (slave-only loop on `PtpPort`, modelled on `engine/src/ptp_worker.cpp`), `encoder/src/station.cpp`, `encoder/src/stream_worker.cpp`, `tests/test_encoder_engine.cpp`.

- Each enabled input: one `UdpSocket` on the SDP port, joined to its group (source filter when present), datagrams dispatched only when the destination group (and sender) match; `RxStream` writes into an in-process ring pair (`ChannelRing` over owned buffers, 65536 frames) on the media clock.
- PTP client: slave only, domain from config; without a master the media clock free-runs on the local clock; status (state, master id, offset) for the API.
- Station thread every 10 ms: reads each input ring up to `now − latency`, feeds the delay lines, mixes, hands the mix to each stream worker's queue (≤ 5 s, drop oldest + count).
- Stream worker thread: conditioner → encoder → `IcecastClient::push`; on reconnect the encoder is reopened (new header/stream).
- `EncoderEngine`: `start/stop`, `apply_config` (rebuild only what changed: an edited stream does not restart other streams or the station), `status()` (inputs: signal, peaks, rx counters; station peak; streams: Icecast status, encoder errors).
- [ ] RED end-to-end test (127.0.0.1 only): a test AES67 sender (use `TxStream` from core or hand-built RTP L24 packets) → input with SDP → delay 2 s → second input at −6 dB → FLAC 48 k stream → fake Icecast → decode → the decoded audio equals the expected mix (16-bit, after the 2 s delay) bit for bit over a 1 s window; status shows Streaming and bytes. Second test: editing one stream's bitrate leaves the other stream's connection untouched (same fake-server connection id).
- [ ] Commit `encoder: engine with AES67 inputs, PTP slave, stations and stream workers`.

### Task 8: HTTP API, service and tray

**Files:** `encoder/src/http_server.cpp` (+ header), `encoder_service/main.cpp`, `service/win_service.hpp/.cpp` (service name parameter, keep BounceAES default), `tray/tray_main.cpp` (`--name`, `--port`, `--panel-url` arguments with BounceAES defaults) or a separate target `BounceEncoderTray`, `CMakeLists.txt` targets `BounceEncoder` (+ rc with version 1.0.0), `tests/test_encoder_http.cpp`.

- API per spec §7 with session login copied from the BounceAES pattern (`core/auth`), `GET /api/codecs` (availability + limits + bitrate hints), panel assets embedded like `cmake/embed_files.cmake` (separate asset set for `encoder/panel/`).
- Service: Windows service `BounceEncoder` ("Bounce Encoder"), console mode `--console --config`, logs in `%ProgramData%\Bounce Encoder\logs`.
- [ ] Tests: login, config get/put with masked passwords, station add/delete, codecs endpoint, 401 without session; commit `encoder: HTTP API, service and tray`.

### Task 9: Panel

**Files:** `encoder/panel/index.html`, `app.js`, `style.css`, logo assets (copy the BounceAES panel logo, caption "Encoder").

- Tabs: Stacje (list, meters, stream states, add/delete/enable), edit station (inputs: SDP from file upload or paste, gain, delay; streams: codec/bitrate/rate/gain/host/port/mount/user/password, bitrate hint with the MP3 equivalent table from spec §8, unavailable codecs greyed with the reason), Statystyki, Logi. Same dark minimal style as BounceAES Master.
- [ ] Browser check with playwright-cli (chromium), service in console mode with a config asserted on 127.0.0.1 (`interface_ip` and `http_bind`), a fake Icecast on 127.0.0.1 and a test AES67 sender on 127.0.0.1; screenshots for the guide; stop everything afterwards. Commit `encoder: web panel`.

### Task 10: Installer, docs, CI, release

**Files:** `installer/nsis/BounceEncoder-setup.nsi`, `docs/ENCODER-INSTALL.md`, `docs/ENCODER-TESTING.md`, `README.md` (section), `.github/workflows/build.yml` (build codecs on MSVC, encoder artifacts and installer), version 1.0.0 in the encoder rc/constants.

- Installer: service + autostart, tray autostart, desktop/start-menu shortcut "Bounce Encoder", firewall rule (AES67 RTP inbound, PTP 319/320, panel 8070), the codec DLLs next to the exe, uninstall.
- Docs: installation, stations, inputs (SDP), delay, streams, codec/bitrate guide (AAC↔MP3 table), Icecast settings and error messages, licence note on fdk-aac (internal use), troubleshooting; TESTING with the manual checks (real Icecast, playback in VLC/browser, 24 h).
- CI: Windows job builds `BounceEncoder` with codecs, runs host tests, builds the installer, uploads artifacts; Linux job builds and runs the encoder tests.
- [ ] Commit `Bounce Encoder 1.0.0: installer, docs and CI`.
