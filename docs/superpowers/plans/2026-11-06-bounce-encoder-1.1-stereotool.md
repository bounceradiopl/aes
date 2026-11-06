# Bounce Encoder 1.1 — Stereo Tool processing: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** optional per-station Stereo Tool processing in Bounce Encoder (libStereoTool loaded at run time), with per-station licence keys, `.sts` presets, the Stereo Tool web interface on 127.0.0.1, CPU/latency status and a bypass that never puts silence on air.

**Architecture:** a new `StereoToolLib` binding (run-time loaded function table, like the codecs) and a per-station `ProcessingWorker` thread between the station thread and the station's stream workers. The station pushes its mix to the worker (when processing is configured) instead of straight to the streams; the worker runs Stereo Tool (or an aligned bypass) and pushes to the streams. Config, engine, API and panel grow a `processing` block per station.

**Tech stack:** C++20, doctest, nlohmann/json, cpp-httplib, NSIS, GitHub Actions. Thimeo `libStereoTool` 11.07 (Windows x64 DLL bundled; macOS universal dylib available for local opt-in tests).

**Spec:** `docs/superpowers/specs/2026-11-06-bounce-encoder-stereotool-design.md`

## Global Constraints

- Pinned SDK: `https://download.thimeo.com/Stereo_Tool_Generic_plugin.zip`, zip SHA-256 `ed07c086cd95523abf5e7d655fa1f32e556fca2539ff0e85b4e12ff3a4c3ec22`; `libStereoTool_1107\lib\Windows\64\libStereoTool_64.dll` SHA-256 `48cbdc06f44a203f7fa2895b3ec79fc63557d61dcd687563c6a17388d6308c4e` (116 883 968 bytes); `include\libStereoTool.h` SHA-256 `b6581abf…1e31`; `include\ParameterEnum.h` SHA-256 `163386c9…214a`. The Thimeo headers are NOT committed: the binding declares only the function types it uses; parameter indices needed (e.g. `PARAM_WebserverEnable`, `PARAM_WebserverPort`) are derived at configure time from the downloaded `ParameterEnum.h` (enum position) or hard-coded for 11.07 with a CI check — implementer's choice, documented.
- Never silence because of processing: every failure path bypasses (5 ms ramps, aligned to the Stereo Tool latency).
- The licence key is a secret like stream passwords: never returned by the API, never logged; empty on save = keep.
- Stereo Tool web interface and UECP bind 127.0.0.1 only (`stereoTool_global_setBindIp` before the first instance).
- Library loaded only from the program directory (`set_codec_library_dirs`-style search, `LoadLibraryExW` restricted).
- Tests and demos only on 127.0.0.1; the real service only in console mode with a config asserted on 127.0.0.1 (interface_ip, http_bind); never a real LAN or a real Icecast; never run a real Stereo Tool licence key in tests.
- Polish user text, English comments; code style of the surrounding code. After each task: `cmake --build build -j8 && (cd build && timeout 900 ./bam_tests)` green; `build-win` and `build-win32` without warnings (32-bit: Stereo Tool off). Commits end with a blank line and `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Do not push.
- Version Bounce Encoder 1.1.0 (BounceAES Master unchanged at 1.2.4).

---

### Task 1: StereoToolLib binding + fake library

**Files:** create `encoder/include/bam/encoder/stereotool.hpp`, `encoder/src/stereotool.cpp`, `tests/fake_stereotool/fake_stereotool.cpp` (+ CMake target building it as a shared library named like the real one, placed where tests point the loader), `tests/test_stereotool.cpp`; modify `CMakeLists.txt` (option `BAM_WITH_STEREOTOOL`, default ON for 64-bit builds with codecs; SDK fetch with the pinned hashes, cache like the codecs; copy the DLL next to the encoder on Windows x64).

**Interfaces (produces):**
```cpp
namespace bam::encoder {
struct StereoToolInfo { bool available; std::string path; int version; std::string error; };
StereoToolInfo stereotool_info();                 // loads once (program dir only)
void set_stereotool_library_dirs(std::vector<std::string> dirs);  // tests
class StereoToolInstance {                        // one per processed station
 public:
  static std::unique_ptr<StereoToolInstance> create(const std::string& key, const std::string& name,
                                                    const std::string& home_dir, std::string* error);
  bool load_preset(const std::string& sts_path, std::string* error);
  bool save_preset(const std::string& sts_path, std::string* error);
  bool set_web_interface(bool enabled, int port);  // 127.0.0.1 only (global bind set before first create)
  void process(float* stereo, int32_t frames);     // in place, 48 kHz, 2 channels; SEH/exception guarded
  int latency_frames();                            // GetLatency2(48000, true)
  bool license_valid();
  std::string unlicensed_features();
  ~StereoToolInstance();                           // Delete
};
}
```
- [ ] RED tests against the fake library (exports `stereoTool_*` used by the binding): load from a test dir only (bare name never loaded), missing library → `available=false` with Polish error, create/delete counts, preset load/save round trip (fake writes/reads a small file), `process` applies the fake's gain and delay exactly, latency reported, licence valid/invalid modes, unlicensed-features text, exception thrown by the fake's `process` is caught and reported (instance marked failed), web interface call recorded with port and 127.0.0.1 bind. Optional local test (skipped unless `BAM_TEST_REAL_STEREOTOOL=<dir>`): real macOS dylib, no key: create, process silence, finite output, delete.
- [ ] Implement; Windows uses `LoadLibraryExW` with the restricted search like the codecs; SEH guard around `process` on MSVC (`__try`), plain try/catch elsewhere.
- [ ] Commit `encoder: Stereo Tool library binding and fake library for tests`.

### Task 2: ProcessingWorker

**Files:** create `encoder/src/processing_worker.cpp` (+ declarations in `encoder/src/engine_internal.hpp`), `tests/test_processing_worker.cpp`.

**Interfaces:** consumes T1. Produces:
```cpp
class ProcessingWorker {   // one per station with processing configured
 public:
  ProcessingWorker(const ProcessingSettings& s, std::string station_id);  // builds the instance off the station thread
  void push(const float* stereo, size_t frames);            // station thread, never waits
  void set_streams(std::vector<std::shared_ptr<StreamWorker>> streams);  // handover like a plan
  void restart();                                           // leave bypass and retry
  ProcessingStatus status() const;   // state off|running|bypass|error, reason (Polish), latency_ms, cpu_pct, license_valid, unlicensed_features
  void stop();
};
```
- [ ] RED tests with the fake: output = input × fake gain delayed by the fake latency, bit-exact; bypass is the input delayed by the same latency (aligned), switching processed↔bypass crossfades over 240 frames with no step above the ramp bound; each failure (no library, create fails, licence invalid, `process` throws, worker too slow → queue > 200 ms) bypasses with the right reason and never outputs silence while input has signal; `restart()` leaves bypass; CPU % = processing time / audio time measured per second; queue bounded, no allocation in the per-block path after construction.
- [ ] Implement; commit `encoder: per-station Stereo Tool worker with aligned bypass`.

### Task 3: Configuration

**Files:** `encoder/include/bam/encoder/config.hpp`, `encoder/src/config.cpp`, `tests/test_encoder_config.cpp`.
- [ ] `StationConfig::processing { bool enabled; std::string license_key; std::string preset; }`, `EncoderConfig::stereotool_default_key`, `EncoderConfig::stereotool_web_port_base` (default 8100). JSON round trip; `config_to_api_json` returns `license_key_set` / `default_key_set` never the keys; `merge_passwords` keeps empty keys (no host rule); validation: preset name charset (`[A-Za-z0-9 _.-]`, ends `.sts`, ≤ 64 chars), processing enabled needs a preset and a key (station or default); warnings (non-blocking, returned separately): the same effective key on two enabled stations with processing. Polish messages naming the station. RED first; commit `encoder: processing settings, keys kept secret`.

### Task 4: Engine wiring, presets store, status

**Files:** `encoder/src/engine.cpp`, `encoder/src/station.cpp`, `encoder/include/bam/encoder/engine.hpp`, new `encoder/src/preset_store.cpp` (+ header), `tests/test_encoder_engine.cpp`.
- [ ] Presets in `%ProgramData%\Bounce Encoder\stereotool\presets\` (data_dir, admin-only ACL like config; created with the protected SDDL as in data_dir.cpp), per-station home `…\stereotool\<station id>\`. Store API: list, put (sanitised name, ≤ 4 MB, ≤ 64 files), get, remove (refuse if used).
- [ ] apply_config: station with processing enabled → build/keep a ProcessingWorker (rebuild only when key or preset changes; preset change → load_preset with crossfade), plan routes the station mix to it; enabling/disabling processing: fade-out/fade-in 5 ms like a delay change; stations without processing unchanged (bit-exact path kept).
- [ ] Web interface port = base + station index (stable per station id order), enabled only while processing runs; save-preset and restart operations.
- [ ] Status: per station `processing` block (T2 status + web port); engine-wide `stereotool` info (T1).
- [ ] E2E test with the fake library: AES67 → station with processing (fake gain −6 dB, latency 512) → FLAC 48 k → fake Icecast, decoded bit-exact = expected; toggling processing never yields silence; second station without processing still bit-exact. RED first; commit `encoder: stations with Stereo Tool processing`.

### Task 5: HTTP API

**Files:** `encoder/src/http_server.cpp`, `tests/test_encoder_http.cpp`.
- [ ] `GET /api/stereotool`; `POST /api/stereotool/presets` (`{name, data_base64}`, this route alone allows a body up to 6 MB — keep the 1 MB cap for every other route and keep all E11 guarantees), `GET/DELETE /api/stereotool/presets/<name>`; `POST /api/stations/<id>/processing/save-preset` (`{name}`), `POST /api/stations/<id>/processing/restart`; status fields; config API with `license_key_set` / `default_key_set` and warnings. All behind the one session guard, JSON only. Tests incl. key never in any response, name sanitising, size limits, smuggling vectors still closed for the bigger route. Commit `encoder: Stereo Tool API`.

### Task 6: Panel

**Files:** `encoder/panel/index.html`, `app.js`, `logic.js`, `style.css` (shared `panel-common` unchanged unless a generic component is missing), `tests/panel/encoder_panel_logic.test.js`.
- [ ] Station editor: "Processing" section (switch, key field "zapisany (puste = bez zmian)", preset select, "Wgraj preset…", "Otwórz Stereo Tool" → `http://127.0.0.1:<port>/` in a new tab with a note that it works on this computer only, "Zapisz jako preset…", "Uruchom ponownie processing", state + CPU % + latency, licence warning with the unlicensed features). Settings dialog: default key, web port base. Station list: processing mark (state, CPU %). Presets dialog (list, upload, download, delete). Same look as 1.2.3/1.0.3.
- [ ] Browser check under the safety rule with the fake library (service console mode, config asserted on 127.0.0.1); screenshots; commit `encoder: panel for Stereo Tool processing`.

### Task 7: Packaging, CI, docs, release prep

**Files:** `.github/workflows/build.yml`, `installer/nsis/BounceEncoder-setup.nsi`, `docs/ENCODER-INSTALL.md`, `docs/ENCODER-TESTING.md`, `LICENSING.md`, `README.md`, version files (encoder 1.1.0).
- [ ] CI: SDK download with the pinned hashes (cached), real-DLL smoke test on Windows x64 (`stereotool_info` available, create without key, process silence, delete), encoder artifacts and installer include `libStereoTool_64.dll` + Thimeo notice; 32-bit unchanged; Linux/macOS run the fake-library tests.
- [ ] Installer: DLL next to the encoder, removed on uninstall; size warning acceptable.
- [ ] Docs: "Processing (Stereo Tool)" chapter (licences per station, presets, web interface on 127.0.0.1 / remote desktop, CPU and RAM per instance — 1 core + ~600 MB per instance, keep CPU < 50 %, bypass reasons), TESTING rows (real key, listening, tuning, 24 h, CPU on the target PC), LICENSING (Thimeo commercial library, one licence per instance, redistribution outside Bounce needs Thimeo's consent, GPLv3 §7 permission extended to libStereoTool), README.
- [ ] Commit `Bounce Encoder 1.1.0: Stereo Tool processing — installer, docs and CI`.
