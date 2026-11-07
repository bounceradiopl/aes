// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder web panel: pure helpers without the DOM (formatting, the codec choices from
// /api/codecs, the bitrate hint of spec §8, SDP summary, validation issues, rule E7, Stereo Tool
// processing: key and preset name rules, states). Loaded by the panel before app.js
// (window.EncoderLogic) and by the node tests in tests/panel/ (module.exports).
"use strict";

(function (L) {
  L.LIMITS = {
    stations: 16, inputs: 3, streams: 8,
    gainMin: -60, gainMax: 12, gainStep: 0.5,
    delayMax: 60, delayStep: 0.1,
    latencyMinMs: 1, latencyMaxMs: 1000,
    sdpMaxBytes: 64 * 1024,
    keyMaxChars: 256,           // kMaxStereoToolKeyChars
    presetNameMaxChars: 64,     // kMaxPresetNameChars
    presetMaxBytes: 4 * 1024 * 1024,
    webPortBaseMin: 1024, webPortBaseMax: 65000,
  };

  // ---------------------------------------------------------------- formatting (Polish)
  const MINUS = "−";

  L.fmtNum = (v, digits = 0) => {
    const s = Number(v || 0).toLocaleString("pl-PL", { minimumFractionDigits: digits, maximumFractionDigits: digits });
    return s.replace("-", MINUS);
  };

  // A gain: "+3,0 dB", "0,0 dB", "−6,5 dB".
  L.fmtGain = (db) => {
    const v = Number(db || 0);
    return `${v > 0 ? "+" : ""}${L.fmtNum(v, 1)} dB`;
  };

  // A peak level in dBFS; −120 (and anything that low) is silence.
  L.fmtPeak = (db) => (db <= -119.5 ? `${MINUS}∞` : L.fmtNum(db, 1));

  L.fmtDelay = (s) => `${L.fmtNum(s, 1)} s`;

  L.fmtRate = (hz) => `${(Number(hz) / 1000).toLocaleString("pl-PL", { maximumFractionDigits: 2 })} kHz`;

  L.fmtKbps = (v) => `${L.fmtNum(v, 1)} kb/s`;

  L.fmtPct = (v) => `${L.fmtNum(v, 1)} %`;

  L.fmtMs = (v) => `${L.fmtNum(v, 1)} ms`;

  // A wait in whole seconds: "25 s", "2 min 5 s", "10 min".
  L.fmtWait = (s) => {
    const t = Math.max(0, Math.round(Number(s || 0)));
    if (t < 60) return `${t} s`;
    return t % 60 ? `${Math.floor(t / 60)} min ${t % 60} s` : `${t / 60} min`;
  };

  L.fmtBytes = (n) => {
    const v = Number(n || 0);
    if (v < 1000) return `${L.fmtNum(v)} B`;
    if (v < 1e6) return `${L.fmtNum(v / 1e3, 1)} kB`;
    if (v < 1e9) return `${L.fmtNum(v / 1e6, 1)} MB`;
    return `${L.fmtNum(v / 1e9, 2)} GB`;
  };

  const pad2 = (n) => String(n).padStart(2, "0");
  L.pad2 = pad2;

  L.fmtUptime = (s) => {
    const t = Math.max(0, Math.floor(Number(s || 0)));
    const d = Math.floor(t / 86400), h = Math.floor(t % 86400 / 3600), m = Math.floor(t % 3600 / 60);
    return (d ? `${d} d ` : "") + `${pad2(h)}:${pad2(m)}:${pad2(t % 60)}`;
  };

  // A number typed by hand: "2,5" and "2.5" both work, and so does a typographic minus; NaN when
  // empty or not a plain decimal number.
  L.parseNumber = (text) => {
    const s = String(text ?? "").trim().replace(",", ".").replace(MINUS, "-");
    return /^[-+]?(\d+\.?\d*|\.\d+)$/.test(s) ? Number(s) : NaN;
  };

  // A number field's text checked: { ok, value, error } (error in Polish, for the field).
  L.checkNumber = (text, { integer = false } = {}) => {
    const value = L.parseNumber(text);
    if (Number.isNaN(value)) return { ok: false, value, error: integer ? "wpisz liczbę całkowitą" : "wpisz liczbę, np. 2,5" };
    if (integer && !Number.isInteger(value)) return { ok: false, value, error: "wpisz liczbę całkowitą" };
    return { ok: true, value, error: "" };
  };

  // A model value as a number field shows it: a decimal comma, a plain minus (parseNumber reads it back).
  L.numberText = (v) => String(v).replace(".", ",");

  L.plural = (n, one, few, many) => {
    if (n === 1) return one;
    const d = n % 10, t = n % 100;
    return d >= 2 && d <= 4 && (t < 12 || t > 14) ? few : many;
  };

  // ---------------------------------------------------------------- codecs (/api/codecs)
  L.codec = (codecs, id) => (codecs?.codecs || []).find((c) => c.id === id) || null;

  L.codecLabel = (codecs, id) => L.codec(codecs, id)?.label || id;

  L.ratesFor = (codecs, id) => (L.codec(codecs, id)?.rates || []).map((r) => r.sample_rate);

  // The bitrates to offer for a codec at a sample rate, built only from the API (rule E2): the
  // listed steps when the allowed set has gaps (MP3), else min, max and every multiple of 8 in
  // between (any whole value from min to max is valid, so `current` is offered too when it fits).
  L.bitrateChoices = (codecs, id, rate, current) => {
    const info = L.codec(codecs, id);
    if (!info || info.lossless) return [];
    const r = info.rates.find((x) => x.sample_rate === rate);
    if (!r) return [];
    if (Array.isArray(r.steps)) return r.steps.slice();
    const out = new Set([r.min_kbps, r.max_kbps]);
    for (let v = Math.ceil(r.min_kbps / 8) * 8; v <= r.max_kbps; v += 8) out.add(v);
    if (Number.isInteger(current) && current >= r.min_kbps && current <= r.max_kbps) out.add(current);
    return [...out].sort((a, b) => a - b);
  };

  L.nearest = (list, v) => list.reduce((best, x) => (best == null || Math.abs(x - v) < Math.abs(best - v) ? x : best), null);

  // After a codec or sample-rate change: keep the stream on an allowed rate and bitrate.
  L.adjustStream = (codecs, s) => {
    const rates = L.ratesFor(codecs, s.codec);
    if (rates.length && !rates.includes(s.sample_rate))
      s.sample_rate = [44100, 48000].find((r) => rates.includes(r)) || rates[0];
    const choices = L.bitrateChoices(codecs, s.codec, s.sample_rate, s.bitrate_kbps);
    if (choices.length && !choices.includes(s.bitrate_kbps)) s.bitrate_kbps = L.nearest(choices, s.bitrate_kbps);
    return s;
  };

  const inRange = (row, kbps) => (row.min_kbps == null || kbps >= row.min_kbps) && (row.max_kbps == null || kbps <= row.max_kbps);

  // Spec §8: what an AAC bitrate sounds like in MP3 terms ("~128 kb/s"), "" when the table has no row.
  L.mp3Equivalent = (codecs, id, kbps) =>
    (codecs?.mp3_equivalents || []).find((e) => e.codec === id && inRange(e, kbps))?.mp3 || "";

  // The AAC profiles spec §8 recommends for a bitrate.
  L.recommendedCodecs = (codecs, kbps) => (codecs?.recommendations || []).filter((r) => inRange(r, kbps)).map((r) => r.codec);

  // The AAC profiles: the codecs the spec §8 tables of /api/codecs talk about.
  L.aacCodecs = (codecs) => new Set([...(codecs?.recommendations || []), ...(codecs?.mp3_equivalents || [])].map((r) => r.codec));

  // The hint next to the bitrate field of an AAC profile: { mp3, advice, text }; null for any other codec.
  L.bitrateHint = (codecs, id, kbps) => {
    const info = L.codec(codecs, id);
    if (!info || info.lossless || !L.aacCodecs(codecs).has(id)) return null;
    const rec = L.recommendedCodecs(codecs, kbps);
    const advice = rec.length && !rec.includes(id) ? `przy ${kbps} kb/s lepiej: ${rec.map((c) => L.codecLabel(codecs, c)).join(" lub ")}` : "";
    return { mp3: L.mp3Equivalent(codecs, id, kbps), advice, text: codecs?.recommendation_text || "" };
  };

  // The spec §8 table as rows: { aac: "HE-AAC 48–64 kb/s", mp3: "~128 kb/s" }.
  L.equivalentRows = (codecs) => (codecs?.mp3_equivalents || []).map((e) => {
    const range = e.max_kbps == null ? `${e.min_kbps}+` : e.min_kbps === e.max_kbps ? `${e.min_kbps}` : `${e.min_kbps}–${e.max_kbps}`;
    return { aac: `${L.codecLabel(codecs, e.codec)} ${range} kb/s`, mp3: e.mp3 };
  });

  // "brak biblioteki libfdk-aac obok programu" from "kodek AAC-LC niedostępny: brak biblioteki ...".
  L.reasonDetail = (reason) => {
    const i = String(reason || "").indexOf(": ");
    return i >= 0 ? reason.slice(i + 2) : String(reason || "");
  };

  // ---------------------------------------------------------------- SDP
  L.sdpInfo = (sdp) => {
    if (!sdp || !String(sdp).trim()) return null;
    const one = (re) => (sdp.match(re) || [])[1];
    const media = sdp.match(/^m=audio (\d+) /m);
    const rtpmap = sdp.match(/^a=rtpmap:\d+ ([A-Za-z0-9-]+)\/(\d+)(?:\/(\d+))?/m);
    const ptime = one(/^a=ptime:([0-9.]+)/m);
    return {
      name: (one(/^s=(.*)$/m) || "").trim(),
      address: one(/^c=IN IP4 ([0-9.]+)/m) || "",
      port: media ? Number(media[1]) : 0,
      encoding: rtpmap ? rtpmap[1] : "",
      rate: rtpmap ? Number(rtpmap[2]) : 0,
      channels: rtpmap ? Number(rtpmap[3] || 1) : 0,
      ptime: ptime ? Number(ptime) : 0,
    };
  };

  // "239.69.1.10:5004 · L24 · 48 kHz · stereo · 1 ms"
  L.sdpSummary = (info) => {
    if (!info) return "";
    const parts = [];
    if (info.address) parts.push(info.port ? `${info.address}:${info.port}` : info.address);
    if (info.encoding) parts.push(info.encoding);
    if (info.rate) parts.push(L.fmtRate(info.rate));
    if (info.channels) parts.push(info.channels === 1 ? "mono" : info.channels === 2 ? "stereo" : `${info.channels} kan.`);
    if (info.ptime) parts.push(`${info.ptime.toLocaleString("pl-PL", { maximumFractionDigits: 3 })} ms`);
    return parts.join(" · ");
  };

  // ---------------------------------------------------------------- validation issues (400)
  // The server names fields of the whole configuration ("stations[2].streams[0].port"). For the
  // station at `index` they become relative ("streams[0].port"); everything else is general.
  L.mapIssues = (issues, index) => {
    const out = { fields: {}, general: [] };
    const prefix = index == null || index < 0 ? null : `stations[${index}].`;
    for (const x of issues || []) {
      const f = String(x.field || "");
      if (prefix && f.startsWith(prefix)) (out.fields[f.slice(prefix.length)] ||= []).push(x.message);
      else out.general.push(x.message);
    }
    return out;
  };

  // ---------------------------------------------------------------- stream passwords (rule E7)
  // The stored password is kept only when the stream id, host and port are unchanged.
  L.serverChanged = (saved, s) => !!saved &&
    (String(saved.host || "").toLowerCase() !== String(s.host || "").toLowerCase() || Number(saved.port) !== Number(s.port));

  // A stored password that will not be carried over: the user has to type it again.
  L.needsPasswordAgain = (saved, s) => !!saved?.password_set && !s.password && L.serverChanged(saved, s);

  // ---------------------------------------------------------------- the station editor
  // After /api/config was reloaded: what happens to the editor of station `editId`. Unsaved edits
  // and a new station are kept; a clean editor is reopened when its station changed, closed when
  // the station is gone, and otherwise left alone.
  L.editorReload = (editId, dirty, stations, saved) => {
    if (!editId || editId === "new" || dirty) return "keep";
    const fresh = (stations || []).find((s) => s.id === editId);
    if (!fresh) return "close";
    return JSON.stringify(fresh) !== JSON.stringify(saved) ? "reopen" : "keep";
  };

  // After a new login: the open draft stays when its station still exists (or it is a new one).
  L.keepDraft = (editId, stations) => editId === "new" || (!!editId && (stations || []).some((s) => s.id === editId));

  // The draft as compared for "unsaved changes" (what would be sent).
  L.draftJson = (station) => JSON.stringify(L.stationBody(station));

  // The saved station differs from the one the draft was opened from (another panel changed or
  // deleted it).
  L.changedElsewhere = (saved, current) => !!saved && JSON.stringify(saved) !== JSON.stringify(current);

  // Saving this draft renames a running station that has enabled streams: the station name is
  // each stream's Ice-Name, so every one of its streams reconnects (a short break on air).
  L.renameRestartsStreams = (saved, draft) => !!saved && !!draft && saved.enabled && draft.enabled &&
    saved.name !== draft.name && (draft.streams || []).some((s) => s.enabled);

  // The station as the API takes it: panel-only keys (leading "_") and password_set left out.
  // license_key_set is the server's answer only; a typed key ("" = keep) and license_key_clear go through.
  L.stationBody = (station) => JSON.parse(JSON.stringify(station, (k, v) =>
    (k.startsWith("_") || k === "password_set" || k === "license_key_set" ? undefined : v)));

  // ---------------------------------------------------------------- new items
  L.newInput = () => ({ enabled: true, name: "", sdp: "", gain_db: 0, delay_s: 0, latency_us: 5000 });

  L.newStream = (codecs) => {
    const available = (codecs?.codecs || []).filter((c) => c.available);
    const codec = (available.find((c) => c.id === "mp3") || available[0] || { id: "mp3" }).id;
    return L.adjustStream(codecs, {
      enabled: true, codec, bitrate_kbps: 128, sample_rate: 44100, gain_db: 0,
      host: "", port: 8000, mount: "", user: "source", password: "",
    });
  };

  // web_port_slot -1: the server gives a new station the lowest free slot.
  L.newProcessing = () => ({ enabled: false, license_key: "", preset: "", web_port_slot: -1 });

  L.newStation = () => ({ name: "", enabled: true, inputs: [L.newInput()], streams: [], processing: L.newProcessing() });

  // ---------------------------------------------------------------- live states (/api/live)
  // { kind: "" | "on" | "warn" | "err", text }: the dot colour and the Polish state (a stream on air
  // also carries its kbps, for the "NA ANTENIE" lamp).
  L.stationState = (st) => {
    if (!st) return { kind: "", text: "—" };
    if (!st.enabled) return { kind: "", text: "wyłączona" };
    if (st.running) return { kind: "on", text: `działa · ${L.fmtUptime(st.uptime_s)}` };
    return { kind: "err", text: "nie działa" };
  };

  L.inputState = (inp, stationOn) => {
    if (!inp) return { kind: "", text: "—" };
    if (!stationOn) return { kind: "", text: "stacja wyłączona" };
    if (!inp.enabled) return { kind: "", text: "wyłączone" };
    if (inp.error) return { kind: "err", text: inp.error };
    if (inp.signal) return { kind: "on", text: "sygnał" };
    return { kind: "err", text: "brak sygnału" };
  };

  L.streamState = (str, stationOn) => {
    if (!str) return { kind: "", text: "—" };
    if (!stationOn) return { kind: "", text: "stacja wyłączona" };
    if (!str.enabled) return { kind: "", text: "wyłączony" };
    if (!str.codec_available) return { kind: "err", text: "kodek niedostępny" };
    if (str.encoder_failed) return { kind: "err", text: `błąd kodera: ${str.encoder_error}` };
    switch (str.state) {
      case "streaming": return { kind: "on", text: `nadaje · ${L.fmtKbps(str.kbps)}`, kbps: str.kbps };
      case "connecting": return { kind: "warn", text: "łączenie…" };
      case "error": return { kind: "err", text: str.error || "błąd połączenia" };
      default: return { kind: "", text: "czeka" };
    }
  };

  // What the station list is built from, the same for /api/config stations and /api/live
  // stations: when they differ (a change from another tab or computer) the panel reloads.
  L.stationsShape = (stations) => JSON.stringify((stations || []).map((s) =>
    [s.id, s.name, s.enabled, (s.inputs || []).length, (s.streams || []).map((x) => [x.id, x.enabled, x.codec, x.host, x.port, x.mount]),
      !!s.processing?.enabled, s.processing?.preset || ""]));

  // ---------------------------------------------------------------- Stereo Tool processing
  const BLANKS = /^[ \t\r\n]+|[ \t\r\n]+$/g;  // the server's trim_blanks

  // Why a typed licence key has the wrong shape (key_shape_error of the server), "" when it is fine
  // or blank (blank = keep the stored key). Never quotes the key.
  L.keyShapeError = (text) => {
    const key = String(text ?? "").replace(BLANKS, "");
    if (!key) return "";
    if (key.length > L.LIMITS.keyMaxChars) return `może mieć najwyżej ${L.LIMITS.keyMaxChars} znaków`;
    if (!/^[\x20-\x7e]*$/.test(key)) return "może zawierać tylko drukowalne znaki ASCII (bez polskich liter i znaków sterujących)";
    if (key.length < 2 || key[0] !== "<" || key.at(-1) !== ">") return "musi zaczynać się znakiem „<” i kończyć znakiem „>” (wklej cały klucz z nawiasami)";
    return "";
  };

  const DEVICE = /^(con|prn|aux|nul|com[0-9]|lpt[0-9])$/i;
  const deviceStem = (name) => DEVICE.test(name.slice(0, (name.indexOf(".") + 1 || name.length + 1) - 1).replace(BLANKS, ""));

  // valid_preset_name of the server: letters, digits, space, "-", "_", ".", no leading space, ".sts",
  // at most 64 characters, a letter or digit before the extension, no Windows device name.
  L.presetNameError = (name) => {
    const n = String(name ?? "");
    const rule = `litery, cyfry, spacja, „-”, „_” i „.”, bez spacji na początku, z rozszerzeniem .sts, najwyżej ${L.LIMITS.presetNameMaxChars} znaki`;
    if (n.length <= 4 || n.length > L.LIMITS.presetNameMaxChars || n[0] === " " || !/^[A-Za-z0-9 _.-]+$/.test(n) ||
      n.slice(-4).toLowerCase() !== ".sts" || !/[A-Za-z0-9]/.test(n.slice(0, -4))) return `nieprawidłowa nazwa presetu (${rule})`;
    if (deviceStem(n)) return "nazwa presetu zarezerwowana przez Windows (CON, PRN, AUX, NUL, COM0–9, LPT0–9)";
    return "";
  };

  // A preset name typed without the extension gets ".sts".
  L.withSts = (name) => {
    const n = String(name ?? "").replace(BLANKS, "");
    return !n || n.toLowerCase().endsWith(".sts") ? n : `${n}.sts`;
  };

  // The name an uploaded file is stored under: its base name with Polish (and other Latin) letters
  // plain, every other character the server refuses as "_", ".sts" added, cut to 64 characters.
  L.presetUploadName = (fileName) => {
    let n = String(fileName ?? "").split(/[\\/]/).pop()
      .replace(/ł/g, "l").replace(/Ł/g, "L").normalize("NFD").replace(/[\u0300-\u036f]/g, "")
      .replace(/[^A-Za-z0-9 _.-]/g, "_").replace(/^ +/, "");
    let base = n.toLowerCase().endsWith(".sts") ? n.slice(0, -4) : n;
    const ext = n.toLowerCase().endsWith(".sts") ? n.slice(-4) : ".sts";
    if (deviceStem(base + ext)) {
      const dot = base.indexOf(".");
      base = dot < 0 ? `${base}_` : `${base.slice(0, dot)}_${base.slice(dot)}`;
    }
    base = base.slice(0, L.LIMITS.presetNameMaxChars - ext.length);
    if (!/[A-Za-z0-9]/.test(base)) base = "preset";
    return base + ext;
  };

  // The preset select: "" first, every uploaded preset, and the station's preset when it is not
  // uploaded (kept selectable, so opening the editor never changes the station).
  L.presetChoices = (presets, current) => {
    const cur = String(current || "");
    const found = (presets || []).find((p) => p.name.toLowerCase() === cur.toLowerCase());
    const out = [{ value: "", label: "— wybierz preset —", selected: !cur }];
    for (const p of presets || []) out.push({ value: p.name, label: p.name, selected: !!cur && p === found });
    if (cur && !found) out.push({ value: cur, label: `${cur} (nie ma go w folderze presetów)`, selected: true, missing: true });
    return out;
  };

  // The processing of a station (/api/live stations[].processing) as { kind, text, title }: a short
  // text for the rows, the full reason of the service in the title.
  L.processingState = (p, stationOn) => {
    if (!p || !p.enabled) return { kind: "", text: "wyłączony" };
    if (!stationOn) return { kind: "", text: "stacja wyłączona" };
    const title = p.reason || "";
    if (p.state === "running")
      return { kind: "on", text: `działa · CPU ${L.fmtPct(p.cpu_pct)}`, title: `działa · procesor ${L.fmtPct(p.cpu_pct)} · opóźnienie ${L.fmtMs(p.latency_ms)}` };
    if (p.state === "bypass" || p.state === "error") {
      const warn = (text) => ({ kind: "warn", text, title: title || text });
      const err = (text) => ({ kind: "err", text, title: title || text });
      switch (p.reason_code) {
        case "starting": return warn("uruchamianie…");
        case "licence_check": return warn("sprawdzanie licencji…");
        case "saving": return warn("zapis presetu…");
        case "restart_waits": return warn("czeka na Stereo Tool…");
        case "too_slow": return warn(p.retry_in_s >= 0 ? `nie nadąża · ponowna próba za ${L.fmtWait(p.retry_in_s)}` : "nie nadąża");
        case "licence": {
          const s = err(p.unlicensed_features ? `licencja: brak ${p.unlicensed_features}` : "licencja nie obejmuje ustawień");
          if (p.unlicensed_features) s.title = `${s.title}: ${p.unlicensed_features}`;
          return s;
        }
        case "crash": return err("awaria: uruchom ponownie usługę");
        case "process": return err("błąd Stereo Tool");
        case "setup": return err(p.reason || "błąd uruchomienia Stereo Tool");
        default: return err(p.reason || "bez processingu");
      }
    }
    return { kind: "err", text: p.reason || "nie działa", title };
  };

  // The licence warning of a station whose licence does not cover its settings, "" otherwise
  // (also while the licence has not been checked yet).
  L.licenceWarning = (p) => {
    if (!p?.enabled || !(p.reason_code === "licence" || (p.license_valid === false && p.unlicensed_features))) return "";
    const what = p.unlicensed_features ? `nie obejmuje funkcji: ${p.unlicensed_features}` : "nie obejmuje funkcji używanych w ustawieniach";
    return `Licencja Stereo Tool ${what}. Stacja nadaje sygnał bez processingu: wyłącz te funkcje w presecie (interfejs WWW) albo użyj klucza z odpowiednią licencją.`;
  };

  // The Stereo Tool web interface of a station while its instance is up, "" otherwise. Built from
  // the port only: it is always on 127.0.0.1 (the service binds it there).
  L.webUiUrl = (p) => {
    const port = Number(p?.web_port);
    if (!Number.isInteger(port) || port <= 0 || port > 65535) return "";
    if (p.state !== "running" && p.state !== "bypass") return "";
    return `http://127.0.0.1:${port}/`;
  };

  // The panel is open on the encoder computer itself (127.0.0.1 then reaches Stereo Tool).
  L.isLocalHost = (hostname) => {
    const h = String(hostname || "").toLowerCase();
    return h === "localhost" || h === "[::1]" || h === "::1" || /^127\.\d+\.\d+\.\d+$/.test(h);
  };
})(typeof module !== "undefined" && module.exports ? module.exports : (window.EncoderLogic = {}));
