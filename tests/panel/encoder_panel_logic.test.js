// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder panel: the pure helpers of encoder/panel/logic.js. Runs with node's built-in
// test runner (no dependencies): node --test tests/panel/encoder_panel_logic.test.js (also a ctest
// entry when node is installed).
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const L = require(path.join(__dirname, "..", "..", "encoder", "panel", "logic.js"));

// The /api/codecs answer in its real shape (task 8), trimmed to what the helpers read.
const MP3_STEPS = [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320];
const codecs = {
  rates: [48000, 44100, 32000, 24000, 22050],
  codecs: [
    { id: "mp3", label: "MP3", available: true, reason: "", lossless: false, min_kbps: 32, max_kbps: 320,
      rates: [{ sample_rate: 48000, min_kbps: 32, max_kbps: 320, steps: MP3_STEPS },
        { sample_rate: 44100, min_kbps: 32, max_kbps: 320, steps: MP3_STEPS },
        { sample_rate: 22050, min_kbps: 32, max_kbps: 160, steps: [32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160] }] },
    { id: "aac", label: "AAC-LC", available: false, reason: "kodek AAC-LC niedostępny: brak biblioteki libfdk-aac obok programu",
      lossless: false, min_kbps: 32, max_kbps: 320,
      rates: [{ sample_rate: 48000, min_kbps: 32, max_kbps: 320, steps: null },
        { sample_rate: 44100, min_kbps: 32, max_kbps: 320, steps: null },
        { sample_rate: 24000, min_kbps: 32, max_kbps: 288, steps: null }] },
    { id: "he-aac", label: "HE-AAC", available: true, reason: "", lossless: false, min_kbps: 24, max_kbps: 128,
      rates: [{ sample_rate: 48000, min_kbps: 24, max_kbps: 128, steps: null }, { sample_rate: 44100, min_kbps: 24, max_kbps: 128, steps: null }] },
    { id: "he-aac-v2", label: "HE-AACv2", available: true, reason: "", lossless: false, min_kbps: 16, max_kbps: 64,
      rates: [{ sample_rate: 48000, min_kbps: 16, max_kbps: 64, steps: null }, { sample_rate: 32000, min_kbps: 16, max_kbps: 40, steps: null }] },
    { id: "flac", label: "FLAC", available: true, reason: "", lossless: true, min_kbps: null, max_kbps: null,
      rates: [48000, 44100, 32000, 24000, 22050].map((r) => ({ sample_rate: r, min_kbps: null, max_kbps: null, steps: null })) },
  ],
  mp3_equivalents: [
    { codec: "he-aac-v2", min_kbps: 32, max_kbps: 32, mp3: "~96 kb/s" },
    { codec: "he-aac", min_kbps: 48, max_kbps: 64, mp3: "~128 kb/s" },
    { codec: "aac", min_kbps: 96, max_kbps: 96, mp3: "~128–160 kb/s" },
    { codec: "aac", min_kbps: 128, max_kbps: 128, mp3: "~192 kb/s" },
    { codec: "aac", min_kbps: 192, max_kbps: null, mp3: "~256–320 kb/s" },
  ],
  recommendations: [
    { codec: "he-aac-v2", min_kbps: null, max_kbps: 48 },
    { codec: "he-aac", min_kbps: 48, max_kbps: 96 },
    { codec: "aac", min_kbps: 96, max_kbps: null },
  ],
  recommendation_text: "HE-AACv2 do 48 kb/s, HE-AAC 48–96 kb/s, AAC-LC powyżej 96 kb/s",
};

test("formatting uses Polish decimals, a real minus and silence as −∞", () => {
  assert.equal(L.fmtGain(3), "+3,0 dB");
  assert.equal(L.fmtGain(0), "0,0 dB");
  assert.equal(L.fmtGain(-6.5), "−6,5 dB");
  assert.equal(L.fmtPeak(-120), "−∞");
  assert.equal(L.fmtPeak(-12.34), "−12,3");
  assert.equal(L.fmtDelay(2.5), "2,5 s");
  assert.equal(L.fmtRate(44100), "44,1 kHz");
  assert.equal(L.fmtRate(48000), "48 kHz");
  assert.equal(L.fmtRate(22050), "22,05 kHz");
  assert.equal(L.fmtKbps(128.44), "128,4 kb/s");
  assert.equal(L.fmtBytes(999), "999 B");
  assert.equal(L.fmtBytes(1500000), "1,5 MB");
  assert.equal(L.fmtUptime(90061), "1 d 01:01:01");
  assert.equal(L.fmtUptime(59), "00:00:59");
});

test("hand-typed numbers accept a decimal comma", () => {
  assert.equal(L.parseNumber("2,5"), 2.5);
  assert.equal(L.parseNumber(" 12.3 "), 12.3);
  assert.ok(Number.isNaN(L.parseNumber("")));
  assert.ok(Number.isNaN(L.parseNumber("abc")));
});

test("Polish plurals", () => {
  assert.equal(L.plural(1, "strumień", "strumienie", "strumieni"), "strumień");
  assert.equal(L.plural(3, "strumień", "strumienie", "strumieni"), "strumienie");
  assert.equal(L.plural(5, "strumień", "strumienie", "strumieni"), "strumieni");
  assert.equal(L.plural(12, "strumień", "strumienie", "strumieni"), "strumieni");
  assert.equal(L.plural(22, "strumień", "strumienie", "strumieni"), "strumienie");
});

test("bitrate choices come from /api/codecs only (rule E2)", () => {
  // MP3: the listed steps, per sample rate
  assert.deepEqual(L.bitrateChoices(codecs, "mp3", 44100), MP3_STEPS);
  assert.equal(Math.max(...L.bitrateChoices(codecs, "mp3", 22050)), 160);
  // AAC-LC: any value min..max, offered as min, max and multiples of 8
  const lc24 = L.bitrateChoices(codecs, "aac", 24000);
  assert.equal(lc24[0], 32);
  assert.equal(lc24[lc24.length - 1], 288);
  assert.ok(lc24.every((v) => v === 32 || v === 288 || v % 8 === 0));
  // a stored value inside the range is offered even when it is not a multiple of 8
  assert.ok(L.bitrateChoices(codecs, "aac", 48000, 100).includes(100));
  assert.ok(!L.bitrateChoices(codecs, "aac", 48000, 400).includes(400));
  // HE-AACv2 at 32 kHz stops at 40
  assert.deepEqual(L.bitrateChoices(codecs, "he-aac-v2", 32000), [16, 24, 32, 40]);
  // FLAC has no bitrate; an unlisted rate has no choices
  assert.deepEqual(L.bitrateChoices(codecs, "flac", 48000), []);
  assert.deepEqual(L.bitrateChoices(codecs, "he-aac", 22050), []);
});

test("a codec change keeps the stream on an allowed rate and bitrate", () => {
  const s = { codec: "he-aac-v2", sample_rate: 44100, bitrate_kbps: 128 };
  L.adjustStream(codecs, s);
  assert.equal(s.sample_rate, 48000);  // 44.1 kHz is not listed for HE-AACv2 in the fixture
  assert.equal(s.bitrate_kbps, 64);
  const m = { codec: "mp3", sample_rate: 22050, bitrate_kbps: 320 };
  L.adjustStream(codecs, m);
  assert.equal(m.bitrate_kbps, 160);
  const f = { codec: "flac", sample_rate: 44100, bitrate_kbps: 128 };
  L.adjustStream(codecs, f);
  assert.deepEqual(f, { codec: "flac", sample_rate: 44100, bitrate_kbps: 128 });
  assert.equal(L.nearest([32, 40, 48], 44), 40);
});

test("the bitrate hint shows the MP3 equivalent and the codec advice of spec §8", () => {
  assert.deepEqual(L.bitrateHint(codecs, "he-aac-v2", 32), { mp3: "~96 kb/s", advice: "", text: codecs.recommendation_text });
  assert.equal(L.bitrateHint(codecs, "he-aac", 48).mp3, "~128 kb/s");
  assert.equal(L.bitrateHint(codecs, "he-aac", 64).mp3, "~128 kb/s");
  assert.equal(L.bitrateHint(codecs, "aac", 96).mp3, "~128–160 kb/s");
  assert.equal(L.bitrateHint(codecs, "aac", 128).mp3, "~192 kb/s");
  assert.equal(L.bitrateHint(codecs, "aac", 256).mp3, "~256–320 kb/s");
  assert.equal(L.bitrateHint(codecs, "aac", 112).mp3, "");  // no row: no equivalent shown
  // a profile outside its recommended range gets advice
  assert.equal(L.bitrateHint(codecs, "aac", 32).advice, "przy 32 kb/s lepiej: HE-AACv2");
  assert.equal(L.bitrateHint(codecs, "he-aac-v2", 64).advice, "przy 64 kb/s lepiej: HE-AAC");
  assert.equal(L.bitrateHint(codecs, "he-aac", 128).advice, "przy 128 kb/s lepiej: AAC-LC");
  assert.equal(L.bitrateHint(codecs, "he-aac", 96).advice, "");  // 96: HE-AAC and AAC-LC both fit
  // MP3 and FLAC: no AAC hint at all
  assert.equal(L.bitrateHint(codecs, "mp3", 128), null);
  assert.equal(L.bitrateHint(codecs, "flac", 0), null);
});

test("the AAC ↔ MP3 table rows", () => {
  assert.deepEqual(L.equivalentRows(codecs).map((r) => `${r.aac} = ${r.mp3}`), [
    "HE-AACv2 32 kb/s = ~96 kb/s",
    "HE-AAC 48–64 kb/s = ~128 kb/s",
    "AAC-LC 96 kb/s = ~128–160 kb/s",
    "AAC-LC 128 kb/s = ~192 kb/s",
    "AAC-LC 192+ kb/s = ~256–320 kb/s",
  ]);
  assert.equal(L.reasonDetail(codecs.codecs[1].reason), "brak biblioteki libfdk-aac obok programu");
});

test("SDP summary", () => {
  const sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Studio A\r\nc=IN IP4 239.69.230.1/1\r\nt=0 0\r\n" +
    "m=audio 15230 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:1\r\n";
  const info = L.sdpInfo(sdp);
  assert.deepEqual(info, { name: "Studio A", address: "239.69.230.1", port: 15230, encoding: "L24", rate: 48000, channels: 2, ptime: 1 });
  assert.equal(L.sdpSummary(info), "239.69.230.1:15230 · L24 · 48 kHz · stereo · 1 ms");
  assert.equal(L.sdpSummary(L.sdpInfo("v=0\ns=x\na=rtpmap:96 L16/48000\na=ptime:0.333")), "L16 · 48 kHz · mono · 0,333 ms");
  assert.equal(L.sdpInfo("  \n"), null);
});

test("validation issues map onto the edited station's fields", () => {
  const issues = [
    { field: "stations[2].streams[0].port", message: "port" },
    { field: "stations[2].streams[1]", message: "kodek" },
    { field: "stations[2].streams[1].mount", message: "mount 1" },
    { field: "stations[2].streams[1].mount", message: "mount 2" },
    { field: "stations[0].streams[0].mount", message: "inna stacja" },
    { field: "", message: "nieprawidłowy JSON" },
  ];
  const m = L.mapIssues(issues, 2);
  assert.deepEqual(m.fields, { "streams[0].port": ["port"], "streams[1]": ["kodek"], "streams[1].mount": ["mount 1", "mount 2"] });
  assert.deepEqual(m.general, ["inna stacja", "nieprawidłowy JSON"]);
  // stations[1] is not a prefix of stations[12]
  assert.deepEqual(L.mapIssues([{ field: "stations[12].name", message: "x" }], 1), { fields: {}, general: ["x"] });
  assert.deepEqual(L.mapIssues([{ field: "stations[0].name", message: "x" }], -1), { fields: {}, general: ["x"] });
});

test("rule E7: a stored password is not carried to another server", () => {
  const saved = { id: "str1", host: "Stream.Example", port: 8000, password_set: true };
  assert.equal(L.needsPasswordAgain(saved, { host: "stream.example", port: 8000, password: "" }), false);  // host case does not matter
  assert.equal(L.needsPasswordAgain(saved, { host: "other.example", port: 8000, password: "" }), true);
  assert.equal(L.needsPasswordAgain(saved, { host: "stream.example", port: 8001, password: "" }), true);
  assert.equal(L.needsPasswordAgain(saved, { host: "other.example", port: 8000, password: "nowe" }), false);
  assert.equal(L.needsPasswordAgain({ ...saved, password_set: false }, { host: "other.example", port: 8000, password: "" }), false);
  assert.equal(L.needsPasswordAgain(null, { host: "x", port: 1, password: "" }), false);  // a new stream
});

test("the station body drops panel-only keys and password_set", () => {
  const body = L.stationBody({ id: "s1", name: "R", enabled: true, inputs: [{ _k: 0, sdp: "v=0" }],
    streams: [{ id: "str1", password: "", password_set: true, host: "h" }] });
  assert.deepEqual(body, { id: "s1", name: "R", enabled: true, inputs: [{ sdp: "v=0" }], streams: [{ id: "str1", password: "", host: "h" }] });
});

test("new items have the defaults of the configuration", () => {
  assert.deepEqual(L.newInput(), { enabled: true, name: "", sdp: "", gain_db: 0, delay_s: 0, latency_us: 5000 });
  const s = L.newStream(codecs);
  assert.equal(s.codec, "mp3");
  assert.equal(s.user, "source");
  assert.equal(s.bitrate_kbps, 128);
  assert.equal(s.sample_rate, 44100);
  assert.equal("id" in s, false);  // the server gives it an id
  const onlyFlac = { ...codecs, codecs: codecs.codecs.map((c) => ({ ...c, available: c.id === "flac" })) };
  assert.equal(L.newStream(onlyFlac).codec, "flac");
  assert.deepEqual(L.newStation().streams, []);
});

test("live states in Polish", () => {
  assert.deepEqual(L.streamState({ enabled: true, codec_available: true, state: "streaming", kbps: 128.2 }, true), { kind: "on", text: "nadaje · 128,2 kb/s" });
  assert.deepEqual(L.streamState({ enabled: true, codec_available: true, state: "error", error: "serwer nieosiągalny" }, true), { kind: "err", text: "serwer nieosiągalny" });
  assert.equal(L.streamState({ enabled: true, codec_available: true, state: "connecting" }, true).kind, "warn");
  assert.equal(L.streamState({ enabled: true, codec_available: false, state: "idle" }, true).text, "kodek niedostępny");
  assert.equal(L.streamState({ enabled: true, codec_available: true, encoder_failed: true, encoder_error: "x" }, true).text, "błąd kodera: x");
  assert.equal(L.streamState({ enabled: false }, true).text, "wyłączony");
  assert.equal(L.streamState({ enabled: true }, false).text, "stacja wyłączona");
  assert.deepEqual(L.inputState({ enabled: true, signal: true, error: "" }, true), { kind: "on", text: "sygnał" });
  assert.deepEqual(L.inputState({ enabled: true, signal: false, error: "" }, true), { kind: "err", text: "brak sygnału" });
  assert.equal(L.inputState({ enabled: true, signal: false, error: "nieprawidłowy SDP: x" }, true).text, "nieprawidłowy SDP: x");
  assert.equal(L.stationState({ enabled: true, running: true, uptime_s: 61 }).text, "działa · 00:01:01");
  assert.equal(L.stationState({ enabled: false }).text, "wyłączona");
});

test("the station shape is the same for /api/config and /api/live stations", () => {
  const cfg = [{ id: "s1", name: "R", enabled: true, inputs: [{ sdp: "" }], streams: [{ id: "str1", enabled: true, codec: "mp3", host: "h", port: 8000, mount: "/a", password: "" }] }];
  const live = [{ id: "s1", name: "R", enabled: true, running: true, inputs: [{ index: 0, signal: false }],
    streams: [{ id: "str1", enabled: true, codec: "mp3", host: "h", port: 8000, mount: "/a", state: "idle" }] }];
  assert.equal(L.stationsShape(cfg), L.stationsShape(live));
  live[0].streams[0].mount = "/b";
  assert.notEqual(L.stationsShape(cfg), L.stationsShape(live));
});

// ---------------------------------------------------------------- fix round 1
test("a configuration reload keeps a clean editor unless its station changed or went away", () => {
  const s1 = { id: "s1", name: "R1", enabled: true, inputs: [], streams: [] };
  const s2 = { id: "s2", name: "R2", enabled: true, inputs: [], streams: [] };
  // another panel renamed s2: the clean s1 editor stays as it is
  assert.equal(L.editorReload("s1", false, [s1, { ...s2, name: "R2 FM" }], s1), "keep");
  // s1 itself changed: reopen it with the new values
  assert.equal(L.editorReload("s1", false, [{ ...s1, enabled: false }, s2], s1), "reopen");
  // s1 was deleted: close the editor
  assert.equal(L.editorReload("s1", false, [s2], s1), "close");
  // unsaved edits are never touched; a new station is not on the server yet
  assert.equal(L.editorReload("s1", true, [{ ...s1, enabled: false }], s1), "keep");
  assert.equal(L.editorReload("s1", true, [s2], s1), "keep");
  assert.equal(L.editorReload("new", false, [s1], null), "keep");
  assert.equal(L.editorReload(null, false, [s1], null), "keep");
});

test("an open draft survives a new login while its station exists", () => {
  const stations = [{ id: "s1" }, { id: "s3" }];
  assert.equal(L.keepDraft("s1", stations), true);
  assert.equal(L.keepDraft("s2", stations), false);
  assert.equal(L.keepDraft("new", stations), true);
  assert.equal(L.keepDraft(null, stations), false);
});

test("a new station is dirty only once something was typed", () => {
  const original = L.draftJson(L.newStation());
  assert.equal(L.draftJson(L.newStation()), original);
  const typed = L.newStation();
  typed.name = "Radio";
  assert.notEqual(L.draftJson(typed), original);
  // panel-only keys do not make a draft dirty
  const s = { id: "s1", name: "R", inputs: [{ sdp: "" }], streams: [{ id: "str1", password: "", password_set: true }] };
  const d = JSON.parse(JSON.stringify(s));
  d.inputs[0]._k = 0;
  assert.equal(L.draftJson(d), L.draftJson(s));
});

test("a station changed in another panel under an open draft is noticed", () => {
  const saved = { id: "s1", name: "R", enabled: true, streams: [{ id: "str1", password_set: true }] };
  assert.equal(L.changedElsewhere(saved, JSON.parse(JSON.stringify(saved))), false);
  assert.equal(L.changedElsewhere(saved, { ...saved, name: "R FM" }), true);
  assert.equal(L.changedElsewhere(saved, undefined), true);  // deleted
  assert.equal(L.changedElsewhere(null, undefined), false);  // a new station
});

test("hand-typed numbers: both decimal separators, a real minus, and errors instead of silent values", () => {
  assert.equal(L.parseNumber("2,5"), 2.5);
  assert.equal(L.parseNumber("2.5"), 2.5);
  assert.equal(L.parseNumber("−3"), -3);
  assert.deepEqual(L.checkNumber("2,5"), { ok: true, value: 2.5, error: "" });
  assert.deepEqual(L.checkNumber(" -6 "), { ok: true, value: -6, error: "" });
  assert.equal(L.checkNumber("abc").ok, false);
  assert.equal(L.checkNumber("").ok, false);
  assert.equal(L.checkNumber("1e3").ok, false);  // only plain decimals
  assert.equal(L.checkNumber("8000", { integer: true }).value, 8000);
  assert.equal(L.checkNumber("80,5", { integer: true }).ok, false);
  assert.match(L.checkNumber("x").error, /liczbę/);
  assert.equal(L.numberText(-3), "-3");
  assert.equal(L.numberText(2.5), "2,5");
});

test("the AAC profiles come from /api/codecs", () => {
  assert.deepEqual([...L.aacCodecs(codecs)].sort(), ["aac", "he-aac", "he-aac-v2"]);
  const renamed = { ...codecs, recommendations: [{ codec: "x-aac", min_kbps: null, max_kbps: null }], mp3_equivalents: [] };
  assert.deepEqual([...L.aacCodecs(renamed)], ["x-aac"]);
});

test("renaming a running station warns that its streams reconnect", () => {
  const saved = { id: "s1", name: "Radio", enabled: true, streams: [{ id: "str1", enabled: true }] };
  assert.equal(L.renameRestartsStreams(saved, { ...saved, name: "Radio Dwa" }), true);
  assert.equal(L.renameRestartsStreams(saved, { ...saved }), false);  // the name unchanged
  assert.equal(L.renameRestartsStreams(null, { ...saved, name: "Nowa" }), false);  // a new station
  assert.equal(L.renameRestartsStreams({ ...saved, enabled: false }, { ...saved, name: "X" }), false);  // not running
  assert.equal(L.renameRestartsStreams(saved, { ...saved, name: "X", enabled: false }), false);  // stops anyway
  assert.equal(L.renameRestartsStreams(saved, { ...saved, name: "X", streams: [{ id: "str1", enabled: false }] }), false);
});
