// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAES Master web panel. Plain JS, no framework; talks to the REST API and the SSE live feed.
"use strict";

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const DEVICE_COUNT = 32;
const PTIMES = [[1000, "1 ms"], [1333, "1,333 ms"], [4000, "4 ms"]];

const state = {
  config: null,  // last saved configuration (server)
  draft: null,   // edited copy
  interfaces: [],
  live: null,
  issues: [],
  logo: null,    // URL of panel logo file, when present
  source: null,  // EventSource
  browseDevice: 0,
  pasteDevice: 0,
};

// ---------------------------------------------------------------- helpers
async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: "same-origin",
    headers: options.body ? { "Content-Type": "application/json" } : {},
    ...options,
  });
  let data = null;
  const text = await res.text();
  try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  if (res.status === 401 && !path.startsWith("/api/login")) {
    showAuth();
    throw new Error("wymagane logowanie");
  }
  return { ok: res.ok, status: res.status, data };
}

function el(tag, props = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === "class") e.className = v;
    else if (k === "dataset") Object.assign(e.dataset, v);
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k in e) e[k] = v;
    else e.setAttribute(k, v);
  }
  for (const c of children) if (c != null) e.append(c);
  return e;
}

function pad2(n) { return String(n).padStart(2, "0"); }
function clone(o) { return JSON.parse(JSON.stringify(o)); }

function toast(message, kind = "ok") {
  const t = el("div", { class: `toast ${kind}` }, message);
  $("#toasts").append(t);
  setTimeout(() => t.remove(), kind === "err" ? 9000 : 5000);
}

function sdpInfo(sdp) {
  if (!sdp) return null;
  const name = (sdp.match(/^s=(.*)$/m) || [])[1] || "";
  const addr = (sdp.match(/^c=IN IP4 ([0-9.]+)/m) || [])[1] || "";
  return { name: name.trim(), addr };
}

function fmtNumber(n) { return Number(n || 0).toLocaleString("pl-PL"); }

function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? `${d} d ` : "") + `${pad2(h)}:${pad2(m)}:${pad2(Math.floor(s % 60))}`;
}

// ---------------------------------------------------------------- brand / logo
async function detectLogo() {
  for (const name of ["logo.svg", "logo.png"]) {
    try {
      const r = await fetch(name, { method: "HEAD" });
      if (r.ok) return name;
    } catch { /* ignore */ }
  }
  return null;
}

function renderBrand() {
  const tpl = $("#brand-template");
  for (const slot of $$(".brand-slot")) {
    slot.replaceChildren(tpl.content.cloneNode(true));
    if (state.logo) {
      const brand = $(".brand", slot);
      brand.classList.add("has-logo");
      brand.prepend(el("img", { class: "brand-logo", src: state.logo, alt: "bounce" }));
    }
  }
}

// ---------------------------------------------------------------- auth
async function showAuth() {
  if (state.source) { state.source.close(); state.source = null; }
  $("#app").classList.add("hidden");
  $("#auth").classList.remove("hidden");
  const { data } = await api("/api/auth-state");
  $("#login-form").classList.toggle("hidden", !data.password_set);
  $("#setup-form").classList.toggle("hidden", data.password_set || !data.local);
  $("#setup-remote").classList.toggle("hidden", data.password_set || data.local);
  if (data.password_set) $("#login-password").focus();
}

$("#login-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  $("#login-error").textContent = "";
  const r = await api("/api/login", { method: "POST", body: JSON.stringify({ password: $("#login-password").value }) });
  if (!r.ok) { $("#login-error").textContent = r.data?.error || "Błąd logowania"; return; }
  $("#login-password").value = "";
  boot();
});

$("#setup-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const a = $("#setup-password").value, b = $("#setup-password2").value;
  if (a !== b) { $("#setup-error").textContent = "Hasła się różnią"; return; }
  const r = await api("/api/setup", { method: "POST", body: JSON.stringify({ password: a }) });
  if (!r.ok) { $("#setup-error").textContent = r.data?.error || "Błąd"; return; }
  boot();
});

$("#logout").addEventListener("click", async () => {
  await api("/api/logout", { method: "POST" });
  showAuth();
});

// ---------------------------------------------------------------- tabs
function showTab() {
  const tab = (location.hash || "#config").slice(1);
  const valid = ["config", "stats", "logs"].includes(tab) ? tab : "config";
  for (const a of $$(".tabs a")) a.classList.toggle("active", a.dataset.tab === valid);
  for (const s of $$(".tab")) s.classList.toggle("hidden", s.id !== `tab-${valid}`);
  if (valid === "logs") loadLogs();
}
window.addEventListener("hashchange", showTab);

// ---------------------------------------------------------------- config: global
function levelHint(v) {
  const gain = v + 20;
  if (gain === 0) return "−20 dBFS = 0 dB: sygnał bez zmian (bit w bit)";
  return `do aplikacji ${gain > 0 ? "+" : "−"}${Math.abs(gain)} dB, do sieci ${gain > 0 ? "−" : "+"}${Math.abs(gain)} dB`;
}

function renderGlobal() {
  const d = state.draft;
  const sel = $("#g-iface");
  sel.replaceChildren(el("option", { value: "" }, "Automatycznie (pierwsza aktywna)"));
  const known = new Set();
  for (const i of state.interfaces) {
    known.add(i.ipv4);
    sel.append(el("option", { value: i.ipv4 }, `${i.name} — ${i.ipv4}${i.up ? "" : " (nieaktywna)"}`));
  }
  if (d.interface_ip && !known.has(d.interface_ip)) sel.append(el("option", { value: d.interface_ip }, `${d.interface_ip} (niedostępna)`));
  sel.value = d.interface_ip;
  $("#g-domain").value = d.ptp_domain;
  $("#g-level").value = d.pc_nominal_level_dbfs;
  $("#g-level-out").textContent = `${d.pc_nominal_level_dbfs} dBFS`;
  $("#g-level-hint").textContent = levelHint(d.pc_nominal_level_dbfs);
  $("#g-port").value = d.http_port;
}

$("#g-iface").addEventListener("change", (e) => { state.draft.interface_ip = e.target.value; markDirty(); });
$("#g-domain").addEventListener("input", (e) => { state.draft.ptp_domain = Number(e.target.value); markDirty(); });
$("#g-port").addEventListener("input", (e) => { state.draft.http_port = Number(e.target.value); markDirty(); });
$("#g-level").addEventListener("input", (e) => {
  const v = Number(e.target.value);
  state.draft.pc_nominal_level_dbfs = v;
  $("#g-level-out").textContent = `${v} dBFS`;
  $("#g-level-hint").textContent = levelHint(v);
  markDirty();
});

// ---------------------------------------------------------------- config: devices
function meterCell(index) {
  const m = (cap, key) => el("div", { class: "meter" },
    el("span", { dataset: { meter: `${index}-${key}-0` } }),
    el("span", { dataset: { meter: `${index}-${key}-1` } }),
    el("div", { class: "meter-caption" }, cap));
  return el("div", { class: "meters" }, m("WE", "in"), m("WY", "out"));
}

function sourceCell(dev, i) {
  const info = sdpInfo(dev.rx_sdp);
  const label = info
    ? el("span", { class: "label", title: info.addr }, info.name || "(bez nazwy)", el("small", {}, info.addr))
    : el("span", { class: "label none" }, "brak");
  return el("div", { class: "source" },
    el("span", { class: "dot", dataset: { sig: i } }),
    label,
    el("button", { type: "button", class: "mini", onclick: () => openBrowse(i) }, "Przeglądaj…"),
    el("button", { type: "button", class: "mini ghost", onclick: () => openPaste(i), title: "Wklej SDP" }, "SDP…"),
    info ? el("button", { type: "button", class: "mini ghost", title: "Usuń źródło", onclick: () => { state.draft.devices[i].rx_sdp = ""; markDirty(); renderDevices(); } }, "✕") : null);
}

function renderDevices() {
  const tbody = $("#device-rows");
  const onlyActive = $("#only-active").checked;
  tbody.replaceChildren();
  state.draft.devices.forEach((dev, i) => {
    if (onlyActive && !dev.enabled) return;
    const n = i + 1;
    const set = (key, value) => { dev[key] = value; markDirty(); };
    const ptime = el("select", { onchange: (e) => set("tx_ptime_us", Number(e.target.value)) },
      ...PTIMES.map(([v, t]) => el("option", { value: v, selected: dev.tx_ptime_us === v }, t)));
    const row = el("tr", { class: dev.enabled ? "" : "off", dataset: { device: n } },
      el("td", { class: "c-num" }, pad2(n)),
      el("td", {}, el("input", { type: "checkbox", checked: dev.enabled, onchange: (e) => { set("enabled", e.target.checked); row.classList.toggle("off", !e.target.checked); } })),
      el("td", {}, el("input", { type: "text", value: dev.name, maxLength: 40, dataset: { field: "name" }, oninput: (e) => set("name", e.target.value) })),
      el("td", {},
        el("input", { type: "text", class: "addr", value: dev.tx_address, dataset: { field: "tx_address" }, oninput: (e) => set("tx_address", e.target.value.trim()) }),
        " ",
        el("button", { type: "button", class: "mini ghost", title: "Pokaż SDP wyjścia", onclick: () => openSdp(n) }, "SDP")),
      el("td", {}, ptime),
      el("td", {}, el("input", { type: "checkbox", checked: dev.keep_active, onchange: (e) => set("keep_active", e.target.checked) })),
      el("td", {}, sourceCell(dev, i)),
      el("td", {}, el("input", { type: "number", class: "lat", min: 1, max: 1000, step: 0.5, value: dev.rx_latency_us / 1000, dataset: { field: "rx_latency_us" },
        oninput: (e) => set("rx_latency_us", Math.round(Number(e.target.value) * 1000)) })),
      el("td", {}, meterCell(n)));
    tbody.append(row);
    const issues = state.issues.filter((x) => x.device === n);
    if (issues.length) {
      for (const x of issues) $(`[data-field="${x.field}"]`, row)?.classList.add("invalid");
      tbody.append(el("tr", { class: "row-error" }, el("td", { colSpan: 9 },
        el("p", { class: "field-error" }, issues.map((x) => x.message).join(" · ")))));
    }
  });
  const global = state.issues.filter((x) => x.device === 0).map((x) => x.message).join(" · ");
  $('[data-issue="0"]').textContent = global;
  applyLive();
}
$("#only-active").addEventListener("change", renderDevices);

function markDirty() {
  const dirty = JSON.stringify(state.draft) !== JSON.stringify(state.config);
  $("#savebar").classList.toggle("hidden", !dirty);
}

$("#discard").addEventListener("click", () => {
  state.draft = clone(state.config);
  state.issues = [];
  renderGlobal();
  renderDevices();
  markDirty();
});

$("#save").addEventListener("click", async () => {
  const body = clone(state.draft);
  const r = await api("/api/config", { method: "PUT", body: JSON.stringify(body) });
  state.issues = r.data?.issues || [];
  if (!r.ok) {
    toast(r.data?.error || "Konfiguracja zawiera błędy — popraw zaznaczone pola", "err");
    renderGlobal();
    renderDevices();
    return;
  }
  state.config = r.data.config;
  state.draft = clone(state.config);
  for (const n of r.data.notices || []) toast(n, "warn");
  toast("Zapisano i zastosowano konfigurację");
  renderGlobal();
  renderDevices();
  markDirty();
});

// ---------------------------------------------------------------- dialogs
async function openBrowse(i) {
  state.browseDevice = i;
  $("#browse-target").textContent = `→ ${state.draft.devices[i].name}`;
  const dlg = $("#dlg-browse");
  await fillBrowse();
  dlg.showModal();
  const timer = setInterval(() => dlg.open ? fillBrowse() : clearInterval(timer), 4000);
}

async function fillBrowse() {
  const { data } = await api("/api/streams");
  const tbody = $("#browse-rows");
  tbody.replaceChildren();
  if (!data.length) {
    tbody.append(el("tr", {}, el("td", { colSpan: 5, class: "hint" }, "Brak ogłoszeń SAP w sieci. Możesz wkleić SDP ręcznie (przycisk „SDP…”).")));
    return;
  }
  for (const s of data) {
    tbody.append(el("tr", {},
      el("td", {}, s.name),
      el("td", { class: "mono" }, s.origin),
      el("td", { class: "mono" }, `${s.address}:${s.port}`),
      el("td", {}, `${s.encoding} · ${s.channels} kan. · ${String(s.ptime_ms).replace(".", ",")} ms`),
      el("td", {}, el("button", { type: "button", class: "mini primary", onclick: () => {
        state.draft.devices[state.browseDevice].rx_sdp = s.sdp;
        $("#dlg-browse").close();
        markDirty();
        renderDevices();
      } }, "Wybierz"))));
  }
}

function openPaste(i) {
  state.pasteDevice = i;
  $("#paste-target").textContent = `→ ${state.draft.devices[i].name}`;
  $("#paste-sdp").value = state.draft.devices[i].rx_sdp || "";
  $("#dlg-paste").showModal();
}

$("#dlg-paste").addEventListener("close", () => {
  if ($("#dlg-paste").returnValue !== "ok") return;
  state.draft.devices[state.pasteDevice].rx_sdp = $("#paste-sdp").value.trim();
  markDirty();
  renderDevices();
});

async function openSdp(n) {
  $("#sdp-target").textContent = `${pad2(n)} ${state.config.devices[n - 1].name}`;
  const res = await fetch(`/api/devices/${n}/sdp`, { credentials: "same-origin" });
  $("#sdp-text").value = await res.text();
  if (JSON.stringify(state.draft.devices[n - 1]) !== JSON.stringify(state.config.devices[n - 1]))
    toast("SDP pokazuje zapisaną konfigurację — zapisz zmiany, aby je uwzględnić", "warn");
  $("#dlg-sdp").showModal();
}

$("#sdp-copy").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText($("#sdp-text").value);
    toast("Skopiowano SDP");
  } catch {
    $("#sdp-text").select();
    document.execCommand("copy");
  }
});

$("#change-password").addEventListener("click", () => {
  for (const id of ["#pw-current", "#pw-new", "#pw-new2"]) $(id).value = "";
  $("#pw-error").textContent = "";
  $("#dlg-password").showModal();
});

$("#pw-save").addEventListener("click", async () => {
  if ($("#pw-new").value !== $("#pw-new2").value) { $("#pw-error").textContent = "Hasła się różnią"; return; }
  const r = await api("/api/password", { method: "POST", body: JSON.stringify({ current: $("#pw-current").value, password: $("#pw-new").value }) });
  if (!r.ok) { $("#pw-error").textContent = r.data?.error || "Błąd"; return; }
  $("#dlg-password").close();
  toast("Hasło zmienione");
});

// ---------------------------------------------------------------- live feed
const PTP_LABELS = {
  locked: ["zsynchronizowany", "ok"],
  holdover: ["holdover", "warn"],
  listening: ["synchronizacja…", "warn"],
  freerun: ["brak mastera", "err"],
  disabled: ["wyłączony", "err"],
};

function setPill(id, text, kind) {
  const p = $(id);
  p.classList.remove("ok", "warn", "err");
  if (kind) p.classList.add(kind);
  $("span", p).textContent = text;
}

function meterValue(db) { return Math.max(0, Math.min(1, (db + 60) / 60)); }

function applyLive() {
  const live = state.live;
  if (!live) return;
  const [ptpText, ptpKind] = PTP_LABELS[live.ptp.state] || ["?", "err"];
  setPill("#pill-ptp", ptpText, ptpKind);
  setPill("#pill-asio", live.asio.connected ? `połączono · ${live.asio.buffer_frames} próbek` : "brak aplikacji", live.asio.connected ? "ok" : "warn");

  const byIndex = new Map(live.devices.map((d) => [d.i, d]));
  for (const span of $$("[data-meter]")) {
    const [i, key, ch] = span.dataset.meter.split("-");
    const d = byIndex.get(Number(i));
    span.style.setProperty("--v", d ? meterValue(d[key][Number(ch)]) : 0);
  }
  for (const dot of $$("[data-sig]")) {
    const n = Number(dot.dataset.sig) + 1;
    const d = byIndex.get(n);
    const configured = !!state.config?.devices[n - 1]?.rx_sdp;
    dot.className = "dot" + (d?.sig ? " on" : configured && d ? " err" : "");
    dot.title = d?.sig ? "sygnał odbierany" : configured ? "brak sygnału" : "";
  }
  renderStats();
}

function renderStats() {
  const live = state.live;
  if (!live || $("#tab-stats").classList.contains("hidden")) return;
  const p = live.ptp;
  const kv = (dl, rows) => dl.replaceChildren(...rows.flatMap(([k, v]) => [el("dt", {}, k), el("dd", {}, String(v))]));
  kv($("#ptp-kv"), [
    ["Stan", (PTP_LABELS[p.state] || ["?"])[0]],
    ["Domena", p.domain],
    ["Grandmaster", p.grandmaster || "—"],
    ["Master", p.master || "—"],
    ["Odchyłka", `${p.offset_us} µs`],
    ["Opóźnienie ścieżki", `${p.path_delay_us} µs`],
    ["Korekta częstotliwości", `${p.rate_ppm} ppm`],
    ["Synchronization Lost", p.sync_lost],
    ["Korekty skokowe", p.steps],
  ]);
  kv($("#svc-kv"), [
    ["Czas działania", fmtUptime(live.uptime_s)],
    ["Aplikacja ASIO", live.asio.connected ? "połączona" : "brak"],
    ["Bufor ASIO", live.asio.connected ? `${live.asio.buffer_frames} próbek (${(live.asio.buffer_frames / 48).toFixed(2).replace(".", ",")} ms)` : "—"],
    ["Karta sieciowa", state.status?.interface_ip || "—"],
    ["Wersja", state.status?.version || "—"],
  ]);
  const tbody = $("#stats-rows");
  tbody.replaceChildren();
  for (const d of live.devices) {
    const c = d.c;
    const name = state.config?.devices[d.i - 1]?.name || "";
    const cell = (v, bad) => el("td", { class: bad ? "bad" : "" }, fmtNumber(v));
    tbody.append(el("tr", {},
      el("td", { class: "c-num" }, pad2(d.i)),
      el("td", {}, name),
      cell(c.tx_packets), cell(c.tx_errors, c.tx_errors > 0),
      cell(c.rx_packets), cell(c.rx_bytes),
      cell(c.rx_underruns, c.rx_underruns > 0), cell(c.rx_overruns, c.rx_overruns > 0), cell(c.rx_seq_errors, c.rx_seq_errors > 0),
      el("td", {}, c.rx_jitter_ms.toFixed(3).replace(".", ",")),
      el("td", {}, el("span", { class: "dot" + (d.sig ? " on" : "") }))));
  }
}

$("#reset-stats").addEventListener("click", async () => {
  await api("/api/stats/reset", { method: "POST" });
  toast("Liczniki wyzerowane");
});

function connectLive() {
  if (state.source) state.source.close();
  const src = new EventSource("/api/live");
  src.addEventListener("live", (e) => {
    state.live = JSON.parse(e.data);
    applyLive();
  });
  src.onerror = () => setPill("#pill-ptp", "brak połączenia z usługą", "err");
  state.source = src;
}

// ---------------------------------------------------------------- logs
const LEVELS = { info: "INFO", warning: "OSTRZEŻENIE", error: "BŁĄD" };
async function loadLogs() {
  const { data } = await api("/api/logs");
  $("#log-rows").replaceChildren(...data.slice().reverse().map((e) => el("tr", { class: e.level },
    el("td", { class: "time" }, new Date(e.time_ms).toLocaleString("pl-PL")),
    el("td", { class: "level" }, LEVELS[e.level] || e.level),
    el("td", {}, e.message))));
}
$("#refresh-logs").addEventListener("click", loadLogs);

// ---------------------------------------------------------------- boot
async function boot() {
  $("#auth").classList.add("hidden");
  $("#app").classList.remove("hidden");
  const [cfg, ifs, st] = await Promise.all([api("/api/config"), api("/api/interfaces"), api("/api/status")]);
  state.config = cfg.data;
  state.draft = clone(cfg.data);
  state.interfaces = ifs.data;
  state.status = st.data;
  state.issues = [];
  setPill("#pill-iface", st.data.interface_ip || "brak", null);
  renderGlobal();
  renderDevices();
  markDirty();
  showTab();
  connectLive();
}

(async function init() {
  state.logo = await detectLogo();
  renderBrand();
  const { data } = await api("/api/auth-state");
  if (data.logged_in) boot();
  else showAuth();
})();
