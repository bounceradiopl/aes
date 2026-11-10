# Bounce Encoder 1.2 — AES67 Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Bounce Encoder station can also send its finished signal (the one its Icecast streams get) as one AES67 multicast stream, timed on the network's PTP clock, announced by SAP.

**Architecture:** The thread that today hands the station signal to the stream workers (the station thread, or the Stereo Tool worker's output thread when processing runs) also writes it, tagged with its mix-timeline position, into the station's `Aes67Output` *content ring*. One encoder-wide sender thread (every 0.25 ms) copies content into each output's *TX ring* at `position + D` (a gate does the 5 ms fades when `D` changes or the clock steps) and pumps a `bam::TxStream` (bam_core, unchanged) to one UDP socket on the AoIP interface. A SAP worker announces enabled outputs and reports other devices announcing the same address.

**Tech Stack:** C++20, bam_core (`TxStream`, `ChannelRing`, `RxStream`, `generate_sdp`, `build_sap`, `SapDirectory`, `parse_ipv4`), bam_platform (`UdpSocket`), doctest, cpp-httplib, nlohmann::json, vanilla JS panel with `node --test` logic tests.

**Spec:** `docs/superpowers/specs/2026-11-10-bounce-encoder-aes67-output-design.md`

## Global Constraints

- Tests and CI send **only to 127.0.0.1** (`cfg.interface_ip = kLoopback`); no test may send to a real LAN. Never run the service against a real interface.
- The station signal path without an enabled AES67 output must stay bit for bit what it is in 1.1 (existing tests unchanged and green).
- Format fixed: L24, 48 kHz, 2 channels, payload type 98, RTP port `kRtpPort` (5004), TTL `kMulticastTtl` (15), SAP `kSapGroup:kSapPort` every 30 s.
- `ptime_us` ∈ {1000, 1333, 4000}, default 1000. Default address `239.69.110.<web_port_slot + 1>`; explicit address must be IPv4 in `239.0.0.0/8`.
- `D` (frames, mix position → RTP timestamp) = station `latency_frames` + 480 (station tick) + processing delay (`ProcessingWorker::delay_frames()`, 0 without processing) + 480 (margin).
- Output never sends before the PTP clock has locked once since start (unless the test-only option `aes67_require_ptp = false`); after a lock it keeps sending in holdover.
- The output never changes its address by itself; conflicts are warnings.
- All user-facing text in Polish, matching existing panel/log wording style; code comments in English, sparse, like the surrounding code.
- Encoder version 1.2.0; BounceAES Master untouched (no file under `engine/`, `asio/`, `bridge/`, `service/`, `tray/`, `panel/` changes).
- Commit after each task with the `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` trailer.

Build and run: `cmake --build build -j8 && ./build/bam_tests -tc="<pattern>"`; full suite `./build/bam_tests`; panel logic `node --test tests/panel/encoder_panel_logic.test.js`.

---

### Task 1: Configuration model, JSON and validation

**Files:**
- Modify: `encoder/include/bam/encoder/config.hpp` (new struct, `StationConfig` field, helpers)
- Modify: `encoder/src/config.cpp` (`to_json` station object ~282, `config_from_json` station ~389, `validate_config` ~415)
- Test: `tests/test_encoder_config.cpp`

**Interfaces:**
- Produces:
  ```cpp
  inline constexpr uint32_t kAes67PtimesUs[] = {1000, 1333, 4000};
  struct Aes67OutputConfig {
    bool enabled = false;
    std::string name;     // empty = the station name
    std::string address;  // empty = aes67_default_address(slot)
    uint32_t ptime_us = 1000;
  };
  // in StationConfig:  Aes67OutputConfig aes67_output;
  std::string aes67_default_address(int web_port_slot);   // "239.69.110.<slot+1>", "" for slot < 0
  std::string aes67_address(const StationConfig& st);     // explicit or default
  std::string aes67_name(const StationConfig& st);        // explicit or st.name
  ```

- [ ] **Step 1: Write the failing tests** (append to `tests/test_encoder_config.cpp`, reuse its helpers for a valid config; the snippet assumes a helper that returns a valid two-station config — use the file's existing one, e.g. the config built by its "valid configuration" test)

```cpp
TEST_CASE("encoder config: AES67 output defaults, JSON round trip and effective address") {
  be::EncoderConfig c = be::default_encoder_config();
  be::StationConfig st;
  st.name = "Radio Test";
  c.stations = {st};
  be::normalize_config(c);
  const be::StationConfig& s = c.stations[0];
  CHECK_FALSE(s.aes67_output.enabled);
  CHECK(s.aes67_output.ptime_us == 1000);
  CHECK(be::aes67_address(s) == "239.69.110." + std::to_string(s.processing.web_port_slot + 1));
  CHECK(be::aes67_name(s) == "Radio Test");

  c.stations[0].aes67_output = {true, "Nadajnik", "239.1.2.3", 1333};
  std::string err;
  const auto back = be::config_from_json(be::config_to_json(c), &err);
  REQUIRE_MESSAGE(back, err);
  const auto& o = back->stations[0].aes67_output;
  CHECK(o.enabled);
  CHECK(o.name == "Nadajnik");
  CHECK(o.address == "239.1.2.3");
  CHECK(o.ptime_us == 1333);
}

TEST_CASE("encoder config: a 1.1 file without aes67_output loads with the output off") {
  std::string err;
  const auto c = be::config_from_json(R"({"version":2,"stations":[{"id":"s1","name":"A"}]})", &err);
  REQUIRE_MESSAGE(c, err);
  CHECK_FALSE(c->stations[0].aes67_output.enabled);
  CHECK(c->stations[0].aes67_output.address.empty());
}

TEST_CASE("encoder config: AES67 output validation") {
  auto issues_for = [](be::EncoderConfig c) {
    be::normalize_config(c);
    std::string all;
    for (const auto& i : be::validate_config(c)) all += i.field + ": " + i.message + "\n";
    return all;
  };
  be::EncoderConfig c = be::default_encoder_config();
  be::StationConfig a; a.name = "A";
  be::StationConfig b; b.name = "B";
  c.stations = {a, b};

  auto bad = c;
  bad.stations[0].aes67_output = {true, "", "10.0.0.1", 1000};
  CHECK(issues_for(bad).find("stations[0].aes67_output.address") != std::string::npos);
  bad.stations[0].aes67_output.address = "239.1.1";
  CHECK(issues_for(bad).find("stations[0].aes67_output.address") != std::string::npos);

  bad = c;
  bad.stations[0].aes67_output = {true, "", "", 500};
  CHECK(issues_for(bad).find("stations[0].aes67_output.ptime_us") != std::string::npos);

  bad = c;  // the same address on two stations (one explicit, one default is enough to collide)
  bad.stations[0].aes67_output = {true, "", "239.5.5.5", 1000};
  bad.stations[1].aes67_output = {true, "", "239.5.5.5", 1000};
  CHECK(issues_for(bad).find("ten sam adres wyjścia AES67 co") != std::string::npos);

  bad = c;  // an input that would receive this encoder's own output
  bad.stations[0].aes67_output = {true, "", "239.5.5.6", 1000};
  be::InputConfig in;
  in.sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=x\r\nc=IN IP4 239.5.5.6/15\r\nt=0 0\r\n"
           "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:1\r\n";
  bad.stations[1].inputs = {in};
  CHECK(issues_for(bad).find("pętla") != std::string::npos);

  auto ok = c;  // an output that is off is not checked for duplicates or loops
  ok.stations[0].aes67_output = {false, "", "239.5.5.6", 1000};
  ok.stations[1].inputs = {in};
  CHECK(issues_for(ok).find("aes67") == std::string::npos);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build -j8 2>&1 | grep -m3 error; ./build/bam_tests -tc="encoder config: *AES67*,encoder config: a 1.1 file*"`
Expected: compile errors (`aes67_output` is not a member of `StationConfig`).

- [ ] **Step 3: Implement**

`config.hpp`, next to `ProcessingConfig`:
```cpp
// The station's AES67 output (Bounce Encoder 1.2): its finished signal, the one its streams get,
// as one L24 48 kHz stereo multicast stream on the PTP clock ("aes67_output" in the JSON).
inline constexpr uint32_t kAes67PtimesUs[] = {1000, 1333, 4000};
struct Aes67OutputConfig {
  bool enabled = false;
  std::string name;     // empty = the station name
  std::string address;  // empty = aes67_default_address(processing.web_port_slot)
  uint32_t ptime_us = 1000;
};
```
add `Aes67OutputConfig aes67_output;` as the last member of `StationConfig`, and declare the three helpers from **Interfaces**.

`config.cpp`:
- helpers:
```cpp
std::string aes67_default_address(int web_port_slot) {
  return web_port_slot < 0 ? std::string() : "239.69.110." + std::to_string(web_port_slot + 1);
}
std::string aes67_address(const StationConfig& st) {
  return st.aes67_output.address.empty() ? aes67_default_address(st.processing.web_port_slot) : st.aes67_output.address;
}
std::string aes67_name(const StationConfig& st) { return st.aes67_output.name.empty() ? st.name : st.aes67_output.name; }
```
- `to_json`, station object: add `{"aes67_output", {{"enabled", st.aes67_output.enabled}, {"name", st.aes67_output.name}, {"address", st.aes67_output.address}, {"ptime_us", st.aes67_output.ptime_us}}}` (same in API and file form: nothing secret).
- `config_from_json`, after the `processing` block:
```cpp
read_object(sj, "aes67_output", sp + ".aes67_output", [&](const json& oj) {
  const std::string op = sp + ".aes67_output";
  read_field(oj, "enabled", st.aes67_output.enabled, op);
  read_field(oj, "name", st.aes67_output.name, op);
  read_field(oj, "address", st.aes67_output.address, op);
  read_field(oj, "ptime_us", st.aes67_output.ptime_us, op);
});
```
  (match the exact `read_object` call shape used for `processing` at ~389; the path argument style must match.)
- `validate_config`: a per-station block (inside the station loop, with the file's `add`, `station_label`, `station_mention` helpers) plus one pass after the loop:
```cpp
// AES67 output (only checked while enabled)
const auto& ao = st.aes67_output;
const std::string of = station_path(i) + ".aes67_output.";
if (ao.enabled) {
  const auto ip = parse_ipv4(ao.address);
  if (!ao.address.empty() && (!ip || (*ip)[0] != 239))
    add(of + "address", label + "adres wyjścia AES67 musi być adresem multicast 239.x.x.x");
  if (std::find(std::begin(kAes67PtimesUs), std::end(kAes67PtimesUs), ao.ptime_us) == std::end(kAes67PtimesUs))
    add(of + "ptime_us", label + "czas pakietu wyjścia AES67 musi wynosić 1, 1,333 albo 4 ms");
  const std::string addr = aes67_address(st);
  if (!addr.empty()) {
    auto [it, inserted] = aes67_addresses.emplace(addr, i);
    if (!inserted)
      add(of + "address", label + "ten sam adres wyjścia AES67 co " + station_mention(c.stations[it->second], it->second));
  }
}
```
  where `std::map<std::string, size_t> aes67_addresses;` is declared next to `targets` (~435). After the station loop:
```cpp
// an input receiving one of this encoder's own AES67 outputs would feed it back into itself
for (size_t i = 0; i < c.stations.size(); ++i)
  for (size_t k = 0; k < c.stations[i].inputs.size(); ++k) {
    const auto r = parse_sdp(c.stations[i].inputs[k].sdp);
    if (!r.stream) continue;
    const auto own = aes67_addresses.find(r.stream->multicast_address);
    if (own == aes67_addresses.end()) continue;
    add(input_path(i, k) + ".sdp", station_label(c.stations[i], i) + "wejście " + std::to_string(k + 1) +
        ": to wyjście AES67 tego enkodera (" + station_mention(c.stations[own->second], own->second) + "): pętla");
  }
```
  (Use the file's real helper names/signatures for `station_path`, `input_path`, `station_label`, `station_mention`; adapt the argument lists if they differ. `#include "bam/ipv4.hpp"`.)

- [ ] **Step 4: Run the tests**

Run: `cmake --build build -j8 && ./build/bam_tests -tc="encoder config*"`
Expected: all pass, including the existing encoder config tests.

- [ ] **Step 5: Commit**

```bash
git add encoder/include/bam/encoder/config.hpp encoder/src/config.cpp tests/test_encoder_config.cpp
git commit -m "encoder: AES67 output in the station configuration, validation and JSON"
```

---

### Task 2: `Aes67Output` — content ring, TX ring, gate and TxStream

**Files:**
- Create: `encoder/src/aes67_output.hpp`, `encoder/src/aes67_output.cpp`
- Test: `tests/test_aes67_output.cpp`

**Interfaces:**
- Consumes: `bam::ChannelRing`, `bam::TxStream`, `bam::TxConfig`, `bam::PacketSink`, `bam::ptime_us_to_frames`, `kRingFrames`, `kBlocksPerRing`, `kRampFrames` (240, `dsp.hpp`), `kStationRate`.
- Produces (namespace `bam::encoder::detail`):
  ```cpp
  struct Aes67Stats { uint64_t packets = 0, gaps = 0, send_errors = 0; };
  class Aes67Output {
   public:
    Aes67Output(uint32_t ptime_us, uint32_t ssrc);
    // Writer side (the one thread that hands the station signal to its streams): stereo float,
    // interleaved; `pos` is the mix-timeline position of the first frame's content.
    void write(const float* stereo, size_t frames, uint64_t pos);
    // Any thread: the delay D (mix position -> RTP timestamp). A change fades out, waits until the
    // new mapping reaches content not sent yet, fades in.
    void set_delay(uint32_t frames);
    uint32_t delay() const;
    // Sender thread only: fill the TX ring up to now + one packet and send the due packets.
    // `generation` is the clock's step generation: a change re-aligns (fade-in, TxStream::reset).
    void pump(uint64_t now, uint64_t generation, const PacketSink& send);
    void count_send_error();
    Aes67Stats stats() const;
    uint32_t frames_per_packet() const;
  };
  ```

Design of `pump` (the gate), with `D` the current delay and `out_` the next TX-ring position to fill:
- First call or new `generation`: `out_ = now`, gate = `FadeIn` from 0, `tx_->reset()`.
- Fill `[out_, now + frames_per_packet)`: for each output position `t`, source position `p = t - D`. Gate states:
  - `Run`: copy content `p` (gain 1).
  - `FadeOut` (entered by `set_delay` with `pending_delay_`): keeps the **old** D for `kRampFrames` frames with gain ramping 1→0; then switch to the pending D and enter `Hold`.
  - `Hold`: write silence while `p < sent_end_` (content already sent: no repeat) ; then `FadeIn`.
  - `FadeIn`: copy with gain 0→1 over `kRampFrames`, then `Run`.
  - `sent_end_` = highest source position copied so far + 1.
- A content block whose lap tag does not match (never written / stale) reads as silence — count it: if the whole packet-worth of content read for a TX packet was unwritten, `gaps++`. Detect "unwritten" by also keeping a per-block written mask? Simpler: the writer keeps `written_end_` (atomic, highest `pos + frames`); a source position `p >= written_end_` or `p + kRingFrames < written_end_` is a gap (silence written, counted once per packet in which it occurs).
- Float → int32 left-aligned with clipping: `int32_t v = f >= 1.0f ? INT32_MAX : f <= -1.0f ? INT32_MIN : int32_t(double(f) * 2147483648.0)`; this is the exact inverse of the station's `kInt32Scale` for every value the station produces, so the path is bit exact at gain 1.
- TX ring: own `ChannelRing` pair, written only by `pump`; `TxStream` reads it with `source_delay = 0`.

- [ ] **Step 1: Write the failing tests** (`tests/test_aes67_output.cpp`)

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "../encoder/src/aes67_output.hpp"
#include "bam/rtp.hpp"

namespace {
namespace bd = bam::encoder::detail;

constexpr float kScale = 1.0f / 2147483648.0f;
float sample(uint64_t pos, int ch) { return float(int32_t((pos * 2654435761u + uint64_t(ch) * 977) & 0x7fffff00u) - 0x40000000) * kScale; }

struct Captured { uint32_t timestamp; std::vector<int32_t> left, right; };

// Decodes the L24 payload of every packet (RTP header 12 bytes, then L R L R, 3 bytes each).
std::vector<Captured> run(bd::Aes67Output& out, uint64_t from, uint64_t to, uint64_t gen = 1) {
  std::vector<Captured> got;
  for (uint64_t now = from; now <= to; now += 12) {
    out.pump(now, gen, [&](std::span<const uint8_t> p) {
      Captured c;
      c.timestamp = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
      for (size_t i = 12; i + 6 <= p.size(); i += 6) {
        auto s24 = [&](size_t o) { return int32_t(uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8); };
        c.left.push_back(s24(i));
        c.right.push_back(s24(i + 3));
      }
      got.push_back(std::move(c));
    });
  }
  return got;
}

void feed(bd::Aes67Output& out, uint64_t from, uint64_t to) {
  std::vector<float> buf;
  for (uint64_t p = from; p < to; p += 480) {
    const size_t n = size_t(std::min<uint64_t>(480, to - p));
    buf.resize(n * 2);
    for (size_t f = 0; f < n; ++f) { buf[f * 2] = sample(p + f, 0); buf[f * 2 + 1] = sample(p + f, 1); }
    out.write(buf.data(), n, p);
  }
}
}  // namespace

TEST_CASE("aes67 output: packet T carries the content of mix position T - D, bit for bit") {
  bd::Aes67Output out(1000, 0x1234);
  out.set_delay(2000);
  feed(out, 100000, 120000);
  const auto pkts = run(out, 100000 + 2000 + 600, 100000 + 2000 + 9000);  // after the first fade-in
  REQUIRE(pkts.size() > 100);
  for (const auto& c : pkts) {
    if (c.timestamp < 100000 + 2000 + 600 + 240 + 48) continue;  // the start fades in over 240 frames
    for (size_t f = 0; f < c.left.size(); ++f) {
      const uint64_t src = uint64_t(c.timestamp) + f - 2000;
      CHECK(c.left[f] == (int32_t(double(sample(src, 0)) * 2147483648.0) & int32_t(0xffffff00)));
      CHECK(c.right[f] == (int32_t(double(sample(src, 1)) * 2147483648.0) & int32_t(0xffffff00)));
    }
  }
  CHECK(out.stats().gaps == 0);
}

TEST_CASE("aes67 output: a longer delay never repeats content; it fades out, holds silence, fades in") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 48000);
  run(out, 2000, 20000);
  out.set_delay(1000 + 4320);  // processing switched on: about 90 ms more
  const auto pkts = run(out, 20012, 40000);
  // no output position after the change carries a source position that was already sent
  bool silent_seen = false;
  for (const auto& c : pkts)
    for (size_t f = 0; f < c.left.size(); ++f) silent_seen |= c.left[f] == 0 && c.right[f] == 0;
  CHECK(silent_seen);
  // and from 240 frames after the hold ends the content is T - 5320 again
  const auto& last = pkts.back();
  const uint64_t src = uint64_t(last.timestamp) - 5320;
  CHECK(last.left[0] == (int32_t(double(sample(src, 0)) * 2147483648.0) & int32_t(0xffffff00)));
}

TEST_CASE("aes67 output: content that never arrived is silence and counts as a gap") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 10000);  // nothing after 10000
  const auto pkts = run(out, 1000, 16000);
  CHECK(out.stats().gaps > 0);
  CHECK(pkts.back().left[0] == 0);
  CHECK(out.stats().packets == pkts.size());
}

TEST_CASE("aes67 output: a clock step re-aligns without a burst of packets") {
  bd::Aes67Output out(1000, 1);
  out.set_delay(1000);
  feed(out, 0, 20000);
  run(out, 1000, 10000, 1);
  const size_t before = out.stats().packets;
  run(out, 500000, 500048, 2);  // the clock jumped far ahead
  CHECK(out.stats().packets - before <= 2);
}
```

(`bam/rtp.hpp` is only needed if you prefer its parser over the manual decoding above; drop the include if unused.)

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: `aes67_output.hpp: No such file or directory`.

- [ ] **Step 3: Implement** `encoder/src/aes67_output.hpp/.cpp` per **Interfaces** and the gate design above. Members: content rings (`std::array<std::vector<int32_t>,2>` samples sized `kRingFrames`, tags sized `kBlocksPerRing`, `ChannelRing` views — same allocation as `InputRx` in `encoder/src/inputs.cpp:94-98`), TX rings (same), `std::unique_ptr<TxStream> tx_` built with `TxConfig{frames_per_packet = ptime_us_to_frames(ptime_us), payload_type = 98, ssrc, gain = 1.0, source_delay = 0}`, `std::atomic<uint32_t> delay_, pending_delay_` (pending flag), `std::atomic<uint64_t> written_end_`, gate state (sender thread only), `std::atomic<uint64_t> gaps_`. `stats()` = `{tx_->stats().packets, gaps_, tx_->stats().errors}`. File header comment: two short lines on what it is and which threads touch what.

- [ ] **Step 4: Run the tests**

Run: `cmake --build build -j8 && ./build/bam_tests -tc="aes67 output*"`
Expected: 4 test cases pass.

- [ ] **Step 5: Commit**

```bash
git add encoder/src/aes67_output.hpp encoder/src/aes67_output.cpp tests/test_aes67_output.cpp
git commit -m "encoder: Aes67Output, the station signal on the AES67 timeline with gated delay changes"
```

---

### Task 3: Positions through the station and the processing worker

**Files:**
- Modify: `encoder/src/engine_internal.hpp` (`StationPlan`, `ProcessingWorker`)
- Modify: `encoder/src/station.cpp` (`Station::process` ~237)
- Modify: `encoder/src/processing_worker.cpp` (`push`, the output thread's emit, `deliver` ~826)
- Test: `tests/test_processing_worker.cpp`, `tests/test_encoder_engine.cpp`

**Interfaces:**
- Consumes: `Aes67Output::write(const float*, size_t, uint64_t pos)` (Task 2).
- Produces:
  ```cpp
  // StationPlan
  std::shared_ptr<Aes67Output> aes67;  // null: no output (the 1.1 path)
  // ProcessingWorker
  void push(const float* stereo, size_t frames, uint64_t pos);           // pos of the first frame
  void set_aes67(std::shared_ptr<Aes67Output> out);                       // any thread; null removes
  // ProcessingSettings::output_tap becomes
  std::function<void(const float*, size_t, uint64_t pos)> output_tap;
  ```
  Contract: the worker's output frame j carries the content of input frame j (processed or bypassed; it is only *emitted* later, by `delay_frames()` — the existing `analyse()` in `tests/test_processing_worker.cpp` compares `out[j]` with `in[j]`), so every delivered frame carries the `pos` its input frame was pushed with. A `pos` that is not the previous `pos + frames` (the station repositioned after a clock step) starts a new base from that frame on.

- [ ] **Step 1: Write the failing tests**

In `tests/test_processing_worker.cpp`:
1. Extend the fixtures: `Collector` also stores positions (reserve like `out`, never reallocate) and its `tap()` returns the three-argument lambda; add `std::vector<uint64_t> positions_copy()`. `feed()` pushes with `w.push(input.in.data() + from * 2, n, kFeedBase + from)` where `constexpr uint64_t kFeedBase = 7'000'000;`. Update the other direct `push`/`output_tap` uses in the file (e.g. ~894) to the new signatures.
2. Add:
```cpp
TEST_CASE("processing: delivered frames carry the positions their input frames were pushed with") {
  UseFake use;
  fake().set("gain", 0.5);
  fake().set("latency", 512);
  Collector col;
  Input input;
  PW w(settings(col), "s1");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  feed(w, input, 48000);
  REQUIRE(drained(w, col, input));
  const auto pos = col.positions_copy();
  REQUIRE(pos.size() == col.frames());
  for (size_t j = 0; j < pos.size(); ++j) REQUIRE(pos[j] == kFeedBase + j);
}

TEST_CASE("processing: a jump in the pushed position starts a new base from that frame") {
  UseFake use;
  Collector col;
  Input input;
  PW w(settings(col), "s1");
  REQUIRE(feed_until(w, input, be::ProcessingState::Running));
  const size_t before = input.frames();
  constexpr uint64_t kJump = 900'000'000;
  input.extend(4800);
  for (size_t k = 0; k < 4800; k += 480) w.push(input.in.data() + (before + k) * 2, 480, kJump + k);
  feed(w, input, 9600);  // contiguous again from kFeedBase + frames: a second jump (back)
  REQUIRE(drained(w, col, input));
  const auto pos = col.positions_copy();
  for (size_t j = 0; j < 4800; ++j) CHECK(pos[before + j] == kJump + j);
  CHECK(pos[before + 4800] == kFeedBase + before + 4800);
}
```

In `tests/test_encoder_engine.cpp`: no new test yet (Task 4 covers the end to end).

- [ ] **Step 2: Run to see it fail**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: compile error on the three-argument `output_tap`.

- [ ] **Step 3: Implement**
- `Station::process`: replace the hand-off with
```cpp
if (plan_->processing) {
  plan_->processing->push(mix_.data(), frames, from);  // a copy into its queue: never waits on Stereo Tool
} else {
  for (const auto& stream : plan_->streams) stream->push(mix_.data(), frames);
  if (plan_->aes67) plan_->aes67->write(mix_.data(), frames, from);
}
```
- `ProcessingWorker`: carry the position alongside the queued audio: the input counter `written_` already numbers input frames, so keep a small fixed-size array of `(first input frame index, pos)` marks written by `push` whenever `pos` is not contiguous (no allocation on the audio threads; the existing "no allocation per block" test must stay green); in the output thread the position of emitted frame j is `mark.pos + (j - mark.index)` for the last mark at or before j; `deliver(stereo, frames, pos)` calls `output_tap_(stereo, frames, pos)`, the streams as today, and `aes67->write(stereo, frames, pos)` where `aes67` is read from an atomic `std::shared_ptr` set by `set_aes67` (`std::atomic<std::shared_ptr<Aes67Output>>` or the file's existing pattern for `set_streams`).
- Update every existing `output_tap` lambda in the tests to the three-argument form (ignore `pos` where not needed).

- [ ] **Step 4: Run the tests**

Run: `cmake --build build -j8 && ./build/bam_tests -tc="processing worker*,stereotool*,encoder engine*"`
Expected: all pass (the new one included).

- [ ] **Step 5: Commit**

```bash
git add encoder/src/engine_internal.hpp encoder/src/station.cpp encoder/src/processing_worker.cpp tests/test_processing_worker.cpp tests/test_encoder_engine.cpp
git commit -m "encoder: the station signal carries its mix position to the AES67 output, also through Stereo Tool"
```

---

### Task 4: Engine wiring — sender thread, socket, D, PTP gating, status

**Files:**
- Create: `encoder/src/aes67_sender.hpp`, `encoder/src/aes67_sender.cpp`
- Modify: `encoder/include/bam/encoder/engine.hpp` (options, status types)
- Modify: `encoder/src/engine.cpp` (`StationRuntime`, `apply`, `publish`, `housekeeping`, `station_status`, `start`/`stop`/`shutdown`)
- Test: `tests/test_encoder_engine.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  ```cpp
  // engine.hpp
  struct EncoderEngineOptions { ...; bool aes67_require_ptp = true; bool enable_sap = true; };  // tests: false / false
  enum class Aes67State { Off, WaitingForPtp, Sending, Holdover, Error };
  struct Aes67Status {
    Aes67State state = Aes67State::Off;
    std::string reason;          // Polish, empty when Sending
    std::string address, name;
    uint32_t ptime_us = 1000;
    double output_latency_ms = 0;  // D + the longest input delay of the station
    uint64_t packets = 0, gaps = 0, send_errors = 0;
    std::string conflict;        // Task 5: "<name> (<origin>)" of another announcer, else empty
  };
  // StationStatus: Aes67Status aes67;
  // EncoderEngine:
  std::optional<std::string> aes67_sdp(const std::string& station_id, std::string* error) const;  // Task 5 fills the SDP
  // detail::Aes67Sender (aes67_sender.hpp)
  class Aes67Sender {
   public:
    Aes67Sender(const ClockSource& clock);
    ~Aes67Sender();                                  // stop()
    void set_outputs(std::vector<std::pair<std::string, std::shared_ptr<Aes67Output>>> outputs_with_address);
    void set_interface(const std::string& iface_ip); // (re)sets multicast out on the socket
    void set_allowed(bool send);                     // false: no packets (waiting for PTP)
    std::string error() const;                       // socket error, Polish, else empty
    void stop();
  };
  ```

Rules:
- `StationRuntime` gets `std::shared_ptr<Aes67Output> aes67` and `uint32_t aes67_ssrc` (random once per station runtime, like BounceAES `tx_ssrc`). Created in `apply` when `cfg.aes67_output.enabled`; rebuilt only when `ptime_us` changes; removed (and the plan republished without it) when disabled. Address/name changes keep the object.
- `publish(rt)`: `plan->aes67 = rt.aes67` and `rt.proc.worker->set_aes67(rt.aes67)` when processing runs (null otherwise).
- `D` = `plan->latency_frames + 480 + (rt.proc.worker ? rt.proc.worker->delay_frames() : 0) + 480`; set in `publish` and re-checked in `housekeeping()` (every 100 ms) so the Stereo Tool delay growing after start moves `D`.
- Sender thread: `UdpSocket::open(0)`, `set_multicast_out(iface, kMulticastTtl, true)`; loop every 250 µs at "Pro Audio" priority, like BounceAES `tx_loop` (`engine/src/stream_workers.cpp:73-118`): `now = clock.get().sample_at(monotonic_ns())`, `generation = clock.step_generation()`, for each output `out->pump(now, generation, send)` where `send` retries `send_to(p, address, kRtpPort)` up to 3 times 20 µs apart and else `out->count_send_error()`. When not allowed: no pump at all (and nothing queued).
- PTP gating in `housekeeping()`: `ever_locked_ |= ptp && ptp->status().state == PtpState::Locked`; `sender->set_allowed(ever_locked_ || !opts.aes67_require_ptp)`; per-station state: `Off` (disabled), `Error` (sender socket error → reason), `WaitingForPtp` (not allowed; reason „czeka na zegar PTP” or „PTP wyłączony — wyjście AES67 wymaga zegara PTP” when `!opts.enable_ptp`), `Holdover` (allowed, PTP not Locked now; reason „zegar podtrzymany (brak mastera PTP)”), else `Sending`. Log a warning once per transition into `Holdover` and an info line on return.
- Interface change (`check_interface`/`apply`): `sender->set_interface(iface)`.
- `stop()/shutdown()`: stop the sender before stations are torn down.

- [ ] **Step 1: Write the failing tests** (`tests/test_encoder_engine.cpp`; add a loopback receiver helper next to `AesSender`)

```cpp
// Receives an AES67 stream on 127.0.0.1 into its own rings (the encoder's output under test).
struct AesReceiver {
  std::array<std::vector<int32_t>, 2> samples;
  std::array<std::vector<uint32_t>, 2> tags;
  std::array<bam::ChannelRing, 2> rings;
  std::optional<bp::UdpSocket> sock;
  std::unique_ptr<bam::RxStream> rx;
  std::thread thread;
  std::atomic<bool> run{true};
  const be::EncoderEngine& engine;
  AesReceiver(const be::EncoderEngine& e, const std::string& group, uint32_t latency_frames = 480) : engine(e) {
    for (int ch = 0; ch < 2; ++ch) {
      samples[ch].assign(bam::kRingFrames, 0);
      tags[ch].assign(bam::kBlocksPerRing, 0);
      rings[ch] = bam::ChannelRing(samples[ch].data(), tags[ch].data());
    }
    std::string err;
    sock = bp::UdpSocket::open(bam::kRtpPort, &err);
    REQUIRE_MESSAGE(sock, err);
    REQUIRE_MESSAGE(sock->join(group, kLoopback, "", &err), err);
    const auto sdp = bam::parse_sdp(make_sdp("rx", group, bam::kRtpPort));
    rx = std::make_unique<bam::RxStream>(*sdp.stream, rings[0], rings[1], latency_frames, 1.0);
    thread = std::thread([this] {
      std::vector<uint8_t> buf(2048);
      while (run) {
        int n;
        while ((n = sock->recv(buf, nullptr)) >= 0)
          rx->on_packet(std::span<const uint8_t>(buf.data(), size_t(n)), media_now(engine), bp::monotonic_ns());
        bp::sleep_ns(200'000);
      }
    });
  }
  ~AesReceiver() { run = false; thread.join(); }
  uint64_t packets() const { return rx->stats().packets; }
  int32_t at(uint64_t pos, int ch) const { int32_t v; rings[ch].read(pos, &v, 1); return v; }
};
```
(Check `RxStats` field names in `core/include/bam/rx_stream.hpp` and adapt `packets()`.)

Tests:
```cpp
TEST_CASE("encoder engine: a station's AES67 output carries its mix bit for bit at a constant delay") {
  // one input at 0 dB carrying sig_a, no processing; the input ring and the mix share the media
  // timeline (mix position p = input sample p), and packet T carries mix position T - D
  const std::string group = "239.69.221.1";
  constexpr uint16_t kInPort = 15221;
  be::EncoderConfig cfg = be::default_encoder_config();
  cfg.interface_ip = kLoopback;
  be::StationConfig st = station("s1", "Radio AES67");
  be::InputConfig a;
  a.sdp = make_sdp("Wejście", group, kInPort);
  a.latency_us = 20000;  // 960 frames: the station's latency_frames
  st.inputs = {a};
  st.aes67_output.enabled = true;
  cfg.stations = {st};
  cfg = ready(cfg);
  be::EncoderEngineOptions opts = no_ptp();
  opts.aes67_require_ptp = false;
  opts.enable_sap = false;
  be::EncoderEngine engine(opts);
  std::string err;
  REQUIRE_MESSAGE(engine.start(cfg, &err), err);
  AesSender in(engine, group, kInPort, sig_a);
  REQUIRE(wait_for([&] { return engine.status().stations[0].aes67.state == be::Aes67State::Sending; }, 5000));
  AesReceiver out(engine, engine.status().stations[0].aes67.address);
  REQUIRE(wait_for([&] { return out.packets() > 2000; }, 10000));
  const auto s = engine.status().stations[0].aes67;
  CHECK(s.gaps == 0);
  constexpr uint64_t kD = 960 + 480 + 0 + 480;  // latency + tick + no processing + margin
  CHECK(s.output_latency_ms == doctest::Approx(kD / 48.0));
  const uint64_t end = media_now(engine) - 4800;
  for (uint64_t t = end - 1000; t < end; ++t) {
    REQUIRE(out.at(t, 0) == sig_a(t - kD, 0));
    REQUIRE(out.at(t, 1) == sig_a(t - kD, 1));
  }
}
```
(The equality is exact: `sig_a` is exact in float and a single input at 0 dB mixes unchanged, as the existing FLAC end-to-end test relies on.)

Further test cases (each a `TEST_CASE`, same setup helpers):
- *"…the AES67 output follows Stereo Tool"*: `UseFake`, processing on with the fake (gain 0.5, latency 64): after `processing_running`, the output equals `0.5 ×` input at `D = latency + 960 + delay_frames()` (read `delay_frames` through `status().stations[0].processing.latency_ms`), and `status().stations[0].aes67.output_latency_ms` matches.
- *"…switching processing on and off never repeats audio on the AES67 output"*: fake gain 1.0; build a map from the `(left, right)` pair of `sig_a` to its position over the test's range (48 bits: no collisions in practice); decode every output position from the switch-on to 3 s after the switch-off into source positions (silence and ramp frames are skipped: they match no pair) and CHECK no source position appears twice and the matched ones are increasing.
- *"…without PTP lock the AES67 output does not send"*: `enable_ptp = true` (no master on loopback), `aes67_require_ptp` default → after 1 s `out.packets() == 0`, state `WaitingForPtp`, reason contains „czeka na zegar PTP”.
- *"…after a lock the AES67 output keeps sending in holdover"*: reuse the BounceAES PTP master from "the PTP slave follows a master on loopback" (~1136); wait for `Locked`, start receiving, stop the master engine, CHECK packets keep rising for 2 s and state becomes `Holdover`.
- *"…a stalled Stereo Tool gives silence on the output and counts gaps, then the bypass refills it"*: processing on (fake), then `fake().set("process_sleep_us", 300000)`: CHECK `gaps` rises while packets keep rising; the worker bypasses after 200 ms behind (existing rule), so content comes back: CHECK `gaps` stops rising within 2 s.
- *"…disabling the output stops its packets; the 1.1 path is untouched without an output"*: compare `engine.status()` packets stop rising after `apply_config` with `enabled = false`.

- [ ] **Step 2: Run to see them fail** — compile errors (`aes67` not in `StationStatus`).

- [ ] **Step 3: Implement** per the rules above. `aes67_sdp` returns an error („wyjście AES67 stacji jest wyłączone”) until Task 5.

- [ ] **Step 4: Run the tests**

Run: `cmake --build build -j8 && ./build/bam_tests -tc="encoder engine*,aes67*"` then the whole suite `./build/bam_tests`.
Expected: all pass; run the new engine tests 10 times (`for i in $(seq 10); do ./build/bam_tests -tc="*AES67*"; done | grep -c "Status: SUCCESS"` → 10) because they are timing based.

- [ ] **Step 5: Commit**

```bash
git add encoder/src/aes67_sender.hpp encoder/src/aes67_sender.cpp encoder/include/bam/encoder/engine.hpp encoder/src/engine.cpp tests/test_encoder_engine.cpp
git commit -m "encoder: AES67 output per station on the PTP clock, waiting before the first lock, holdover after"
```

---

### Task 5: SDP and SAP — announce, withdraw, detect conflicts

**Files:**
- Create: `encoder/src/aes67_sap.hpp`, `encoder/src/aes67_sap.cpp`
- Modify: `encoder/src/engine.cpp` (SDP builder, SAP worker lifetime, `aes67_sdp`, `conflict` in status)
- Test: `tests/test_encoder_engine.cpp`

**Interfaces:**
- Consumes: `generate_sdp`, `build_sap`, `sap_hash`, `SapDirectory`, `parse_ipv4`, `kSapGroup`, `kSapPort`.
- Produces:
  ```cpp
  // engine.cpp (Impl)
  std::string build_aes67_sdp(const StationConfig& st) const;  // like BounceAES build_output_sdp (engine/src/engine.cpp:479-508)
  // detail::Aes67Sap
  class Aes67Sap {
   public:
    Aes67Sap();
    ~Aes67Sap();  // withdraws everything announced, stops
    // the SDPs to announce (key: station id); a changed SDP withdraws the old one first
    void set_announcements(const std::string& iface_ip, std::map<std::string, std::string> sdps);
    // another announcer's stream on `address` ("<s=> (<origin>)"), or empty
    std::string conflict(const std::string& address, const std::string& own_origin) const;
  };
  ```

SDP fields (mirror BounceAES `build_output_sdp`): `session_name = aes67_name(st)`, `origin_address = iface` (or "0.0.0.0"), `multicast_address = aes67_address(st)`, `ttl = kMulticastTtl`, `port = kRtpPort`, `payload_type = 98`, `encoding = L24`, `sample_rate = 48000`, `channels = 2`, `frames_per_packet = ptime_us_to_frames(ptime_us)`, `ptime_ms = frames * 1000.0 / 48000`; `ptp_gmid`/`ptp_domain` from `ptp->status()` when it has a master, else `ptp_domain = config.ptp_domain`; `session_id = 0x42454E00 + web_port_slot` ("BEN\0"), `version = sap_hash(name|address|ptime|gmid|origin)`.

SAP worker (mirror `engine/src/sap_worker.cpp`): runs only while at least one output is announced (`set_announcements` with an empty map stops the socket and withdraws); socket `open(kSapPort)`, `join(kSapGroup, iface)`, `set_multicast_out(iface, kMulticastTtl, true)`; announce on change and every 30 s; feed received packets to a `SapDirectory`; `expire` every 10 s; on destruction send a deletion for every announced SDP. Announcements happen only for outputs that are allowed to send (Task 4 gating) — an output waiting for PTP is not announced.

Status: `conflict` = `sap->conflict(address, iface)`; when it becomes non-empty log a warning once („Wyjście AES67 stacji „X”: ten sam adres ogłasza …”). The state stays `Sending`; the panel shows the warning.

- [ ] **Step 1: Write the failing tests** — a SAP listener helper on loopback (`UdpSocket::open(kSapPort)`, `join(kSapGroup, kLoopback, "")`, `parse_sap`), then:
  - *"AES67 output: announced by SAP with its SDP, withdrawn when disabled and at stop"*: `enable_sap = true`, `aes67_require_ptp = false`; CHECK an announcement whose SDP parses to the output's address, ptime and name arrives within 2 s; disable → a deletion with the same hash; enable again, `engine.stop()` → a deletion.
  - *"AES67 output: another device announcing the same address is reported, the address stays"*: send a foreign SAP announcement (`build_sap(false, 0x1111, {127,0,0,2}, sdp_for_same_address)`) to `kSapGroup:kSapPort` on loopback; CHECK `status().stations[0].aes67.conflict` contains the foreign `s=` name within 2 s, `address` unchanged, `state == Sending`, `logged("ten sam adres ogłasza")`.
  - *"AES67 output: the SDP"*: `engine.aes67_sdp("s1", &err)` parses with `parse_sdp`, multicast address/ptime/name/channels/encoding as configured, `ts-refclk` present.
- [ ] **Step 2: Run to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `./build/bam_tests -tc="AES67 output*,encoder engine*"`, then the whole suite. Expected: pass.
- [ ] **Step 5: Commit** `git commit -m "encoder: AES67 output SDP, SAP announcements and address conflict warnings"`

---

### Task 6: HTTP API

**Files:**
- Modify: `encoder/src/http_server.cpp` (`status_json` ~220, `keep_processing` ~482 → also keep `aes67_output`, a new route next to `/processing/web-password` ~1019)
- Test: `tests/test_encoder_http.cpp`

**Interfaces:**
- Consumes: `EncoderEngine::aes67_sdp`, `Aes67Status`.
- Produces (JSON):
  - station config: `"aes67_output": {"enabled","name","address","ptime_us"}` (Task 1 serialisation).
  - status station object: `"aes67": {"state": "off"|"waiting_for_ptp"|"sending"|"holdover"|"error", "reason", "address", "name", "ptime_us", "output_latency_ms", "packets", "gaps", "send_errors", "conflict"}`.
  - `GET /api/stations/<id>/aes67.sdp` (kSession): 200 `application/sdp`, header `Content-Disposition: attachment; filename="<station name>.sdp"` (ASCII-fold the name like the preset download does; fall back to `<id>.sdp`), 404 `no_station`, 409 `not_running` with the engine's reason.

Steps:
- [ ] **Step 1: failing tests** in `tests/test_encoder_http.cpp` with the file's server fixture: `PUT /api/stations/<id>` without `aes67_output` keeps the stored one; with it replaces it; `GET /api/status` has `stations[0].aes67.state == "off"` for a station without output; the SDP route is 401 without a session cookie, 409 when the output is off, 200 with `Content-Type: application/sdp` and a body starting with `v=0` when on (engine options `aes67_require_ptp = false`, `enable_sap = false`, loopback).
- [ ] **Step 2: run, see them fail.**
- [ ] **Step 3: implement** (`keep_processing` → generalise to keep both `"processing"` and `"aes67_output"` when the body omits them; name it `keep_station_parts`).
- [ ] **Step 4: run** `./build/bam_tests -tc="encoder http*"`, then the whole suite.
- [ ] **Step 5: commit** `git commit -m "encoder: AES67 output in the API, SDP download"`

---

### Task 7: Panel

**Files:**
- Modify: `encoder/panel/index.html` (a container `#edit-aes67` between `#edit-processing` and the streams section, ~117)
- Modify: `encoder/panel/app.js` (`renderEditor` ~546, a new `aes67Block()` next to `processingBlock()` ~969, `stationCard` ~337 badge, `applyLive` ~1482 live fields, save path)
- Modify: `encoder/panel/logic.js` (pure helpers), `encoder/panel/style.css` if needed (reuse `panel-common` classes)
- Test: `tests/panel/encoder_panel_logic.test.js`

**Interfaces:**
- Consumes: the JSON of Task 6.
- Produces (`logic.js`, on `L`):
  ```js
  L.aes67DefaultAddress(slot)          // "239.69.110.<slot+1>" or ""
  L.aes67AddressError(text)            // "" when empty or 239.x.x.x, else the Polish message
  L.aes67StateLabel(aes67)             // "nadaje · 239.69.110.1", "czeka na zegar PTP", "zegar podtrzymany (brak mastera PTP)", "błąd: …", "wyłączone"
  L.aes67Dot(aes67)                    // "ok" | "warn" | "err" | "off"  (warn: waiting, holdover, conflict)
  L.PTIMES = [[1000, "1 ms"], [1333, "1,333 ms"], [4000, "4 ms"]]
  ```

Section content (spec §7): switch „Wyjście AES67”, Nazwa (placeholder = station name), Adres multicast (placeholder = default address; inline error from `aes67AddressError`), Czas pakietu (select from `PTIMES`), live fields Stan / Adres / Opóźnienie wyjścia („N ms”) / Pakiety · Luki, conflict line in warning colour („ten sam adres ogłasza …”), buttons **Kopiuj SDP** (fetch `aes67.sdp`, `navigator.clipboard.writeText`, fallback: select text in a read-only textarea, as the Stereo Tool password does) and **Pobierz SDP** (link to the route). Station card: badge „AES67” with a dot class from `aes67Dot`. Save: the station PUT includes `aes67_output`.

Steps:
- [ ] **Step 1: failing logic tests** (append to `tests/panel/encoder_panel_logic.test.js`):
```js
test("aes67 helpers", () => {
  assert.equal(L.aes67DefaultAddress(0), "239.69.110.1");
  assert.equal(L.aes67DefaultAddress(-1), "");
  assert.equal(L.aes67AddressError(""), "");
  assert.equal(L.aes67AddressError("239.1.2.3"), "");
  assert.match(L.aes67AddressError("10.0.0.1"), /239/);
  assert.match(L.aes67AddressError("239.1.2"), /239/);
  assert.equal(L.aes67StateLabel({ state: "sending", address: "239.69.110.1" }), "nadaje · 239.69.110.1");
  assert.equal(L.aes67StateLabel({ state: "waiting_for_ptp", reason: "czeka na zegar PTP" }), "czeka na zegar PTP");
  assert.equal(L.aes67Dot({ state: "sending", conflict: "" }), "ok");
  assert.equal(L.aes67Dot({ state: "sending", conflict: "X (127.0.0.2)" }), "warn");
  assert.equal(L.aes67Dot({ state: "holdover" }), "warn");
  assert.equal(L.aes67Dot({ state: "error" }), "err");
  assert.equal(L.aes67Dot({ state: "off" }), "off");
});
```
- [ ] **Step 2: run** `node --test tests/panel/encoder_panel_logic.test.js` → fails (`L.aes67DefaultAddress is not a function`).
- [ ] **Step 3: implement** helpers and UI.
- [ ] **Step 4: run** the node tests (pass) and check the UI in a browser against a local service on loopback: build `encoder_service`, run it with a config whose `interface_ip` is `127.0.0.1`, open `http://127.0.0.1:8070` with `playwright-cli open --browser=chromium` (scratchpad), screenshot the editor with the output off, on (waiting), and the station list; verify layout at 1280 px and 390 px wide. Never run it against a real interface.
- [ ] **Step 5: commit** `git commit -m "encoder panel: AES67 output section, SDP copy and download, station badge"`

---

### Task 8: Documentation, manual tests, version 1.2.0

**Files:**
- Modify: `docs/ENCODER-INSTALL.md` (signal diagram at the top, requirements, new chapter „Wyjście AES67” after chapter 10, troubleshooting table, firewall note: outgoing multicast only), `docs/ENCODER-TESTING.md` (automatic summary + manual rows), `README.md` (1.2.0 line), screenshots `docs/images/encoder/aes67*.png` from Task 7's browser run.
- Modify: version `1.1.0` → `1.2.0` in `encoder/include/bam/encoder/version.hpp:7`, `encoder/src/icecast_client.cpp:33`, `encoder_service/encoder.rc:18,20` (and the numeric FILEVERSION/PRODUCTVERSION lines of the same file), `encoder_tray/encoder_tray.rc:25,27` (same), `encoder_tray/encoder_tray.manifest:3` (`1.2.0.0`), `installer/nsis/BounceEncoder-setup.nsi:62,85`.
- Test: `git grep -n "1\.1\.0" -- encoder encoder_service encoder_tray installer/nsis/BounceEncoder-setup.nsi` returns nothing.

Chapter content (Polish, the guide's style; spec §3–§6): what the output sends (the same signal as the streams, with Stereo Tool only when the station has processing), settings, default addresses `239.69.110.N`, SDP/SAP and how a transmitter/Axia/Orban subscribes, output latency and the recommended receiver link offset (at least 3 × packet time), clock behaviour (waits before the first lock; holdover after; PTP off → no output), what happens when processing is switched on (≈ 90 ms hold, no repeat), conflicts (warning, address never changed automatically), loop protection, statistics counters (gaps should stay 0), and a troubleshooting table (no packets: PTP / interface / IGMP; receiver silent: link offset, address, ptime; gaps: CPU).

Manual tests (ENCODER-TESTING): BounceAES Master on the same computer subscribes to the output (Przeglądaj… shows it via SAP) and plays it; an Axia node or Orban receives it; the transmitter/link codec receives it; switching processing on: short hold, no repeat; Orban off for 1 min: holdover warning, sound continues, return on Orban back; 24 h: gaps 0.

- [ ] **Step 1:** write the docs and version changes.
- [ ] **Step 2:** run `git grep` check above (empty), full suite `./build/bam_tests` and node tests (pass).
- [ ] **Step 3: commit** `git commit -m "docs: AES67 output chapter and manual tests; Bounce Encoder 1.2.0"`
