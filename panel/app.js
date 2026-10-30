// SPDX-License-Identifier: GPL-3.0-or-later
// BounceAES Master web panel. Plain JS, no framework; talks to the REST API and the SSE live feed.
"use strict";

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const PTIMES = [[1000, "1 ms"], [1333, "1,333 ms"], [4000, "4 ms"]];
const GROUPS = [["net", "Sieć AoIP"], ["asio", "ASIO"], ["win", "Windows"]];
const TABS = ["routing", "config", "stats", "logs"];

const state = {
  config: null,  // last saved configuration (server)
  draft: null,   // edited copy
  interfaces: [],
  live: null,
  status: null,
  routing: null,
  issues: [],
  logo: null,    // URL of panel logo file, when present
  source: null,  // EventSource
  pasteTarget: null,  // null = add a new subscription, number = edit that subscription (0-based)
  collapsed: new Set(),  // "src:net", "dst:asio", ...
  routingTimer: null,
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

function issuesText(r, fallback) {
  const list = r.data?.issues || [];
  return list.length ? list.map((x) => x.message).join(" · ") : (r.data?.error || fallback);
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

function meterValue(db) { return Math.max(0, Math.min(1, (db + 60) / 60)); }

// A stereo meter: two hairline bars, updated through data-meter="<key>-<side>".
function meter(key) {
  return el("div", { class: "meter stereo" },
    el("span", { dataset: { meter: `${key}-0` } }),
    el("span", { dataset: { meter: `${key}-1` } }));
}

function setMeter(span, db) {
  span.style.setProperty("--v", meterValue(db));
  span.classList.toggle("clip", db > -1);
}

function sourceName(id) {
  const n = state.routing?.sources.find((s) => s.id === id);
  if (n) return n.name;
  const [kind, rest] = [id.slice(0, id.indexOf(":")), id.slice(id.indexOf(":") + 1)];
  if (kind === "asio-out") return state.config?.asio_names[Number(rest) - 1] || `BounceAES ${pad2(rest)}`;
  if (kind === "net-in") return state.config?.subscriptions[Number(rest) - 1]?.name || `Subskrypcja ${pad2(rest)}`;
  return rest;
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
async function showAuth() {
  if (state.source) { state.source.close(); state.source = null; }
  stopRoutingPoll();
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
function currentTab() {
  const tab = (location.hash || "#routing").slice(1);
  return TABS.includes(tab) ? tab : "routing";
}

function showTab() {
  const valid = currentTab();
  for (const a of $$(".tabs a")) a.classList.toggle("active", a.dataset.tab === valid);
  for (const s of $$(".tab")) s.classList.toggle("hidden", s.id !== `tab-${valid}`);
  if (valid === "logs") loadLogs();
  if (valid === "routing") startRoutingPoll(); else stopRoutingPoll();
  if (valid === "stats") renderStats();
}
window.addEventListener("hashchange", showTab);

// ---------------------------------------------------------------- routing
async function loadRouting() {
  try {
    const r = await api("/api/routing");
    if (!r.ok) return;
    const before = state.routing ? JSON.stringify({ s: state.routing.sources.map((n) => [n.id, n.name, n.available, n.note, n.virtual]),
      d: state.routing.dests.map((n) => [n.id, n.name, n.available, n.note, n.virtual]), r: state.routing.routes }) : "";
    state.routing = r.data;
    const after = JSON.stringify({ s: r.data.sources.map((n) => [n.id, n.name, n.available, n.note, n.virtual]),
      d: r.data.dests.map((n) => [n.id, n.name, n.available, n.note, n.virtual]), r: r.data.routes });
    if (before !== after) {
      renderMatrix();
      if ($("#dlg-virtual").open) renderVirtual();
    } else applyRoutingMeters();
  } catch { /* shown by the auth screen */ }
}

function startRoutingPoll() {
  if (state.routingTimer) return;
  loadRouting();
  state.routingTimer = setInterval(loadRouting, 500);
}

function stopRoutingPoll() {
  clearInterval(state.routingTimer);
  state.routingTimer = null;
}

function usedIds() {
  const routes = state.routing?.routes || {};
  return { src: new Set(Object.values(routes)), dst: new Set(Object.keys(routes)) };
}

function visibleNodes(list, side) {
  const only = $("#only-used").checked;
  const used = usedIds()[side];
  return GROUPS.map(([g, label]) => ({
    group: g, label,
    nodes: list.filter((n) => n.group === g && (!only || used.has(n.id))),
    collapsed: state.collapsed.has(`${side}:${g}`),
  }));
}

function toggleGroup(key) {
  if (state.collapsed.has(key)) state.collapsed.delete(key); else state.collapsed.add(key);
  renderMatrix();
}

function renderMatrix() {
  const table = $("#matrix");
  const v = state.routing;
  if (!v) return;
  const winNote = state.live && !state.live.bridge?.connected ? "Audio Windows: brak sesji użytkownika" : "";
  const srcGroups = visibleNodes(v.sources, "src");
  const dstGroups = visibleNodes(v.dests, "dst").filter((g) => g.nodes.length || g.group !== "win" || !$("#only-used").checked);
  // Columns: a collapsed or empty group keeps one narrow placeholder column.
  const columns = [];
  for (const g of dstGroups) {
    if (g.collapsed || !g.nodes.length) columns.push({ group: g.group, placeholder: true, first: true });
    else g.nodes.forEach((n, k) => columns.push({ group: g.group, node: n, first: k === 0 }));
  }

  const head1 = el("tr", { class: "groups" }, el("th", { class: "corner", rowSpan: 2 },
    el("span", { class: "corner-dst" }, "cele →"), el("span", { class: "corner-src" }, "źródła ↓")));
  for (const g of dstGroups) {
    const span = g.collapsed || !g.nodes.length ? 1 : g.nodes.length;
    head1.append(el("th", { colSpan: span, class: "group-head first" },
      el("button", { type: "button", class: "group-toggle", "aria-expanded": String(!g.collapsed), onclick: () => toggleGroup(`dst:${g.group}`) },
        `${g.collapsed ? "▸" : "▾"} ${g.label}`)));
  }
  const head2 = el("tr", { class: "names" });
  for (const c of columns) {
    if (c.placeholder) {
      head2.append(el("th", { class: "dst placeholder first" }, el("span", {}, c.group === "win" && !c.collapsed ? (winNote ? "brak sesji" : "brak urządzeń") : "…")));
      continue;
    }
    const n = c.node;
    head2.append(el("th", { class: "dst" + (c.first ? " first" : "") + (n.available ? "" : " unavailable"), title: `${n.name}\n${n.id}${virtualText(n)}${n.note ? "\n" + n.note : ""}` },
      el("span", {}, n.name)));
  }

  const body = [];
  for (const g of srcGroups) {
    const tb = el("tbody", { class: `group-${g.group}` });
    tb.append(el("tr", { class: "group-row" }, el("th", { class: "group-head", colSpan: columns.length + 1 },
      el("button", { type: "button", class: "group-toggle", "aria-expanded": String(!g.collapsed), onclick: () => toggleGroup(`src:${g.group}`) },
        `${g.collapsed ? "▸" : "▾"} ${g.label}`),
      g.group === "net" && !g.nodes.length ? el("span", { class: "hint inline-hint" }, "brak subskrypcji — „Dodaj źródło…”") : null,
      g.group === "win" && !g.nodes.length ? el("span", { class: "hint inline-hint" }, winNote || "brak źródeł Windows") : null)));
    if (!g.collapsed) {
      for (const n of g.nodes) {
        const row = el("tr", { class: n.available ? "" : "unavailable" },
          el("th", { class: "src", title: `${n.id}${virtualText(n)}${n.note ? "\n" + n.note : ""}` },
            el("span", { class: "src-name" }, n.name), n.virtual ? el("i", { class: "vtag" }, "wirt.") : null, meter(`r-${n.id}`)));
        for (const c of columns) {
          if (c.placeholder) { row.append(el("td", { class: "cell placeholder first" })); continue; }
          const on = v.routes[c.node.id] === n.id;
          const label = `${n.name} → ${c.node.name}`;
          row.append(el("td", { class: "cell" + (c.first ? " first" : "") },
            el("button", {
              type: "button",
              class: "x" + (on ? " on" : "") + (on && (!n.available || !c.node.available) ? " dim" : ""),
              title: on ? `${label} (połączone${n.available ? "" : ", źródło niedostępne"}) — kliknij, aby rozłączyć` : label,
              "aria-pressed": String(on),
              "aria-label": label,
              onclick: () => toggleRoute(c.node.id, n.id),
            })));
        }
        tb.append(row);
      }
    }
    body.push(tb);
  }
  table.replaceChildren(el("thead", {}, head1, head2), ...body);
  applyRoutingMeters();
}

function applyRoutingMeters() {
  const v = state.routing;
  if (!v) return;
  const peaks = new Map(v.sources.map((n) => [n.id, n.db]));
  for (const span of $$("#matrix [data-meter]")) {
    const key = span.dataset.meter;
    const side = Number(key.slice(-1));
    const id = key.slice(2, -2);
    setMeter(span, (peaks.get(id) || [-120, -120])[side]);
  }
}

// "ASIO: Warszawa" — the group tells apart a source and an output that share a name
function sourceLabel(id) {
  const group = GROUPS.find(([g]) => g === state.routing?.sources.find((n) => n.id === id)?.group);
  return group ? `${group[1]}: ${sourceName(id)}` : sourceName(id);
}

function virtualText(n) { return n.virtual ? `\nwirtualna karta: ${n.virtual}` : ""; }

async function toggleRoute(dst, src) {
  const on = state.routing.routes[dst] === src;
  return setRoute(dst, on ? null : src);
}

async function setRoute(dst, src) {
  const r = await api("/api/routing", { method: "PUT", body: JSON.stringify({ dst, src }) });
  if (!r.ok) { toast(issuesText(r, "Nie można zmienić połączenia"), "err"); return false; }
  for (const n of r.data.notices || []) toast(n, "warn");
  state.routing = r.data.routing;
  syncRoutes(r.data.routing.routes);
  renderMatrix();
  if ($("#dlg-virtual").open) renderVirtual();
  return true;
}

// Routes change immediately (routing tab), so both the saved config and the draft follow them.
function syncRoutes(routes) {
  if (state.config) state.config.routes = clone(routes);
  if (state.draft) state.draft.routes = clone(routes);
  if (state.draft) renderOutputs();
}

// ---------------------------------------------------------------- virtual sound cards
// Quick routes between virtual cards (VB-CABLE, ...) and AoIP: the same routes as the matrix.
function renderVirtual() {
  const v = state.routing;
  const box = $("#virtual-body");
  if (!v) { box.replaceChildren(); return; }
  const vSources = v.sources.filter((n) => n.virtual);
  const vDests = v.dests.filter((n) => n.virtual);
  const outputs = v.dests.filter((n) => n.group === "net");
  const netSources = v.sources.filter((n) => n.group === "net");
  const bridgeDown = state.live && !state.live.bridge?.connected;
  if (!vSources.length && !vDests.length) {
    box.replaceChildren(el("p", { class: "hint" }, bridgeDown
      ? "Audio Windows nie działa: ikona BounceAES Master w zasobniku uruchamia je po zalogowaniu użytkownika."
      : "Nie wykryto wirtualnej karty. Zainstaluj darmowy VB-CABLE (vb-audio.com/Cable) jako administrator, uruchom komputer ponownie i ustaw CABLE Input oraz CABLE Output na 48 000 Hz."));
    return;
  }
  const feeding = (src) => outputs.filter((o) => v.routes[o.id] === src).map((o) => o.name);
  const option = (value, text) => el("option", { value }, text);

  const sendRows = vSources.map((n) => {
    const sel = el("select", { "aria-label": `Wyjście AoIP dla ${n.name}` },
      ...outputs.map((o) => option(o.id, v.routes[o.id] && v.routes[o.id] !== n.id ? `${o.name} — teraz gra ${sourceLabel(v.routes[o.id])}` : o.name)));
    const free = outputs.find((o) => !v.routes[o.id]);
    if (free) sel.value = free.id;
    const now = feeding(n.id);
    return el("tr", {},
      el("td", {}, n.name, el("div", { class: "hint" }, n.virtual)),
      el("td", {}, now.length ? now.join(", ") : el("span", { class: "hint" }, "—")),
      el("td", {}, outputs.length ? sel : el("span", { class: "hint" }, "brak włączonych wyjść")),
      el("td", {}, outputs.length ? el("button", { type: "button", class: "mini primary", onclick: () => setRoute(sel.value, n.id) }, "Wysyłaj") : null));
  });
  const recvRows = vDests.map((d) => {
    const cur = v.routes[d.id];
    const sel = el("select", { "aria-label": `Źródło dla ${d.name}` }, ...netSources.map((s) => option(s.id, s.name)));
    if (cur && netSources.some((s) => s.id === cur)) sel.value = cur;
    return el("tr", {},
      el("td", {}, d.name, el("div", { class: "hint" }, d.virtual)),
      el("td", {}, cur ? sourceLabel(cur) : el("span", { class: "hint" }, "—")),
      el("td", {}, netSources.length ? sel : el("span", { class: "hint" }, "brak subskrypcji")),
      el("td", {},
        netSources.length ? el("button", { type: "button", class: "mini primary", onclick: () => setRoute(d.id, sel.value) }, "Odbieraj") : null,
        cur ? el("button", { type: "button", class: "mini", onclick: () => setRoute(d.id, null) }, "Rozłącz") : null));
  });
  const table = (caption, hint, head, rows) => [
    el("h4", { class: "virtual-head" }, caption), el("p", { class: "hint" }, hint),
    el("div", { class: "table-wrap" }, el("table", { class: "streams" },
      el("thead", {}, el("tr", {}, ...head.map((h) => el("th", {}, h)))),
      el("tbody", {}, ...(rows.length ? rows : [el("tr", {}, el("td", { colSpan: 4, class: "hint" }, "brak"))]))))];
  box.replaceChildren(
    ...table("Do sieci", "Aplikacja gra na wirtualnej karcie (np. CABLE Input), dźwięk idzie na wyjście AoIP.",
      ["Karta", "Teraz na", "Wyjście AoIP", ""], sendRows),
    ...table("Z sieci", "Strumień AoIP trafia na kartę; aplikacja nagrywa z jej drugiego końca (np. CABLE Output).",
      ["Karta", "Teraz gra", "Subskrypcja", ""], recvRows));
}

$("#open-virtual").addEventListener("click", async () => {
  await loadRouting();
  renderVirtual();
  $("#dlg-virtual").showModal();
});

$("#only-used").addEventListener("change", renderMatrix);
$("#add-source").addEventListener("click", () => openBrowse());

// ---------------------------------------------------------------- config: global
function levelHint(v) {
  const gain = v + 20;
  if (gain === 0) return "−20 dBFS = 0 dB: sygnał bez zmian (bit w bit)";
  return `do aplikacji ${gain > 0 ? "+" : "−"}${Math.abs(gain)} dB, do sieci ${gain > 0 ? "−" : "+"}${Math.abs(gain)} dB`;
}

const ROLE_HINTS = {
  auto: "Komputer zostaje masterem tylko wtedy, gdy w sieci nie ma lepszego zegara.",
  preferred: "Uwaga: komputer może przejąć zegar całej sieci AoIP.",
  slave: "Komputer nigdy nie nadaje zegara PTP.",
};

function renderGlobal() {
  const d = state.draft;
  $("#g-station").value = d.station_name;
  $("#g-station").placeholder = state.status?.station_name || "nazwa komputera";
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
  $("#g-role").value = d.ptp_role;
  $("#g-role-hint").textContent = ROLE_HINTS[d.ptp_role] || "";
  $("#g-role-hint").classList.toggle("warn-text", d.ptp_role === "preferred");
  $("#g-prio1").value = d.ptp_priority1;
  $("#g-prio2").value = d.ptp_priority2;
  $("#g-prio1").disabled = $("#g-prio2").disabled = d.ptp_role === "slave";
  $("#g-level").value = d.pc_nominal_level_dbfs;
  $("#g-level-out").textContent = `${d.pc_nominal_level_dbfs} dBFS`;
  $("#g-level-hint").textContent = levelHint(d.pc_nominal_level_dbfs);
  $("#g-port").value = d.http_port;
}

$("#g-station").addEventListener("input", (e) => { state.draft.station_name = e.target.value.trim(); markDirty(); });
$("#g-iface").addEventListener("change", (e) => { state.draft.interface_ip = e.target.value; markDirty(); });
$("#g-domain").addEventListener("input", (e) => { state.draft.ptp_domain = Number(e.target.value); markDirty(); });
$("#g-role").addEventListener("change", (e) => {
  const d = state.draft;
  d.ptp_role = e.target.value;
  // default priorities follow the role while the user has not changed them
  if (d.ptp_role === "preferred" && d.ptp_priority1 === 250) d.ptp_priority1 = 100;
  if (d.ptp_role === "auto" && d.ptp_priority1 === 100) d.ptp_priority1 = 250;
  renderGlobal();
  markDirty();
});
$("#g-prio1").addEventListener("input", (e) => { state.draft.ptp_priority1 = Number(e.target.value); markDirty(); });
$("#g-prio2").addEventListener("input", (e) => { state.draft.ptp_priority2 = Number(e.target.value); markDirty(); });
$("#g-port").addEventListener("input", (e) => { state.draft.http_port = Number(e.target.value); markDirty(); });
$("#g-level").addEventListener("input", (e) => {
  const v = Number(e.target.value);
  state.draft.pc_nominal_level_dbfs = v;
  $("#g-level-out").textContent = `${v} dBFS`;
  $("#g-level-hint").textContent = levelHint(v);
  markDirty();
});

// ---------------------------------------------------------------- config: tables
function issueRow(tbody, row, prefix, cols) {
  const issues = state.issues.filter((x) => x.field.startsWith(prefix));
  if (!issues.length) return;
  for (const x of issues) $(`[data-field="${x.field.slice(prefix.length)}"]`, row)?.classList.add("invalid");
  tbody.append(el("tr", { class: "row-error" }, el("td", { colSpan: cols },
    el("p", { class: "field-error" }, issues.map((x) => x.message).join(" · ")))));
}

function renderOutputs() {
  const tbody = $("#output-rows");
  const onlyActive = $("#only-active").checked;
  tbody.replaceChildren();
  state.draft.outputs.forEach((o, i) => {
    if (onlyActive && !o.enabled) return;
    const n = i + 1;
    const set = (key, value) => { o[key] = value; markDirty(); };
    const ptime = el("select", { onchange: (e) => set("ptime_us", Number(e.target.value)) },
      ...PTIMES.map(([v, t]) => el("option", { value: v, selected: o.ptime_us === v }, t)));
    const src = state.draft.routes[`net-out:${n}`];
    const row = el("tr", { class: o.enabled ? "" : "off" },
      el("td", { class: "c-num" }, pad2(n)),
      el("td", {}, el("input", { type: "checkbox", checked: o.enabled, "aria-label": `Wyjście ${pad2(n)} aktywne`,
        onchange: (e) => { set("enabled", e.target.checked); row.classList.toggle("off", !e.target.checked); } })),
      el("td", {}, el("input", { type: "text", value: o.name, maxLength: 40, dataset: { field: "name" }, "aria-label": `Nazwa wyjścia ${pad2(n)}`, oninput: (e) => set("name", e.target.value) })),
      el("td", {}, el("div", { class: "inline" },
        el("input", { type: "text", class: "addr", value: o.address, dataset: { field: "address" }, "aria-label": `Adres wyjścia ${pad2(n)}`, oninput: (e) => set("address", e.target.value.trim()) }),
        el("button", { type: "button", class: "mini", title: "Pokaż SDP wyjścia", onclick: () => openSdp(n) }, "SDP"))),
      el("td", {}, ptime),
      el("td", {}, el("input", { type: "checkbox", checked: o.keep_active, "aria-label": `Utrzymuj wyjście ${pad2(n)}`, onchange: (e) => set("keep_active", e.target.checked) })),
      el("td", {}, el("a", { class: "route-link" + (src ? "" : " none"), href: "#routing", title: src || "" }, src ? sourceName(src) : "brak")),
      el("td", {}, meter(`out-${n}`)));
    tbody.append(row);
    issueRow(tbody, row, `outputs[${n}].`, 8);
  });
  $('[data-issue="0"]').textContent = state.issues.filter((x) => x.device === 0).map((x) => x.message).join(" · ");
  applyLive();
}
$("#only-active").addEventListener("change", renderOutputs);

function renderSubscriptions() {
  const tbody = $("#subscription-rows");
  tbody.replaceChildren();
  let shown = 0;
  state.draft.subscriptions.forEach((s, i) => {
    if (!s.enabled && !s.sdp) return;
    ++shown;
    const n = i + 1;
    const set = (key, value) => { s[key] = value; markDirty(); };
    const info = sdpInfo(s.sdp);
    const row = el("tr", { class: s.enabled ? "" : "off" },
      el("td", { class: "c-num" }, pad2(n)),
      el("td", {}, el("input", { type: "checkbox", checked: s.enabled, "aria-label": `Subskrypcja ${pad2(n)} aktywna`,
        onchange: (e) => { set("enabled", e.target.checked); row.classList.toggle("off", !e.target.checked); } })),
      el("td", {}, el("input", { type: "text", value: s.name, maxLength: 40, dataset: { field: "name" }, "aria-label": `Nazwa subskrypcji ${pad2(n)}`, oninput: (e) => set("name", e.target.value) })),
      el("td", {}, el("div", { class: "source" },
        el("span", { class: "dot", dataset: { sig: n } }),
        info ? el("span", { class: "label", title: info.addr }, info.name || "(bez nazwy)", el("small", {}, info.addr)) : el("span", { class: "label none" }, "brak SDP"),
        el("button", { type: "button", class: "mini", title: "Zmień SDP źródła", onclick: () => openPaste(i) }, "SDP…"))),
      el("td", {}, el("input", { type: "number", class: "lat", min: 1, max: 1000, step: 0.5, value: s.latency_us / 1000, dataset: { field: "latency_us" },
        "aria-label": `Opóźnienie subskrypcji ${pad2(n)} w ms`, oninput: (e) => set("latency_us", Math.round(Number(e.target.value) * 1000)) })),
      el("td", {}, meter(`sub-${n}`)),
      el("td", {}, el("button", { type: "button", class: "mini", title: "Usuń subskrypcję i jej połączenia", onclick: () => removeSubscription(i) }, "Usuń")));
    tbody.append(row);
    issueRow(tbody, row, `subscriptions[${n}].`, 7);
  });
  if (!shown) tbody.append(el("tr", {}, el("td", { colSpan: 7, class: "hint" }, "Brak subskrypcji. „Dodaj źródło…” wybiera strumień z sieci (SAP) albo przyjmuje wklejone SDP.")));
  applyLive();
}

function removeSubscription(i) {
  const d = state.draft;
  d.subscriptions[i] = { enabled: false, name: "", sdp: "", latency_us: 5000 };
  const id = `net-in:${i + 1}`;
  for (const [dst, src] of Object.entries(d.routes)) if (src === id) delete d.routes[dst];
  markDirty();
  renderSubscriptions();
  renderOutputs();
}

function renderAsioNames() {
  const box = $("#asio-names");
  box.replaceChildren(...state.draft.asio_names.map((name, i) => el("label", { class: "asio-name" },
    el("span", { class: "c-num" }, pad2(i + 1)),
    el("input", { type: "text", value: name, maxLength: 40, placeholder: `BounceAES ${pad2(i + 1)}`, "aria-label": `Nazwa pary ASIO ${pad2(i + 1)}`,
      oninput: (e) => { state.draft.asio_names[i] = e.target.value; markDirty(); } }))));
}

function renderConfig() {
  renderGlobal();
  renderOutputs();
  renderSubscriptions();
  renderAsioNames();
  markDirty();
}

function markDirty() {
  const dirty = JSON.stringify(state.draft) !== JSON.stringify(state.config);
  $("#savebar").classList.toggle("hidden", !dirty);
}

$("#discard").addEventListener("click", () => {
  state.draft = clone(state.config);
  state.issues = [];
  renderConfig();
});

$("#save").addEventListener("click", async () => {
  const body = clone(state.draft);
  // routes are merged on the server: removed routes are sent as null
  for (const dst of Object.keys(state.config.routes)) if (!(dst in body.routes)) body.routes[dst] = null;
  const r = await api("/api/config", { method: "PUT", body: JSON.stringify(body) });
  state.issues = r.data?.issues || [];
  if (!r.ok) {
    toast(r.data?.error || "Konfiguracja zawiera błędy — popraw zaznaczone pola", "err");
    renderConfig();
    return;
  }
  state.config = r.data.config;
  state.draft = clone(state.config);
  for (const n of r.data.notices || []) toast(n, "warn");
  toast("Zapisano i zastosowano konfigurację");
  renderConfig();
  loadRouting();
});

// ---------------------------------------------------------------- dialogs
async function openBrowse() {
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
    tbody.append(el("tr", {}, el("td", { colSpan: 5, class: "hint" }, "Brak ogłoszeń SAP w sieci. Możesz wkleić SDP ręcznie (przycisk „Wklej SDP…”).")));
    return;
  }
  for (const s of data) {
    tbody.append(el("tr", {},
      el("td", {}, s.name),
      el("td", { class: "mono" }, s.origin),
      el("td", { class: "mono" }, `${s.address}:${s.port}`),
      el("td", {}, `${s.encoding} · ${s.channels} kan. · ${String(s.ptime_ms).replace(".", ",")} ms`),
      el("td", {}, el("button", { type: "button", class: "mini primary", onclick: () => addSubscription({ sap_id: s.id }) }, "Dodaj"))));
  }
}

async function addSubscription(body) {
  const r = await api("/api/subscriptions", { method: "POST", body: JSON.stringify(body) });
  if (!r.ok) { toast(issuesText(r, "Nie można dodać subskrypcji"), "err"); return false; }
  $("#dlg-browse").close();
  for (const n of r.data.notices || []) toast(n, "warn");
  toast(`Dodano subskrypcję ${pad2(r.data.created)}`);
  // keep unsaved edits: take the new subscription from the server, everything else from the draft
  const i = r.data.created - 1;
  const fresh = r.data.config;
  const dirty = JSON.stringify(state.draft) !== JSON.stringify(state.config);
  state.config = fresh;
  if (dirty) state.draft.subscriptions[i] = clone(fresh.subscriptions[i]); else state.draft = clone(fresh);
  renderConfig();
  loadRouting();
  return true;
}

$("#add-subscription").addEventListener("click", () => openBrowse());
$("#browse-paste").addEventListener("click", () => { $("#dlg-browse").close(); openPaste(null); });

function openPaste(i) {
  state.pasteTarget = i;
  $("#paste-target").textContent = i == null ? "→ nowa subskrypcja" : `→ subskrypcja ${pad2(i + 1)}`;
  $("#paste-sdp").value = i == null ? "" : state.draft.subscriptions[i].sdp || "";
  $("#dlg-paste").showModal();
}

$("#dlg-paste").addEventListener("close", async () => {
  if ($("#dlg-paste").returnValue !== "ok") return;
  const sdp = $("#paste-sdp").value.trim();
  if (state.pasteTarget == null) {
    if (!(await addSubscription({ sdp }))) openPaste(null);
    return;
  }
  const s = state.draft.subscriptions[state.pasteTarget];
  s.sdp = sdp;
  if (!s.name) s.name = sdpInfo(sdp)?.name.slice(0, 40) || "";
  markDirty();
  renderSubscriptions();
});

async function openSdp(n) {
  $("#sdp-target").textContent = `${pad2(n)} ${state.config.outputs[n - 1].name}`;
  const res = await fetch(`/api/outputs/${n}/sdp`, { credentials: "same-origin" });
  $("#sdp-text").value = await res.text();
  if (JSON.stringify(state.draft.outputs[n - 1]) !== JSON.stringify(state.config.outputs[n - 1]) || state.draft.station_name !== state.config.station_name)
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
  master: ["master", "ok"],
  locked: ["slave", "ok"],
  holdover: ["holdover", "warn"],
  listening: ["synchronizacja…", "warn"],
  freerun: ["brak mastera", "err"],
  disabled: ["wyłączony", "err"],
};
const ROLE_LABELS = { auto: "automatyczna", preferred: "preferowany master", slave: "tylko slave" };

function ptpText(p) {
  if (p.state === "master") return "master (ten komputer)";
  if (p.state === "locked") return `slave → ${p.master || "?"}`;
  return (PTP_LABELS[p.state] || ["?"])[0];
}

function setPill(id, text, kind, title) {
  const p = $(id);
  p.classList.remove("ok", "warn", "err");
  if (kind) p.classList.add(kind);
  $("span", p).textContent = text;
  if (title) p.title = title;
}

function applyLive() {
  const live = state.live;
  if (!live) return;
  const [ptpLabel, ptpKind] = PTP_LABELS[live.ptp.state] || ["?", "err"];
  setPill("#pill-ptp", ptpLabel, ptpKind, `Synchronizacja zegara PTP: ${ptpText(live.ptp)}\nRola: ${ROLE_LABELS[live.ptp.role] || live.ptp.role}`);
  setPill("#pill-asio", live.asio.connected ? `połączono · ${live.asio.buffer_frames} próbek` : "brak aplikacji", live.asio.connected ? "ok" : "warn");
  setPill("#pill-win", live.bridge?.connected ? "aktywne" : "brak sesji", live.bridge?.connected ? "ok" : null,
    live.bridge?.connected ? "Audio Windows: BounceAES Audio Bridge działa" : "Audio Windows: brak sesji użytkownika (AoIP i ASIO działają)");

  const outs = new Map(live.outputs.map((d) => [d.i, d]));
  const subs = new Map(live.subscriptions.map((d) => [d.i, d]));
  for (const span of $$("#tab-config [data-meter]")) {
    const [kind, i, side] = span.dataset.meter.split("-");
    const d = (kind === "out" ? outs : subs).get(Number(i));
    setMeter(span, d ? d.db[Number(side)] : -120);
  }
  for (const dot of $$("[data-sig]")) {
    const n = Number(dot.dataset.sig);
    const d = subs.get(n);
    const configured = !!state.config?.subscriptions[n - 1]?.enabled;
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
    ["Stan", ptpText(p)],
    ["Rola", ROLE_LABELS[p.role] || p.role],
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
    ["Stacja", state.status?.station_name || "—"],
    ["Czas działania", fmtUptime(live.uptime_s)],
    ["Aplikacja ASIO", live.asio.connected ? "połączona" : "brak"],
    ["Bufor ASIO", live.asio.connected ? `${live.asio.buffer_frames} próbek (${(live.asio.buffer_frames / 48).toFixed(2).replace(".", ",")} ms)` : "—"],
    ["Audio Windows", live.bridge?.connected ? "aktywne" : "brak sesji użytkownika"],
    ["Karta sieciowa", state.status?.interface_ip || "—"],
    ["Wersja", state.status?.version || "—"],
  ]);
  const cell = (v, bad) => el("td", { class: bad ? "bad" : "" }, fmtNumber(v));
  $("#stats-out-rows").replaceChildren(...live.outputs.map((d) => {
    const c = d.c;
    const src = state.config?.routes[`net-out:${d.i}`];
    return el("tr", {},
      el("td", { class: "c-num" }, pad2(d.i)),
      el("td", {}, state.config?.outputs[d.i - 1]?.name || ""),
      el("td", { class: "src-cell" }, src ? sourceName(src) : "—"),
      cell(c.tx_packets), cell(c.tx_bytes), cell(c.tx_errors, c.tx_errors > 0),
      el("td", {}, el("span", { class: "dot" + (d.tx ? " on" : "") })));
  }));
  $("#stats-sub-rows").replaceChildren(...live.subscriptions.map((d) => {
    const c = d.c;
    return el("tr", {},
      el("td", { class: "c-num" }, pad2(d.i)),
      el("td", {}, state.config?.subscriptions[d.i - 1]?.name || ""),
      cell(c.rx_packets), cell(c.rx_bytes),
      cell(c.rx_underruns, c.rx_underruns > 0), cell(c.rx_overruns, c.rx_overruns > 0), cell(c.rx_seq_errors, c.rx_seq_errors > 0),
      el("td", {}, c.rx_jitter_ms.toFixed(3).replace(".", ",")),
      el("td", {}, el("span", { class: "dot" + (d.sig ? " on" : "") })));
  }));
}

$("#reset-stats").addEventListener("click", async () => {
  await api("/api/stats/reset", { method: "POST" });
  toast("Liczniki wyzerowane");
});

function connectLive() {
  if (state.source) state.source.close();
  const src = new EventSource("/api/live");
  src.addEventListener("live", (e) => {
    const bridgeBefore = state.live?.bridge?.connected;
    state.live = JSON.parse(e.data);
    applyLive();
    if (bridgeBefore !== state.live.bridge?.connected && currentTab() === "routing") renderMatrix();
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
  const [cfg, ifs, st, rt] = await Promise.all([api("/api/config"), api("/api/interfaces"), api("/api/status"), api("/api/routing")]);
  state.config = cfg.data;
  state.draft = clone(cfg.data);
  state.interfaces = ifs.data;
  state.status = st.data;
  state.routing = rt.data;
  state.issues = [];
  setPill("#pill-iface", st.data.interface_ip || "brak", null);
  renderConfig();
  renderMatrix();
  showTab();
  connectLive();
}

(async function init() {
  const { data } = await api("/api/auth-state");
  state.logo = data.logo || null;  // panel/logo.svg or logo.png, when the file is bundled
  renderBrand();
  if (data.logged_in) boot();
  else showAuth();
})();
