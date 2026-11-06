// SPDX-License-Identifier: GPL-3.0-or-later
// Bounce Encoder web panel. Plain JS, no framework; talks to the REST API (task 8) and the SSE
// status feed /api/live. Pure helpers (formatting, codec choices, hints) live in logic.js.
"use strict";

const L = window.EncoderLogic;
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const TABS = ["stations", "edit", "stats", "logs"];
const LIM = L.LIMITS;

const state = {
  config: null,       // last configuration from the server (API form, passwords "")
  codecs: null,       // /api/codecs
  live: null,         // last /api/live status document
  logo: null,         // URL of the panel logo file, when present
  source: null,       // EventSource of /api/live
  liveRetry: null,    // timer of the next /api/live reconnect
  reloadedFor: "",    // the /api/live station shape that last triggered a /api/config reload
  // the station editor
  editId: null,       // null (not opened yet), "new", or a station id
  saved: null,        // the station as saved (null for a new one)
  draft: null,        // the edited copy
  original: "",       // JSON of the draft when it was opened (dirty check)
  issues: { fields: {}, general: [] },
  issuesUsed: new Set(),
  saving: false,
};

// ---------------------------------------------------------------- helpers
// A 401: the login screen is already shown, the caller just stops (unsaved drafts stay).
class AuthRequired extends Error {}
window.addEventListener("unhandledrejection", (e) => { if (e.reason instanceof AuthRequired) e.preventDefault(); });

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
    throw new AuthRequired("wymagane logowanie");
  }
  return { ok: res.ok, status: res.status, data };
}

function el(tag, props = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null) continue;
    if (k === "class") e.className = v;
    else if (k === "dataset") Object.assign(e.dataset, v);
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k in e) e[k] = v;
    else e.setAttribute(k, v);
  }
  for (const c of children) if (c != null && c !== false) e.append(c);
  return e;
}

function clone(o) { return JSON.parse(JSON.stringify(o)); }

function toast(message, kind = "ok") {
  const t = el("div", { class: `toast ${kind}` }, message);
  $("#toasts").append(t);
  setTimeout(() => t.remove(), kind === "err" ? 9000 : 5000);
}

function issuesText(r, fallback) {
  const list = r.data?.issues || [];
  return list.length ? list.map((x) => x.message).join(" · ") : (r.data?.error || fallback);
}

function meterValue(db) { return Math.max(0, Math.min(1, (db + 60) / 60)); }

// A stereo LED meter: two segmented bars, updated through data-meter="<key>" and the side (0, 1).
function meter(key) {
  return el("div", { class: "meter stereo", dataset: { meter: key } }, el("span"), el("span"));
}

// The dBFS scale under a meter (positions in common.css).
function meterScale() {
  return el("span", { class: "meter-scale wide", "aria-hidden": "true" }, el("i", {}, "−40"), el("i", {}, "−20"), el("i", {}, "−10"), el("i", {}, "0"));
}

// Each bar: the level (--v), the peak of the last HOLD_MS (--p) and clipping above -1 dBFS (.clip), both held.
const HOLD_MS = 1500;
function setMeter(box, peaks) {
  const spans = box.children;
  const now = performance.now();
  for (let side = 0; side < 2; ++side) {
    const db = peaks ? peaks[side] : -120;
    const bar = spans[side];
    const v = meterValue(db);
    if (!(bar._peak >= v) || now - bar._peakAt > HOLD_MS) { bar._peak = v; bar._peakAt = now; }
    if (db > -1) bar._clipAt = now;
    bar.style.setProperty("--v", v);
    bar.style.setProperty("--p", bar._peak);
    bar.classList.toggle("clip", now - (bar._clipAt ?? -Infinity) < HOLD_MS);
  }
  box.title = peaks ? `L ${L.fmtPeak(peaks[0])} dBFS · P ${L.fmtPeak(peaks[1])} dBFS` : "";
}

// A state: a mark and a text, updated through data-state="<key>". A stream on air (`tally`) shows
// the "NA ANTENIE" lamp and its bitrate.
function stateBox(key) {
  return el("span", { class: "state", dataset: key ? { state: key } : null }, el("span", { class: "dot" }), el("span", { class: "txt" }, "—"));
}

function setState(box, s, tally = false) {
  const onAir = tally && s.kind === "on" && s.kbps != null;
  const [mark, txt] = box.children;
  box.className = `state ${s.kind}`;
  box.title = s.text;  // the whole text, also when the row cuts it short
  mark.className = onAir ? "tally" : `dot ${s.kind}`;
  mark.textContent = onAir ? "NA ANTENIE" : "";
  txt.textContent = onAir ? L.fmtKbps(s.kbps) : s.text;
}

function setPill(id, text, kind, title) {
  const p = $(id);
  p.classList.remove("ok", "warn", "err");
  if (kind) p.classList.add(kind);
  $("span", p).textContent = text;
  if (title) p.title = title;
}

function stationLabel(st, i) { return st.name || `Stacja ${i + 1}`; }

function streamTitle(s) {
  const label = L.codecLabel(state.codecs, s.codec);
  const info = L.codec(state.codecs, s.codec);
  return info?.lossless ? `${label} · ${L.fmtRate(s.sample_rate)}` : `${label} · ${s.bitrate_kbps} kb/s · ${L.fmtRate(s.sample_rate)}`;
}

function streamTarget(s) { return s.host ? `${s.host}:${s.port}${s.mount || ""}` : "serwer nie ustawiony"; }

// Asks before something that cannot be undone; resolves to true on the danger button.
function confirmDialog(title, text, okText = "Usuń") {
  const dlg = $("#dlg-confirm");
  $("#confirm-title").textContent = title;
  $("#confirm-text").textContent = text;
  $("#confirm-ok").textContent = okText;
  dlg.returnValue = "";
  dlg.showModal();
  return new Promise((resolve) => dlg.addEventListener("close", () => resolve(dlg.returnValue === "ok"), { once: true }));
}

// ---------------------------------------------------------------- brand / logo
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
function stopLive() {
  clearTimeout(state.liveRetry);
  state.liveRetry = null;
  if (state.source) { state.source.close(); state.source = null; }
}

// Typed passwords do not stay in the page while it is logged out: the login and setup fields, the
// password dialog and the hidden editor's stream passwords (a draft kept for the next login
// refills its editor then, see boot()).
function clearPasswordFields() {
  for (const p of $$("input[type=password]")) p.value = "";
  if ($("#dlg-password").open) $("#dlg-password").close();
}

async function showAuth() {
  stopLive();
  clearPasswordFields();
  state.config = null;  // boot() loads it again after the login
  state.live = null;
  $("#app").classList.add("hidden");
  $("#auth").classList.remove("hidden");
  const { data } = await api("/api/auth-state");
  $("#login-form").classList.toggle("hidden", !data.password_set);
  $("#setup-form").classList.toggle("hidden", data.password_set || !data.local);
  $("#setup-remote").classList.toggle("hidden", data.password_set || data.local);
  $("#setup-remote-url").textContent = `http://127.0.0.1:${location.port || (location.protocol === "https:" ? 443 : 80)}`;
  if (data.password_set) $("#login-password").focus();
  else if (data.local) $("#setup-password").focus();
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
  $("#setup-error").textContent = "";
  const a = $("#setup-password").value, b = $("#setup-password2").value;
  if (a !== b) { $("#setup-error").textContent = "Hasła się różnią"; return; }
  const r = await api("/api/setup", { method: "POST", body: JSON.stringify({ password: a }) });
  if (!r.ok) { $("#setup-error").textContent = r.data?.error || "Błąd"; return; }
  $("#setup-password").value = $("#setup-password2").value = "";
  boot();
});

$("#logout").addEventListener("click", async () => {
  if (editorDirty() && !(await confirmDialog("Wylogować i odrzucić niezapisane zmiany?",
    `Zmiany w stacji „${state.draft.name || "nowa stacja"}” nie zostały zapisane.`, "Wyloguj"))) return;
  stopLive();
  state.editId = null;
  state.draft = null;
  await api("/api/logout", { method: "POST" }).catch(() => {});
  showAuth();
});

$("#change-password").addEventListener("click", () => {
  for (const id of ["#pw-current", "#pw-new", "#pw-new2"]) $(id).value = "";
  $("#pw-error").textContent = "";
  $("#dlg-password").showModal();
});

// Enter in a dialog field submits its form (the save button); "Anuluj" only closes the dialog.
for (const b of $$("dialog [data-close]")) b.addEventListener("click", () => b.closest("dialog").close());

$("#password-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  if ($("#pw-new").value !== $("#pw-new2").value) { $("#pw-error").textContent = "Hasła się różnią"; return; }
  const r = await api("/api/password", { method: "POST", body: JSON.stringify({ current: $("#pw-current").value, password: $("#pw-new").value }) });
  if (!r.ok) { $("#pw-error").textContent = r.data?.error || "Błąd"; return; }
  $("#dlg-password").close();
  toast("Hasło zmienione; inne otwarte panele trzeba zalogować ponownie");
  connectLive();  // the old feed ended with the old sessions
});

// ---------------------------------------------------------------- tabs
function parseHash() {
  const [tab, id] = (location.hash || "#stations").slice(1).split("/");
  return { tab: TABS.includes(tab) ? tab : "stations", id: id ? decodeURIComponent(id) : null };
}

async function showTab() {
  if (!state.config) return;  // not logged in yet
  const { tab, id } = parseHash();
  if (tab === "edit") {
    const want = id || state.editId || (state.config.stations[0]?.id ?? "new");
    if (want !== state.editId && !(await openEditor(want))) {
      history.replaceState(null, "", editHash(state.editId));  // stays on the unsaved draft
    }
  }
  for (const a of $$(".tabs a")) {
    a.classList.toggle("active", a.dataset.tab === tab);
    a.setAttribute("aria-selected", String(a.dataset.tab === tab));
  }
  for (const s of $$(".tab")) s.classList.toggle("hidden", s.id !== `tab-${tab}`);
  if (tab === "logs") loadLogs();
  updateSavebar();
  applyLive();
}
window.addEventListener("hashchange", showTab);

function editHash(id) { return id && id !== "new" ? `#edit/${encodeURIComponent(id)}` : "#edit/new"; }

// ---------------------------------------------------------------- configuration
async function loadConfig() {
  const r = await api("/api/config");
  if (!r.ok) return;
  state.config = r.data;
  renderStations();
  switch (L.editorReload(state.editId, editorDirty(), state.config.stations, state.saved)) {
    case "reopen": openEditor(state.editId, true); break;
    case "close":  // the station was deleted elsewhere
      state.editId = null;
      state.draft = null;
      if (parseHash().tab === "edit") location.hash = "#stations";
      break;
    default: break;  // unchanged, unsaved edits, or a new station: the editor stays as it is
  }
  renderEditPick();
}

// A change the server accepted: adopt its configuration and show its notices.
function adoptConfig(data) {
  state.config = data.config;
  for (const n of data.notices || []) toast(n, "warn");
  renderStations();
  renderEditPick();
}

// ---------------------------------------------------------------- station list
function renderStations() {
  const list = $("#station-list");
  const stations = state.config.stations;
  $("#station-count").textContent = `${stations.length} z ${LIM.stations}`;
  $("#add-station").disabled = stations.length >= LIM.stations;
  $("#add-station").title = stations.length >= LIM.stations ? `Najwyżej ${LIM.stations} stacji` : "";
  if (!stations.length) {
    list.replaceChildren(el("div", { class: "empty-state" },
      el("p", { class: "hint" }, "Brak stacji. Stacja odbiera do 3 wejść AES67, sumuje je i wysyła do 8 strumieni Icecast."),
      el("button", { type: "button", class: "primary", dataset: { icon: "plus" }, onclick: () => { location.hash = "#edit/new"; } }, "Dodaj pierwszą stację")));
    return;
  }
  list.replaceChildren(...stations.map((st, i) => stationCard(st, i)));
  applyLive();
}

// A station as a strip: its head (switch, name, state, sum meter) over one dense row per input and
// stream, grouped under a side legend.
function stationCard(st, i) {
  const toggle = el("input", { type: "checkbox", checked: st.enabled, "aria-label": `${stationLabel(st, i)}: włączona`,
    onchange: (e) => setStationEnabled(st.id, e.target.checked, e.target) });
  const group = (legend, rows, empty) => el("tbody", {},
    ...(rows.length ? rows : [el("tr", {}, el("td", { colSpan: 6, class: "empty" }, empty))])
      .map((row, k) => { if (k === 0) row.prepend(el("th", { class: "legend", scope: "rowgroup", rowSpan: Math.max(1, rows.length) }, legend)); return row; }));
  const inputs = st.inputs.map((inp, j) => {
    const info = L.sdpInfo(inp.sdp);
    const name = inp.name || info?.name || `Wejście ${j + 1}`;
    const addr = info?.address ? `${info.address}:${info.port}` : "brak SDP";
    const set = `gain ${L.fmtGain(inp.gain_db)} · opóźnienie ${L.fmtDelay(inp.delay_s)}`;
    return el("tr", { class: inp.enabled ? "" : "off" },
      el("td", { class: "c-num" }, L.pad2(j + 1)),
      el("td", { class: "c-name", title: name }, name),
      el("td", { class: "c-addr", title: addr }, addr),
      el("td", { class: "c-set", title: set }, set),
      el("td", { class: "c-meter" }, meter(`in:${st.id}:${j}`)),
      el("td", { class: "c-state" }, stateBox(`in:${st.id}:${j}`)));
  });
  const streams = st.streams.map((s, k) => el("tr", { class: s.enabled ? "" : "off" },
    el("td", { class: "c-num" }, L.pad2(k + 1)),
    el("td", { class: "c-name", title: streamTitle(s) }, streamTitle(s)),
    el("td", { class: "c-addr", title: streamTarget(s) }, streamTarget(s)),
    el("td", { class: "c-set" }, `gain ${L.fmtGain(s.gain_db)}`),
    el("td", { class: "c-meter" }, meter(`str:${st.id}:${s.id}`)),
    el("td", { class: "c-state" }, stateBox(`str:${st.id}:${s.id}`))));
  return el("article", { class: "station" + (st.enabled ? "" : " off") },
    el("div", { class: "station-head" },
      el("label", { class: "toggle", title: "Włącz lub wyłącz stację (od razu)" }, toggle),
      el("div", { class: "station-title" },
        el("h2", {}, stationLabel(st, i)),
        stateBox(`st:${st.id}`)),
      el("div", { class: "station-meter" }, meter(`st:${st.id}`), meterScale()),
      el("span", { class: "peak", dataset: { peak: st.id } }, ""),
      el("div", { class: "head-tools" },
        el("button", { type: "button", class: "ghost", dataset: { icon: "pencil" }, onclick: () => { location.hash = editHash(st.id); } }, "Edytuj"),
        el("button", { type: "button", class: "mini danger", dataset: { icon: "trash" }, onclick: () => deleteStation(st.id) }, "Usuń"))),
    el("div", { class: "table-wrap" }, el("table", { class: "lines" },
      el("colgroup", {}, ...["legend", "num", "name", "addr", "set", "meter", "state"].map((c) => el("col", { class: `k-${c}` }))),
      group(`Wejścia ${st.inputs.length}`, inputs, "Brak wejść: stacja nadaje ciszę."),
      group(`Strumienie ${st.streams.length}`, streams, "Brak strumieni: dodaj je w edycji stacji."))));
}

// The station switch in the list: applied at once (the stored stream passwords stay, E7).
async function setStationEnabled(id, enabled, input) {
  const i = state.config.stations.findIndex((s) => s.id === id);
  if (i < 0) return;
  const st = state.config.stations[i];
  input.disabled = true;
  const body = L.stationBody({ ...st, enabled });
  let r;
  try {
    r = await api(`/api/stations/${encodeURIComponent(id)}`, { method: "PUT", body: JSON.stringify(body) });
  } finally {
    input.disabled = false;
  }
  if (!r.ok) {
    input.checked = !enabled;
    toast(issuesText(r, "Nie można zmienić stacji"), "err");
    return;
  }
  adoptConfig(r.data);
  toast(`Stacja „${stationLabel(st, i)}” ${enabled ? "włączona" : "wyłączona"}`);
  if (state.editId !== id) return;
  if (!editorDirty()) { openEditor(id, true); return; }
  // unsaved edits stay; the switch, already saved, applies to the draft and its saved copy too
  state.saved = { ...state.saved, enabled };
  const original = JSON.parse(state.original);
  original.enabled = enabled;
  state.original = JSON.stringify(original);
  state.draft.enabled = enabled;
  $("#edit-enabled").checked = enabled;
  updateSavebar();
}

async function deleteStation(id) {
  const i = state.config.stations.findIndex((s) => s.id === id);
  if (i < 0) return;
  const st = state.config.stations[i];
  const n = st.streams.length;
  const streams = n ? ` ${n} ${L.plural(n, "strumień", "strumienie", "strumieni")} ${L.plural(n, "rozłączy", "rozłączą", "rozłączy")} się z serwerami.` : "";
  const ok = await confirmDialog(`Usunąć stację „${stationLabel(st, i)}”?`,
    `Stacja zostanie usunięta od razu.${streams} Tego nie można cofnąć.`, "Usuń stację");
  if (!ok) return;
  const r = await api(`/api/stations/${encodeURIComponent(id)}`, { method: "DELETE" });
  if (!r.ok) { toast(issuesText(r, "Nie można usunąć stacji"), "err"); return; }
  adoptConfig(r.data);
  toast(`Usunięto stację „${stationLabel(st, i)}”`);
  if (state.editId === id) {
    state.editId = null;
    state.draft = null;
    if (parseHash().tab === "edit") location.hash = "#stations";
  }
  updateSavebar();
}

$("#add-station").addEventListener("click", () => { location.hash = "#edit/new"; });

// ---------------------------------------------------------------- station editor
function editorDirty() {
  if (!state.draft) return false;
  return L.draftJson(state.draft) !== state.original;  // a new station: dirty once something was typed
}

// Opens a station (or "new") in the editor. With unsaved changes it asks first; false = stayed.
async function openEditor(id, force = false) {
  if (!force && editorDirty() && id !== state.editId) {
    const ok = await confirmDialog("Odrzucić niezapisane zmiany?",
      `Zmiany w stacji „${state.draft.name || "nowa stacja"}” nie zostały zapisane.`, "Odrzuć zmiany");
    if (!ok) return false;
  }
  const saved = id === "new" ? null : state.config.stations.find((s) => s.id === id);
  if (id !== "new" && !saved) { toast(`Brak stacji „${id}”`, "err"); id = "new"; }
  state.editId = id;
  state.saved = saved ? clone(saved) : null;
  state.draft = saved ? clone(saved) : L.newStation();
  state.draft.inputs.forEach((inp, j) => { inp._k = j; });  // the saved input it was (live meters)
  state.original = L.draftJson(state.draft);
  state.issues = { fields: {}, general: [] };
  renderEditor();
  return true;
}

function renderEditPick() {
  const pick = $("#edit-pick");
  const opts = state.config.stations.map((s, i) => el("option", { value: s.id }, stationLabel(s, i)));
  if (state.config.stations.length < LIM.stations) opts.push(el("option", { value: "new" }, "+ nowa stacja"));
  pick.replaceChildren(...opts);
  if (state.editId) pick.value = state.editId;
}

$("#edit-pick").addEventListener("change", (e) => { location.hash = editHash(e.target.value); });

function markDirty() { updateSavebar(); }

function updateSavebar() {
  const onEdit = parseHash().tab === "edit" && state.draft;
  const isNew = state.editId === "new";
  $("#savebar").classList.toggle("hidden", !onEdit || !editorDirty());
  $("#savebar-text").textContent = isNew ? "Nowa stacja nie jest jeszcze zapisana"
    : L.renameRestartsStreams(state.saved, state.draft)
      ? "Niezapisane zmiany. Nowa nazwa stacji połączy ponownie wszystkie jej strumienie (krótka przerwa w nadawaniu)"
      : "Niezapisane zmiany";
  $("#save").textContent = isNew ? "Dodaj stację" : "Zapisz stację";
  $("#save").disabled = state.saving;
}

// Validation issues (400) for one field, marked as shown.
function issue(rel) {
  const list = state.issues.fields[rel];
  if (!list) return [];
  state.issuesUsed.add(rel);
  return list;
}

function fieldError(rel) { return el("p", { class: "field-error", dataset: { err: rel } }, issue(rel).join(" · ")); }

function checkField(node, rel) {
  if (state.issues.fields[rel]) {
    node.classList.add("invalid");
    node.setAttribute("aria-invalid", "true");
  }
  return node;
}

// A number the panel cannot read is marked on its field (data-bad) and blocks the save, so a value
// different from the one shown is never sent. Typing a valid number clears the mark (and any
// server message about the old value).
function setClientError(input, message) {
  const err = input.closest("label")?.querySelector("[data-err]");
  if (message) {
    input.dataset.bad = message;
    input.classList.add("invalid");
    input.setAttribute("aria-invalid", "true");
    if (err) err.textContent = message;
  } else if (input.dataset.bad || input.classList.contains("invalid")) {
    delete input.dataset.bad;
    input.classList.remove("invalid");
    input.removeAttribute("aria-invalid");
    if (err) err.textContent = "";
  }
}

// A number field typed by hand: "2,5" and "2.5" both work (text field, decimal keyboard).
function numberField(value, { rel, label, integer = false, onvalue }) {
  const input = checkField(el("input", { type: "text", inputMode: integer ? "numeric" : "decimal", value: L.numberText(value), autocomplete: "off",
    spellcheck: false, "aria-label": label, dataset: { field: rel.split(".").pop() } }), rel);
  input.addEventListener("input", () => {
    const c = L.checkNumber(input.value, { integer });
    setClientError(input, c.ok ? "" : c.error);
    if (c.ok) onvalue(c.value);
  });
  return input;
}

function renderEditor() {
  const d = state.draft;
  const isNew = state.editId === "new";
  state.issuesUsed = new Set();
  $("#edit-title").textContent = isNew ? "Nowa stacja" : `Edycja: ${d.name || state.editId}`;
  $("#edit-delete").hidden = isNew;
  renderEditPick();
  $("#edit-name").value = d.name;
  $("#edit-name").classList.toggle("invalid", !!state.issues.fields.name);
  if (state.issues.fields.name) $("#edit-name").setAttribute("aria-invalid", "true"); else $("#edit-name").removeAttribute("aria-invalid");
  $("#edit-name-error").textContent = issue("name").join(" · ");
  $("#edit-enabled").checked = d.enabled;
  $("#input-count").textContent = `${d.inputs.length} z ${LIM.inputs}`;
  $("#stream-count").textContent = `${d.streams.length} z ${LIM.streams}`;
  $("#add-input").disabled = d.inputs.length >= LIM.inputs;
  $("#add-stream").disabled = d.streams.length >= LIM.streams;
  $("#inputs-error").textContent = issue("inputs").join(" · ");
  $("#streams-error").textContent = issue("streams").join(" · ");
  $("#edit-inputs").replaceChildren(...d.inputs.map((_, j) => inputBlock(j)));
  if (!d.inputs.length) $("#edit-inputs").append(el("p", { class: "hint" }, "Bez wejść stacja nadaje ciszę."));
  $("#edit-streams").replaceChildren(...d.streams.map((_, k) => streamBlock(k)));
  if (!d.streams.length) $("#edit-streams").append(el("p", { class: "hint" }, "Brak strumieni. „Dodaj strumień” ustawia kodek, bitrate i serwer Icecast."));
  renderEquivalents();
  renderGeneralIssues();
  updateSavebar();
  applyLive();
}

// Issues the editor has no field for (or for another station) go into one list at the top.
function renderGeneralIssues() {
  const rest = Object.entries(state.issues.fields).filter(([rel]) => !state.issuesUsed.has(rel)).flatMap(([, m]) => m);
  const all = [...state.issues.general, ...rest];
  const box = $("#edit-general");
  box.replaceChildren(...all.map((m) => el("li", {}, m)));
  box.hidden = !all.length;
}

$("#edit-name").addEventListener("input", (e) => { state.draft.name = e.target.value; markDirty(); });
$("#edit-enabled").addEventListener("change", (e) => { state.draft.enabled = e.target.checked; markDirty(); });

$("#add-input").addEventListener("click", () => {
  if (state.draft.inputs.length >= LIM.inputs) return;
  state.draft.inputs.push(L.newInput());
  renderEditor();
  markDirty();
  $(`#edit-inputs .block:last-child textarea`)?.focus();
});

$("#add-stream").addEventListener("click", () => {
  if (state.draft.streams.length >= LIM.streams) return;
  state.draft.streams.push(L.newStream(state.codecs));
  renderEditor();
  markDirty();
  $(`#edit-streams .block:last-child [data-field=host]`)?.focus();
});

$("#edit-delete").addEventListener("click", () => { if (state.editId && state.editId !== "new") deleteStation(state.editId); });

// A gain or delay: a slider and a number field that follow each other.
function rangeField(value, { min, max, step, unit, label, rel, onvalue }) {
  const range = el("input", { type: "range", min, max, step, value: Math.min(max, Math.max(min, value)), "aria-label": `${label} (suwak)` });
  const num = numberField(value, { rel, label, onvalue: (v) => { range.value = Math.min(max, Math.max(min, v)); onvalue(v); } });
  range.addEventListener("input", () => {
    num.value = L.numberText(range.value);
    setClientError(num, "");
    onvalue(Number(range.value));
  });
  return el("span", { class: "range-row" }, range, num, el("span", { class: "unit" }, unit));
}

function inputBlock(j) {
  const inp = state.draft.inputs[j];
  const p = `inputs[${j}]`;
  const set = (key, value) => { inp[key] = value; markDirty(); };
  const live = inp._k != null && state.saved ? `in:${state.saved.id}:${inp._k}` : null;
  const info = L.sdpInfo(inp.sdp);
  const infoLine = el("span", { class: "hint sdp-info" }, info ? L.sdpSummary(info) : "brak SDP");
  const name = checkField(el("input", { type: "text", value: inp.name, maxLength: 64, placeholder: info?.name ? `z SDP: ${info.name}` : "nazwa (domyślnie z SDP)",
    "aria-label": `Nazwa wejścia ${j + 1}`, dataset: { field: "name" }, oninput: (e) => set("name", e.target.value) }), `${p}.name`);
  const sdp = checkField(el("textarea", { rows: 7, spellcheck: false, value: inp.sdp, placeholder: "v=0\no=- … IN IP4 …\ns=…\nc=IN IP4 239.69.…/32\nm=audio 5004 RTP/AVP 98\na=rtpmap:98 L24/48000/2",
    "aria-label": `SDP wejścia ${j + 1}`, dataset: { field: "sdp" } }), `${p}.sdp`);
  const updateSdp = () => {
    const i2 = L.sdpInfo(inp.sdp);
    infoLine.textContent = i2 ? L.sdpSummary(i2) : "brak SDP";
    name.placeholder = i2?.name ? `z SDP: ${i2.name}` : "nazwa (domyślnie z SDP)";
  };
  sdp.addEventListener("input", () => { set("sdp", sdp.value); updateSdp(); });
  const file = el("input", { type: "file", accept: ".sdp,application/sdp,text/plain", hidden: true, "aria-label": `Plik SDP wejścia ${j + 1}` });
  file.addEventListener("change", async () => {
    const f = file.files[0];
    file.value = "";
    if (!f) return;
    if (f.size > LIM.sdpMaxBytes) { toast(`Plik „${f.name}” jest za duży na SDP (najwyżej 64 KB)`, "err"); return; }
    const text = await f.text();
    sdp.value = text;
    set("sdp", text);
    updateSdp();
    toast(`Wczytano SDP z pliku „${f.name}”`);
  });
  const enabled = el("input", { type: "checkbox", checked: inp.enabled, "aria-label": `Wejście ${j + 1} włączone`,
    onchange: (e) => { set("enabled", e.target.checked); block.classList.toggle("off", !e.target.checked); } });
  const block = el("div", { class: "block" + (inp.enabled ? "" : " off"), dataset: { input: j } },
    el("div", { class: "block-head" },
      el("span", { class: "c-num" }, L.pad2(j + 1)),
      el("label", { class: "toggle" }, enabled, "włączone"),
      el("div", { class: "block-title" }, el("span", {}, `Wejście ${j + 1}`)),
      live ? meter(live) : null,
      live ? stateBox(live) : el("span", { class: "hint" }, "nowe: stan po zapisaniu"),
      el("button", { type: "button", class: "mini danger", dataset: { icon: "trash" }, onclick: () => removeInput(j) }, "Usuń")),
    el("div", { class: "block-body input-body" },
      el("label", { class: "sdp" }, "SDP źródła", sdp,
        el("div", { class: "sdp-tools" },
          el("button", { type: "button", class: "mini", dataset: { icon: "upload" }, onclick: () => file.click() }, "Wczytaj plik SDP…"), file, infoLine),
        fieldError(`${p}.sdp`)),
      el("label", {}, "Nazwa", name, fieldError(`${p}.name`)),
      el("label", {}, "Gain wejścia",
        rangeField(inp.gain_db, { min: LIM.gainMin, max: LIM.gainMax, step: LIM.gainStep, unit: "dB", label: `Gain wejścia ${j + 1} w dB`, rel: `${p}.gain_db`, onvalue: (v) => set("gain_db", v) }),
        el("small", {}, "−60…+12 dB, co 0,5 dB"), fieldError(`${p}.gain_db`)),
      el("label", {}, "Opóźnienie",
        rangeField(inp.delay_s, { min: 0, max: LIM.delayMax, step: LIM.delayStep, unit: "s", label: `Opóźnienie wejścia ${j + 1} w sekundach`, rel: `${p}.delay_s`, onvalue: (v) => set("delay_s", v) }),
        el("small", {}, "0–60 s, co 0,1 s: przesuń suwak albo wpisz wartość"), fieldError(`${p}.delay_s`)),
      el("label", {}, "Opóźnienie odbioru",
        el("span", { class: "range-row" },
          numberField(inp.latency_us / 1000, { rel: `${p}.latency_us`, label: `Opóźnienie odbioru wejścia ${j + 1} w ms`,
            onvalue: (v) => set("latency_us", Math.round(v * 1000)) }),
          el("span", { class: "unit" }, "ms")),
        el("small", {}, "bufor sieciowy, co najmniej ptime źródła + 1 ms"), fieldError(`${p}.latency_us`))));
  return block;
}

async function removeInput(j) {
  const inp = state.draft.inputs[j];
  if (inp.sdp && !(await confirmDialog(`Usunąć wejście ${j + 1}?`, "Wejście zniknie z edytowanej stacji; zmiana zadziała po zapisaniu.", "Usuń wejście"))) return;
  state.draft.inputs.splice(j, 1);
  state.issues = { fields: {}, general: state.issues.general };  // indices moved: field issues are stale
  renderEditor();
  markDirty();
}

function codecSelect(s, k) {
  const sel = el("select", { "aria-label": `Kodek strumienia ${k + 1}`, dataset: { field: "codec" } },
    ...state.codecs.codecs.map((c) => el("option", {
      value: c.id,
      disabled: !c.available && c.id !== s.codec,
      title: c.available ? c.content_type : c.reason,
    }, c.available ? c.label : `${c.label} (niedostępny)`)));
  sel.value = s.codec;
  return sel;
}

function streamBlock(k) {
  const s = state.draft.streams[k];
  const p = `streams[${k}]`;
  const saved = s.id ? state.saved?.streams.find((x) => x.id === s.id) : null;
  const live = saved && state.saved ? `str:${state.saved.id}:${s.id}` : null;
  const info = L.codec(state.codecs, s.codec);
  const set = (key, value) => { s[key] = value; markDirty(); };
  const rerender = (focusField) => {
    const fresh = streamBlock(k);
    block.replaceWith(fresh);
    applyLive();
    if (focusField) $(`[data-field=${focusField}]`, fresh)?.focus();
  };

  const codec = checkField(codecSelect(s, k), p);
  codec.addEventListener("change", () => { s.codec = codec.value; L.adjustStream(state.codecs, s); markDirty(); rerender("codec"); });
  const unavailable = state.codecs.codecs.filter((c) => !c.available);
  const codecNote = !info?.available
    ? el("small", { class: "warn-text" }, info?.reason || "kodek niedostępny")
    : unavailable.length
      ? el("small", {}, `Niedostępne: ${unavailable.map((c) => `${c.label} (${L.reasonDetail(c.reason)})`).join("; ")}`)
      : null;

  const rate = checkField(el("select", { "aria-label": `Częstotliwość strumienia ${k + 1}`, dataset: { field: "sample_rate" } },
    ...L.ratesFor(state.codecs, s.codec).map((r) => el("option", { value: r }, L.fmtRate(r)))), p);
  rate.value = String(s.sample_rate);
  rate.addEventListener("change", () => { s.sample_rate = Number(rate.value); L.adjustStream(state.codecs, s); markDirty(); rerender("sample_rate"); });

  let bitrateField;
  const hint = el("div", { class: "bitrate-hint", "aria-live": "polite" });
  const renderHint = () => {
    const h = L.bitrateHint(state.codecs, s.codec, s.bitrate_kbps);
    hint.replaceChildren();
    if (!h) return;
    if (h.mp3) hint.append(el("span", { class: "eq" }, "≈ MP3 ", el("b", {}, h.mp3)));
    if (h.advice) hint.append(el("span", { class: "warn-text" }, h.advice));
    if (h.text) hint.append(el("span", {}, h.text));
  };
  if (info?.lossless) {
    bitrateField = el("div", { class: "lossless" }, "bezstratny: 16 bit, Ogg FLAC");
  } else {
    const choices = L.bitrateChoices(state.codecs, s.codec, s.sample_rate, s.bitrate_kbps);
    bitrateField = checkField(el("select", { "aria-label": `Bitrate strumienia ${k + 1}`, dataset: { field: "bitrate_kbps" } },
      ...choices.map((v) => el("option", { value: v }, `${v} kb/s`))), p);
    bitrateField.value = String(s.bitrate_kbps);
    bitrateField.addEventListener("change", () => { set("bitrate_kbps", Number(bitrateField.value)); renderHint(); title.textContent = streamTitle(s); });
    renderHint();
  }

  // Rule E7: a stored password goes only to the server it was saved for.
  const pwHint = el("small", { class: "warn-text", hidden: true }, "po zmianie serwera wpisz hasło ponownie");
  const password = checkField(el("input", { type: "password", value: s.password, autocomplete: "new-password", maxLength: 255,
    "aria-label": `Hasło strumienia ${k + 1}`, dataset: { field: "password" } }), `${p}.password`);
  const updatePassword = () => {
    const again = L.needsPasswordAgain(saved, s);
    pwHint.hidden = !again;
    password.required = again && s.enabled;
    password.placeholder = again ? "wpisz hasło ponownie" : saved?.password_set ? "zapisane (puste = bez zmian)" : "hasło źródła Icecast";
  };
  password.addEventListener("input", () => { set("password", password.value); updatePassword(); });
  const text = (key, extra = {}) => checkField(el("input", { type: "text", value: s[key], autocomplete: "off", spellcheck: false, dataset: { field: key }, ...extra,
    oninput: (e) => { set(key, e.target.value.trim()); if (key === "host") updatePassword(); target.textContent = streamTarget(s); } }), `${p}.${key}`);
  const host = text("host", { placeholder: "np. stream.example.pl albo 192.168.1.20", maxLength: 253, "aria-label": `Serwer strumienia ${k + 1}` });
  const port = numberField(s.port, { rel: `${p}.port`, label: `Port strumienia ${k + 1}`, integer: true,
    onvalue: (v) => { set("port", v); updatePassword(); target.textContent = streamTarget(s); } });
  const mount = text("mount", { placeholder: "/radio.mp3", maxLength: 255, "aria-label": `Mountpoint strumienia ${k + 1}` });
  const user = text("user", { placeholder: "source", maxLength: 255, "aria-label": `Użytkownik strumienia ${k + 1}` });
  updatePassword();

  const title = el("span", {}, streamTitle(s));
  const target = el("small", {}, streamTarget(s));
  const enabled = el("input", { type: "checkbox", checked: s.enabled, "aria-label": `Strumień ${k + 1} włączony`,
    onchange: (e) => { set("enabled", e.target.checked); block.classList.toggle("off", !e.target.checked); updatePassword(); } });

  const block = el("div", { class: "block" + (s.enabled ? "" : " off"), dataset: { stream: k } },
    el("div", { class: "block-head" },
      el("span", { class: "c-num" }, L.pad2(k + 1)),
      el("label", { class: "toggle" }, enabled, "włączony"),
      el("div", { class: "block-title" }, title, target),
      live ? meter(live) : null,
      live ? stateBox(live) : el("span", { class: "hint" }, "nowy: stan po zapisaniu"),
      el("button", { type: "button", class: "mini danger", dataset: { icon: "trash" }, onclick: () => removeStream(k) }, "Usuń")),
    el("div", { class: "block-body stream-body" },
      el("label", { class: "s2" }, "Kodek", codec, codecNote),
      el("label", {}, "Częstotliwość", rate),
      el("label", { class: "s3" }, "Bitrate", bitrateField, hint),
      issue(p).length ? el("p", { class: "field-error s6" }, issue(p).join(" · ")) : null,
      el("label", { class: "s2" }, "Gain strumienia",
        rangeField(s.gain_db, { min: LIM.gainMin, max: LIM.gainMax, step: LIM.gainStep, unit: "dB", label: `Gain strumienia ${k + 1} w dB`, rel: `${p}.gain_db`, onvalue: (v) => set("gain_db", v) }),
        fieldError(`${p}.gain_db`)),
      el("label", { class: "s3" }, "Serwer Icecast", host, fieldError(`${p}.host`)),
      el("label", {}, "Port", port, fieldError(`${p}.port`)),
      el("label", { class: "s2" }, "Mountpoint", mount, el("small", {}, "zaczyna się od „/”"), fieldError(`${p}.mount`)),
      el("label", { class: "s2" }, "Użytkownik", user, fieldError(`${p}.user`)),
      el("label", { class: "s2" }, "Hasło", password, pwHint, fieldError(`${p}.password`))));
  return block;
}

async function removeStream(k) {
  const s = state.draft.streams[k];
  if (s.id && !(await confirmDialog(`Usunąć strumień ${k + 1}?`, `${streamTitle(s)} → ${streamTarget(s)}. Strumień rozłączy się po zapisaniu stacji.`, "Usuń strumień"))) return;
  state.draft.streams.splice(k, 1);
  state.issues = { fields: {}, general: state.issues.general };
  renderEditor();
  markDirty();
}

function renderEquivalents() {
  $("#eq-rows").replaceChildren(...L.equivalentRows(state.codecs).map((r) => el("tr", {}, el("td", {}, r.aac), el("td", {}, r.mp3))));
  $("#eq-text").textContent = state.codecs.recommendation_text ? `Podpowiedź: ${state.codecs.recommendation_text}.` : "";
}

// Fields the panel checks itself before sending: only rule E7 (everything else is the server's).
function clientIssues() {
  const fields = {};
  state.draft.streams.forEach((s, k) => {
    const saved = s.id ? state.saved?.streams.find((x) => x.id === s.id) : null;
    if (s.enabled && L.needsPasswordAgain(saved, s))
      fields[`streams[${k}].password`] = [`Strumień ${k + 1}: hasło jest wymagane, bo zapisane hasło nie jest wysyłane do innego serwera`];
  });
  return fields;
}

// Keyboard focus (and the view) on the first marked field, else on the list of other issues.
function focusFirstError() {
  const first = $("#tab-edit [aria-invalid=true]");
  const target = first || ($("#edit-general").hidden ? null : $("#edit-general"));
  if (!target) return;
  target.focus({ preventScroll: true });
  target.scrollIntoView({ block: "center" });
}

$("#save").addEventListener("click", async () => {
  if (!state.draft || state.saving) return;
  // a number the panel cannot read: the value shown is not the one that would be sent
  if ($("#tab-edit [data-bad]")) {
    toast("Popraw zaznaczone pola", "err");
    focusFirstError();
    return;
  }
  const own = clientIssues();
  if (Object.keys(own).length) {
    state.issues = { fields: own, general: [] };
    renderEditor();
    toast("Popraw zaznaczone pola", "err");
    focusFirstError();
    return;
  }
  const isNew = state.editId === "new";
  const id = state.editId;
  state.saving = true;
  updateSavebar();
  let r;
  try {
    if (!isNew) {
      // changed in another panel since this draft was opened: ask before replacing that change
      const fresh = await api("/api/config");
      if (fresh.ok) {
        state.config = fresh.data;
        renderStations();
        const current = state.config.stations.find((s) => s.id === id);
        if (current && L.changedElsewhere(state.saved, current) &&
          !(await confirmDialog("Stacja została zmieniona w innym panelu",
            "Zapisać mimo to? Twoje zmiany zastąpią zmiany wprowadzone w innym panelu lub na innym komputerze.", "Zapisz mimo to"))) return;
      }
    }
    const index = isNew ? state.config.stations.length : state.config.stations.findIndex((s) => s.id === id);
    r = await api(isNew ? "/api/stations" : `/api/stations/${encodeURIComponent(id)}`,
      { method: isNew ? "POST" : "PUT", body: JSON.stringify(L.stationBody(state.draft)) });
    r.index = index;
  } catch (e) {
    if (!(e instanceof AuthRequired)) toast("Brak połączenia z usługą: stacja nie została zapisana", "err");
    return;  // a 401 shows the login screen; the draft stays for after the login
  } finally {
    state.saving = false;
    updateSavebar();
  }
  if (!r.ok) {
    state.issues = L.mapIssues(r.data?.issues || [], r.index);
    if (!r.data?.issues?.length) state.issues.general.push(r.data?.error || `Błąd zapisu (HTTP ${r.status})`);
    renderEditor();
    toast(r.status === 400 ? "Stacja nie została zapisana: popraw zaznaczone pola" : (r.data?.error || "Błąd zapisu"), "err");
    focusFirstError();
    return;
  }
  adoptConfig(r.data);
  toast(!isNew ? "Zapisano i zastosowano stację" : state.draft.enabled ? "Dodano stację; nadaje od razu" : "Dodano stację (wyłączoną)");
  await openEditor(r.data.id, true);
  history.replaceState(null, "", editHash(r.data.id));
  renderEditPick();
});

$("#discard").addEventListener("click", async () => {
  if (state.editId === "new") {
    state.draft = null;
    state.editId = null;
    location.hash = "#stations";
    updateSavebar();
    return;
  }
  await openEditor(state.editId, true);
});

window.addEventListener("beforeunload", (e) => { if (editorDirty()) e.preventDefault(); });

// ---------------------------------------------------------------- global settings
async function openSettings() {
  const r = await api("/api/interfaces");
  const ifs = r.ok ? r.data : [];
  const c = state.config;
  const opts = [el("option", { value: "" }, "automatycznie (pierwsza karta z adresem IPv4)"),
    ...ifs.filter((i) => i.ipv4).map((i) => el("option", { value: i.ipv4 }, `${i.ipv4} · ${i.description || i.name}${i.up ? "" : " (rozłączona)"}`))];
  // the configured address stays selectable even when no interface has it now (or it is loopback)
  if (c.interface_ip && !ifs.some((i) => i.ipv4 === c.interface_ip)) opts.push(el("option", { value: c.interface_ip }, `${c.interface_ip} (obecne ustawienie)`));
  $("#set-iface").replaceChildren(...opts);
  $("#set-iface").value = c.interface_ip;
  $("#set-domain").value = c.ptp_domain;
  $("#set-port").value = c.http_port;
  $("#set-bind").value = c.http_bind;
  $("#set-error").hidden = true;
  $("#dlg-settings").showModal();
}
$("#open-settings").addEventListener("click", openSettings);

$("#settings-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const c = state.config;
  const next = {
    interface_ip: $("#set-iface").value,
    ptp_domain: Math.trunc(L.parseNumber($("#set-domain").value)),
    http_bind: $("#set-bind").value.trim(),
    http_port: Math.trunc(L.parseNumber($("#set-port").value)),
  };
  // only what changed is sent: PUT /api/config keeps every key left out
  const body = Object.fromEntries(Object.entries(next).filter(([k, v]) => v !== c[k]));
  if (!Object.keys(body).length) { $("#dlg-settings").close(); return; }
  for (const k of ["ptp_domain", "http_port"]) if (k in body && Number.isNaN(body[k])) body[k] = -1;  // the server names the field
  const r = await api("/api/config", { method: "PUT", body: JSON.stringify(body) });
  if (!r.ok) {
    const list = r.data?.issues?.length ? r.data.issues.map((x) => x.message) : [r.data?.error || "Błąd zapisu"];
    $("#set-error").replaceChildren(...list.map((m) => el("li", {}, m)));
    $("#set-error").hidden = false;
    return;
  }
  $("#dlg-settings").close();
  adoptConfig(r.data);
  toast("Zapisano ustawienia");
});

// ---------------------------------------------------------------- live feed
const PTP_LABELS = {
  locked: ["zsynchronizowany", "ok"],
  holdover: ["holdover", "warn"],
  listening: ["synchronizacja…", "warn"],
  freerun: ["zegar własny", "warn"],
  disabled: ["wyłączony", "warn"],
};

function ptpText(p) {
  if (p.state === "locked") return `zsynchronizowany z ${p.master || "?"}`;
  if (p.state === "freerun") return "brak mastera PTP: zegar tego komputera (free-run)";
  return (PTP_LABELS[p.state] || ["?"])[0];
}

function applyLive() {
  const live = state.live;
  if (!live || !state.config) return;
  const [ptpLabel, ptpKind] = PTP_LABELS[live.ptp.state] || ["?", "err"];
  setPill("#pill-ptp", ptpLabel, ptpKind, `Zegar PTP: ${ptpText(live.ptp)}${live.ptp.error ? `\n${live.ptp.error}` : ""}`);
  setPill("#pill-iface", live.interface_ip || "brak", null);
  let inOn = 0, inAll = 0, strOn = 0, strAll = 0;
  const stations = new Map(live.stations.map((s) => [s.id, s]));
  for (const st of live.stations) {
    if (!st.running) continue;
    for (const i of st.inputs) if (i.enabled) { ++inAll; inOn += i.signal ? 1 : 0; }
    for (const s of st.streams) if (s.enabled) { ++strAll; strOn += s.state === "streaming" ? 1 : 0; }
  }
  setPill("#pill-inputs", `${inOn}/${inAll}`, inAll ? (inOn === inAll ? "ok" : "warn") : null, "Wejścia AES67 z sygnałem / włączone (stacje działające)");
  setPill("#pill-streams", `${strOn}/${strAll}`, strAll ? (strOn === strAll ? "ok" : "warn") : null, "Strumienie, które nadają / włączone (stacje działające)");
  setPill("#stat-uptime", L.fmtUptime(live.uptime_s), null);

  for (const box of $$("[data-meter]")) {
    const [kind, sid, x] = box.dataset.meter.split(":");
    const st = stations.get(sid);
    const item = kind === "st" ? st : kind === "in" ? st?.inputs[Number(x)] : st?.streams.find((s) => s.id === x);
    setMeter(box, item?.peak_db);
  }
  for (const box of $$("[data-state]")) {
    const [kind, sid, x] = box.dataset.state.split(":");
    const st = stations.get(sid);
    const on = !!st?.enabled;
    setState(box, kind === "st" ? L.stationState(st)
      : kind === "in" ? L.inputState(st?.inputs[Number(x)], on)
        : L.streamState(st?.streams.find((s) => s.id === x), on), kind === "str");
  }
  for (const span of $$("[data-peak]")) {
    const st = stations.get(span.dataset.peak);
    span.textContent = st ? `${L.fmtPeak(st.peak_db[0])} / ${L.fmtPeak(st.peak_db[1])} dBFS` : "";
    span.title = "Szczyt sumy stacji L / P, dBFS";
  }
  if (parseHash().tab === "stats") renderStats();
  // another tab or computer changed the configuration: reload it (once per new live shape, so a
  // feed that lags behind a save, or never matches, does not cause a stream of reloads)
  const shape = L.stationsShape(live.stations);
  if (shape !== state.reloadedFor && shape !== L.stationsShape(state.config.stations)) {
    state.reloadedFor = shape;
    loadConfig().catch(() => {});
  }
}

function renderStats() {
  const live = state.live;
  if (!live) return;
  const p = live.ptp;
  const kv = (dl, rows) => dl.replaceChildren(...rows.flatMap(([k, v]) => [el("dt", {}, k), el("dd", {}, String(v))]));
  const fmtUs = (v) => `${L.fmtNum(v, 1)} µs`;
  kv($("#ptp-kv"), [
    ["Stan", ptpText(p)],
    ["Zegar", p.free_run ? "własny (free-run)" : "PTP"],
    ["Domena", p.domain],
    ["Grandmaster", p.grandmaster || "—"],
    ["Master", p.master || "—"],
    ["Odchyłka", p.has_master ? fmtUs(p.offset_us) : "—"],
    ["Opóźnienie ścieżki", p.has_master ? fmtUs(p.path_delay_us) : "—"],
    ["Korekta częstotliwości", `${L.fmtNum(p.rate_ppm, 3)} ppm`],
    ["Utraty synchronizacji", p.sync_lost],
    ["Korekty skokowe", p.steps],
    ...(p.error ? [["Błąd", p.error]] : []),
  ]);
  const count = (list, f) => list.filter(f).length;
  const streams = live.stations.flatMap((s) => s.streams.map((x) => ({ st: s, s: x })));
  const inputs = live.stations.flatMap((s) => s.inputs.map((x) => ({ st: s, i: x })));
  kv($("#svc-kv"), [
    ["Program", `${live.product} ${live.version}`],
    ["Czas działania", L.fmtUptime(live.uptime_s)],
    ["Karta sieciowa", live.interface_ip || "—"],
    ["Stacje działające", `${count(live.stations, (s) => s.running)} z ${live.stations.length}`],
    ["Wejścia z sygnałem", `${count(inputs, (x) => x.st.running && x.i.enabled && x.i.signal)} z ${count(inputs, (x) => x.st.running && x.i.enabled)}`],
    ["Strumienie nadające", `${count(streams, (x) => x.st.running && x.s.enabled && x.s.state === "streaming")} z ${count(streams, (x) => x.st.running && x.s.enabled)}`],
  ]);
  const num = (v, bad) => el("td", { class: bad ? "bad" : "" }, L.fmtNum(v));
  const name = (st, i) => el("td", { class: "l name" }, st.name || `Stacja ${i + 1}`);
  const stateCell = (s, tally = false) => { const b = stateBox(null); setState(b, s, tally); return el("td", { class: "l" }, b); };
  $("#stats-station-rows").replaceChildren(...live.stations.map((st, i) => el("tr", {},
    name(st, i), stateCell(L.stationState(st)), el("td", {}, st.running ? L.fmtUptime(st.uptime_s) : "—"), num(st.skipped_frames, st.skipped_frames > 0))));
  if (!live.stations.length) $("#stats-station-rows").append(el("tr", {}, el("td", { class: "l hint", colSpan: 4 }, "Brak stacji.")));
  $("#stats-input-rows").replaceChildren(...live.stations.flatMap((st, i) => st.inputs.map((x, j) => {
    const r = x.rx;
    return el("tr", {},
      name(st, i), el("td", { class: "l" }, x.name || `Wejście ${j + 1}`), el("td", { class: "l mono" }, x.address || "—"),
      num(r.packets), num(r.bytes), num(r.underruns, r.underruns > 0), num(r.overruns, r.overruns > 0), num(r.seq_errors, r.seq_errors > 0),
      el("td", {}, L.fmtNum(r.jitter_ms, 3)), stateCell(L.inputState(x, st.enabled)));
  })));
  if (!inputs.length) $("#stats-input-rows").append(el("tr", {}, el("td", { class: "l hint", colSpan: 10 }, "Brak wejść.")));
  $("#stats-stream-rows").replaceChildren(...live.stations.flatMap((st, i) => st.streams.map((x) => el("tr", {},
    name(st, i),
    el("td", { class: "l" }, L.codec(state.codecs, x.codec)?.lossless ? `${x.codec_label} · ${L.fmtRate(x.sample_rate)}` : `${x.codec_label} · ${x.bitrate_kbps} kb/s · ${L.fmtRate(x.sample_rate)}`),
    el("td", { class: "l mono" }, `${x.host}:${x.port}${x.mount}`),
    stateCell(L.streamState(x, st.enabled), true),
    el("td", {}, L.fmtBytes(x.bytes_sent)), el("td", {}, L.fmtNum(x.kbps, 1)),
    num(x.reconnects, x.reconnects > 0), el("td", { class: x.dropped_bytes > 0 ? "bad" : "" }, L.fmtBytes(x.dropped_bytes)),
    num(x.queue_dropped_frames, x.queue_dropped_frames > 0), num(x.conditioner_dropped_frames, x.conditioner_dropped_frames > 0),
    el("td", {}, x.state === "streaming" ? L.fmtUptime(x.connected_s) : "—")))));
  if (!streams.length) $("#stats-stream-rows").append(el("tr", {}, el("td", { class: "l hint", colSpan: 11 }, "Brak strumieni.")));
}

// The feed ends on logout, session expiry, a password change or a service restart; the browser
// reconnects by itself unless the answer was an error (401): then the login screen or a retry.
function connectLive() {
  stopLive();
  const src = new EventSource("/api/live");
  src.addEventListener("status", (e) => {
    state.live = JSON.parse(e.data);
    applyLive();
  });
  src.onerror = async () => {
    setPill("#pill-ptp", "brak połączenia z usługą", "err");
    if (src.readyState !== EventSource.CLOSED || state.source !== src) return;
    state.source = null;
    try {
      const { data } = await api("/api/auth-state");
      if (!data.logged_in) { showAuth(); return; }
    } catch { /* the service is not answering: try again */ }
    if ($("#app").classList.contains("hidden")) return;
    state.liveRetry = setTimeout(connectLive, 2000);
  };
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
  if (!data.length) $("#log-rows").append(el("tr", {}, el("td", { class: "hint" }, "Dziennik jest pusty.")));
}
$("#refresh-logs").addEventListener("click", loadLogs);

// ---------------------------------------------------------------- boot
async function boot() {
  $("#auth").classList.add("hidden");
  $("#app").classList.remove("hidden");
  const [cfg, codecs] = await Promise.all([api("/api/config"), api("/api/codecs")]);
  state.config = cfg.data;
  state.codecs = codecs.data;
  // after a new login (session expired, service restarted) an open draft stays while its station exists
  if (L.keepDraft(state.editId, state.config.stations) && state.draft) {
    renderEditor();
  } else {
    state.editId = null;
    state.draft = null;
  }
  renderStations();
  renderEditPick();
  connectLive();
  showTab();
}

(async function init() {
  const { data } = await api("/api/auth-state");
  state.logo = data.logo || null;  // encoder/panel/logo.svg (or logo.png), when the file is bundled
  renderBrand();
  if (data.logged_in) boot();
  else showAuth();
})();
