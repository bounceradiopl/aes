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
  liveRetry: null,  // timer of the next live feed attempt after it closed for good
  pasteTarget: null,  // null = add a new subscription, number = edit that subscription (0-based)
  collapsed: new Set(),  // "src:net", "dst:asio", ...
  routingTimer: null,
  statsTimer: null,
  destOpen: null,       // id of the dest dialog's destination while open, else null
  destDisabling: null,  // id of a dest whose mix checkbox was just unchecked, pending the "keep" choice
  mixPending: null,     // { dst, sources } — a mix-member edit (gain/mute/membership) not yet confirmed
                        // by the server, kept outside state.routing so the 500 ms routing poll (or an
                        // in-flight PUT's own stale response) can't silently revert it mid-edit.
  mixSending: null,     // id of a destination whose /api/mixes PUT is currently in flight, else null
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
  clearTimeout(state.liveRetry);
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
  if (valid === "stats") startStatsPoll(); else stopStatsPoll();
}
window.addEventListener("hashchange", showTab);

// ---------------------------------------------------------------- routing
function routingSnapshot(v) {
  return JSON.stringify({ s: v.sources.map((n) => [n.id, n.name, n.available, n.note, n.virtual]),
    d: v.dests.map((n) => [n.id, n.name, n.available, n.note, n.virtual, n.mix]), r: v.routes, m: v.mixes });
}

async function loadRouting() {
  try {
    const r = await api("/api/routing");
    if (!r.ok) return;
    const before = state.routing ? routingSnapshot(state.routing) : "";
    const polled = r.data;
    // A mix edit still in flight (or just queued) would otherwise be clobbered by this poll's
    // older snapshot — overlay it back onto the destination it belongs to before adopting the rest.
    overlayMixPending(polled);
    state.routing = polled;
    if (before !== routingSnapshot(polled)) {
      renderMatrix();
      if ($("#dlg-virtual").open) renderVirtual();
      // The open dialog's member rows must not be rebuilt from this poll tick while its
      // destination has a pending/in-flight mix edit: overlayMixPending already keeps the values
      // correct, but replacing the row DOM (e.g. an <input type=range> mid-drag) would still
      // interrupt the user's gesture. Just keep its meters live instead; the matrix above is safe
      // to refresh regardless (no per-member editable controls in it).
      if (state.destOpen) {
        if (state.mixPending?.dst === state.destOpen) applyRoutingMeters();
        else renderDest(state.destOpen);
      }
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
    const dstTitle = `${n.name}\n${n.id}${virtualText(n)}${n.note ? "\n" + n.note : ""}${n.mix ? "\ntryb miksu" : ""}`;
    head2.append(el("th", { class: "dst" + (c.first ? " first" : "") + (n.available ? "" : " unavailable") + (n.mix ? " mix" : ""), title: `${dstTitle}\nKliknij: szczegóły` },
      el("button", { type: "button", class: "dst-btn", "aria-label": `${n.name}: szczegóły`, onclick: () => openDest(n.id) },
        el("span", {}, (n.mix ? "Σ " : "") + n.name))));
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
            n.group === "net"
              ? el("button", { type: "button", class: "src-name src-btn", title: "Kliknij: SDP, usuń", onclick: () => openStream(n.id) }, n.name)
              : el("span", { class: "src-name" }, n.name),
            n.virtual ? el("i", { class: "vtag" }, "wirt.") : null, meter(`r-${n.id}`)));
        for (const c of columns) {
          if (c.placeholder) { row.append(el("td", { class: "cell placeholder first" })); continue; }
          const on = c.node.mix ? (v.mixes[c.node.id] || []).some((s) => s.source === n.id) : v.routes[c.node.id] === n.id;
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
  for (const span of $$("#matrix [data-meter], #dlg-dest [data-meter]")) {
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
  syncRoutes(r.data.routing);
  renderMatrix();
  if ($("#dlg-virtual").open) renderVirtual();
  return true;
}

// The routing view's mixes (dst → members) in the configuration's shape (dst → {sources}).
function configMixes(routing) {
  return Object.fromEntries(Object.entries(routing.mixes || {}).map(([dst, list]) =>
    [dst, { sources: list.map((m) => ({ source: m.source, gain_db: m.gain_db, mute: m.mute })) }]));
}

// Routes and mixes change immediately (routing tab, destination dialog), so both the saved config
// and the draft follow them; a later "Zapisz" never sends them back stale.
function syncRoutes(routing) {
  const mixes = configMixes(routing);
  if (state.config) { state.config.routes = clone(routing.routes); state.config.mixes = clone(mixes); }
  if (state.draft) { state.draft.routes = clone(routing.routes); state.draft.mixes = clone(mixes); }
  if (state.draft) renderOutputs();
}

function mixCountText(members) {
  return `miks (${members.length} ${plural(members.length, "źródło", "źródła", "źródeł")})`;
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
  const mixes = v.mixes || {};
  const inMix = (dst, src) => (mixes[dst] || []).some((m) => m.source === src);
  const feeding = (src) => outputs.filter((o) => v.routes[o.id] === src || inMix(o.id, src))
    .map((o) => (mixes[o.id] ? `${o.name} (miks)` : o.name));
  const option = (value, text, disabled = false) => el("option", { value, disabled }, text);
  // An output in mix mode is not free: sending to it here would silently add or remove a mix
  // member (or clear the mix), so it is listed but cannot be picked; its dialog edits the mix.
  const pickable = outputs.filter((o) => !mixes[o.id]);

  const sendRows = vSources.map((n) => {
    const sel = el("select", { "aria-label": `Wyjście AoIP dla ${n.name}` },
      ...outputs.map((o) => mixes[o.id]
        ? option(o.id, `${o.name} — miks (zmiana w oknie wyjścia)`, true)
        : option(o.id, v.routes[o.id] && v.routes[o.id] !== n.id ? `${o.name} — teraz gra ${sourceLabel(v.routes[o.id])}` : o.name)));
    // preselect an output this card already feeds, else a free one (never a mixed one)
    const pick = pickable.find((o) => v.routes[o.id] === n.id) || pickable.find((o) => !v.routes[o.id]) || pickable[0];
    if (pick) sel.value = pick.id;
    const now = feeding(n.id);
    const send = el("button", { type: "button", class: "mini primary", onclick: () => setRoute(sel.value, n.id) }, "Wysyłaj");
    const stop = el("button", { type: "button", class: "mini", onclick: () => setRoute(sel.value, null) }, "Rozłącz");
    const sync = () => { const fed = v.routes[sel.value] === n.id; send.hidden = fed; stop.hidden = !fed; };
    sel.addEventListener("change", sync);
    sync();
    return el("tr", {},
      el("td", {}, n.name, el("div", { class: "hint" }, n.virtual)),
      el("td", {}, now.length ? now.join(", ") : el("span", { class: "hint" }, "—")),
      el("td", {}, !outputs.length ? el("span", { class: "hint" }, "brak włączonych wyjść")
        : !pickable.length ? el("span", { class: "hint" }, "wszystkie wyjścia AoIP są w trybie miksu") : sel),
      el("td", { class: "actions" }, pickable.length ? send : null, pickable.length ? stop : null));
  });
  const recvRows = vDests.map((d) => {
    if (mixes[d.id]) {
      // in mix mode: "Odbieraj" / "Rozłącz" would toggle or clear members; the destination dialog edits the mix
      return el("tr", {},
        el("td", {}, d.name, el("div", { class: "hint" }, d.virtual)),
        el("td", {}, mixCountText(mixes[d.id])),
        el("td", { colSpan: 2, class: "hint" }, "tryb miksu — zmiana w oknie celu (kliknij nazwę w siatce)"));
    }
    const cur = v.routes[d.id];
    const sel = el("select", { "aria-label": `Źródło dla ${d.name}` }, ...netSources.map((s) => option(s.id, s.name)));
    if (cur && netSources.some((s) => s.id === cur)) sel.value = cur;
    return el("tr", {},
      el("td", {}, d.name, el("div", { class: "hint" }, d.virtual)),
      el("td", {}, cur ? sourceLabel(cur) : el("span", { class: "hint" }, "—")),
      el("td", {}, netSources.length ? sel : el("span", { class: "hint" }, "brak subskrypcji")),
      el("td", { class: "actions" },
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
    const mixMembers = state.routing?.mixes?.[`net-out:${n}`];
    const row = el("tr", { class: o.enabled ? "" : "off" },
      el("td", { class: "c-del" }, state.config.outputs[i].enabled ? deleteButton(`net-out:${n}`, `Usuń wyjście ${pad2(n)}`) : null),
      el("td", { class: "c-num" }, pad2(n)),
      el("td", {}, el("input", { type: "checkbox", checked: o.enabled, "aria-label": `Wyjście ${pad2(n)} aktywne`,
        onchange: (e) => { set("enabled", e.target.checked); row.classList.toggle("off", !e.target.checked); } })),
      el("td", {}, el("input", { type: "text", value: o.name, maxLength: 40, dataset: { field: "name" }, "aria-label": `Nazwa wyjścia ${pad2(n)}`, oninput: (e) => set("name", e.target.value) })),
      el("td", {}, el("div", { class: "inline" },
        el("input", { type: "text", class: "addr", value: o.address, dataset: { field: "address" }, "aria-label": `Adres wyjścia ${pad2(n)}`, oninput: (e) => set("address", e.target.value.trim()) }),
        el("button", { type: "button", class: "mini", title: "SDP wyjścia: pokaż, kopiuj, pobierz plik", onclick: () => openSdp(n) }, "SDP"))),
      el("td", {}, ptime),
      el("td", {}, el("input", { type: "checkbox", checked: o.keep_active, "aria-label": `Utrzymuj wyjście ${pad2(n)}`, onchange: (e) => set("keep_active", e.target.checked) })),
      el("td", {}, mixMembers
        ? el("span", {}, mixCountText(mixMembers))
        : el("a", { class: "route-link" + (src ? "" : " none"), href: "#routing", title: src || "" }, src ? sourceName(src) : "brak")),
      el("td", {}, meter(`out-${n}`)));
    tbody.append(row);
    issueRow(tbody, row, `outputs[${n}].`, 9);
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
    const saved = state.config.subscriptions[i];
    const row = el("tr", { class: s.enabled ? "" : "off" },
      el("td", { class: "c-del" }, saved.enabled || saved.sdp ? deleteButton(`net-in:${n}`, `Usuń subskrypcję ${pad2(n)}`) : null),
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
      el("td", {}, meter(`sub-${n}`)));
    tbody.append(row);
    issueRow(tbody, row, `subscriptions[${n}].`, 8);
  });
  if (!shown) tbody.append(el("tr", {}, el("td", { colSpan: 8, class: "hint" }, "Brak subskrypcji. „Dodaj źródło…” wybiera strumień z sieci (SAP) albo przyjmuje wklejone SDP.")));
  applyLive();
}

function deleteButton(id, label) {
  return el("button", { type: "button", class: "mini danger", title: `${label} (od razu, po potwierdzeniu)`, "aria-label": label,
    onclick: () => openStream(id, true) }, "Usuń");
}

// A change the server already saved (add / remove): keep unsaved edits of other rows.
function applyServerChange(fresh, kind, i) {
  const dirty = draftDirty();
  state.config = fresh;
  if (dirty) {
    state.draft[kind][i] = clone(fresh[kind][i]);
    state.draft.routes = clone(fresh.routes);
    state.draft.mixes = clone(fresh.mixes);
  } else {
    state.draft = clone(fresh);
  }
  renderConfig();
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

// Unsaved edits in Konfiguracja. Mixes are not edited there (they change at once in Routing and are
// never sent with "Zapisz"), so they never make the draft dirty.
function draftDirty() {
  const strip = (c) => JSON.stringify({ ...c, mixes: undefined });
  return strip(state.draft) !== strip(state.config);
}

function markDirty() {
  $("#savebar").classList.toggle("hidden", !draftDirty());
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
  delete body.mixes;  // mixes change only through Routing (PUT /api/mixes); the server ignores them here too
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
  applyServerChange(r.data.config, "subscriptions", r.data.created - 1);
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
  $("#sdp-kind").textContent = "SDP wyjścia";
  $("#sdp-hint").textContent = "Wczytaj plik w odbiorniku AES67 (np. Orban, Omnia), jeśli nie korzysta z SAP.";
  $("#sdp-target").textContent = `${pad2(n)} ${state.config.outputs[n - 1].name}`;
  const res = await fetch(`/api/outputs/${n}/sdp`, { credentials: "same-origin" });
  $("#sdp-text").value = await res.text();
  if (JSON.stringify(state.draft.outputs[n - 1]) !== JSON.stringify(state.config.outputs[n - 1]) || state.draft.station_name !== state.config.station_name)
    toast("SDP pokazuje zapisaną konfigurację — zapisz zmiany, aby je uwzględnić", "warn");
  $("#dlg-sdp").showModal();
}

function openSourceSdp(n) {
  $("#sdp-kind").textContent = "SDP źródła";
  $("#sdp-hint").textContent = "Opis strumienia, który odbiera ta subskrypcja.";
  $("#sdp-target").textContent = `${pad2(n)} ${state.config.subscriptions[n - 1].name}`;
  $("#sdp-text").value = state.config.subscriptions[n - 1].sdp || "";
  $("#dlg-sdp").showModal();
}

// Saves the SDP as "<station> - <output>.sdp", the name receivers show for the session.
function downloadSdp(text) {
  const session = (sdpInfo(text)?.name || "BounceAES").replace(/\s*\/\s*/g, " - ").replace(/[\\/:*?"<>|]+/g, "_").trim();
  const url = URL.createObjectURL(new Blob([text.replace(/\r?\n/g, "\r\n")], { type: "application/sdp" }));
  const a = el("a", { href: url, download: `${session || "BounceAES"}.sdp` });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

async function copyText(text, fallbackInput) {
  try {
    await navigator.clipboard.writeText(text);
    toast("Skopiowano SDP");
  } catch {
    fallbackInput.select();
    document.execCommand("copy");
  }
}

$("#sdp-download").addEventListener("click", () => downloadSdp($("#sdp-text").value));

$("#sdp-copy").addEventListener("click", () => copyText($("#sdp-text").value, $("#sdp-text")));

// ---------------------------------------------------------------- send to a device (new output)
function sendStep(n) {
  $("#send-step1").hidden = n !== 1;
  $("#send-step2").hidden = n !== 2;
  $("#send-create").hidden = n !== 1;
  $("#send-copy").hidden = n !== 2;
  $("#send-download").hidden = n !== 2;
  $("#send-cancel").textContent = n === 1 ? "Anuluj" : "Zamknij";
}

async function openSend() {
  if (!state.routing) await loadRouting();
  const sources = state.routing?.sources || [];
  const groups = GROUPS.map(([g, label]) => {
    const list = sources.filter((n) => n.group === g);
    return list.length ? el("optgroup", { label }, ...list.map((n) => el("option", { value: n.id }, `${label}: ${n.name}${n.available ? "" : " (niedostępne)"}`))) : null;
  }).filter(Boolean);
  const sel = $("#send-source");
  sel.replaceChildren(el("option", { value: "" }, "— wybiorę później w siatce —"), ...groups);
  sel.value = sources.find((n) => n.group === "asio")?.id || "";
  $("#send-ptime").replaceChildren(...PTIMES.map(([p, t]) => el("option", { value: p, selected: p === 1000 }, t)));
  $("#send-name").value = "";
  $("#send-address").value = "";
  $("#send-error").textContent = "";
  sendStep(1);
  $("#dlg-send").showModal();
  $("#send-name").focus();
}

$("#send-create").addEventListener("click", async () => {
  const name = $("#send-name").value.trim();
  if (!name) { $("#send-error").textContent = "Podaj nazwę, np. „Orban Warszawa”."; $("#send-name").focus(); return; }
  const body = { name, source: $("#send-source").value, ptime_us: Number($("#send-ptime").value), address: $("#send-address").value.trim() };
  const r = await api("/api/outputs", { method: "POST", body: JSON.stringify(body) });
  if (!r.ok) { $("#send-error").textContent = issuesText(r, "Nie można utworzyć wyjścia"); return; }
  const n = r.data.created;
  applyServerChange(r.data.config, "outputs", n - 1);
  state.routing = r.data.routing;
  renderMatrix();
  for (const x of r.data.notices || []) toast(x, "warn");
  $("#send-made").textContent = `${pad2(n)} „${name}” (${r.data.config.outputs[n - 1].address})`;
  $("#send-sdp").value = r.data.sdp;
  sendStep(2);
});
// Enter in a field creates the output; Cancel / Close always leaves the dialog
for (const id of ["#send-name", "#send-address"])
  $(id).addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); $("#send-create").click(); } });
$("#send-cancel").addEventListener("click", () => $("#dlg-send").close());
$("#send-download").addEventListener("click", () => downloadSdp($("#send-sdp").value));
$("#send-copy").addEventListener("click", () => copyText($("#send-sdp").value, $("#send-sdp")));
$("#send-output").addEventListener("click", openSend);
$("#add-output").addEventListener("click", openSend);

// ---------------------------------------------------------------- stream details and removal
function plural(n, one, few, many) {
  if (n === 1) return one;
  const d = n % 10, t = n % 100;
  return d >= 2 && d <= 4 && (t < 12 || t > 14) ? few : many;
}

function destName(id) { return state.routing?.dests.find((d) => d.id === id)?.name || id; }

function streamInfo(id) {
  const [kind, num] = id.split(":");
  const n = Number(num);
  const routes = state.routing?.routes || state.config.routes;
  if (kind === "net-in") {
    const sub = state.config.subscriptions[n - 1];
    const info = sdpInfo(sub.sdp) || { name: "", addr: "" };
    const feeds = Object.entries(routes).filter(([, src]) => src === id).map(([dst]) => destName(dst));
    return {
      kind, n, feeds, name: sub.name || info.name || `Subskrypcja ${pad2(n)}`, title: "Subskrypcja", remove: "Usuń źródło",
      facts: [["Strumień", info.name || "—"], ["Adres", info.addr || "—"],
        ["Opóźnienie odbioru", `${(sub.latency_us / 1000).toLocaleString("pl-PL")} ms`], ["Zasila", feeds.length ? feeds.join(", ") : "nic"]],
    };
  }
  const o = state.config.outputs[n - 1];
  const src = routes[id];
  return {
    kind, n, feeds: [], name: o.name || `Wyjście ${pad2(n)}`, title: "Wyjście AoIP", remove: "Usuń wyjście",
    facts: [["Adres", o.address], ["Pakiet", PTIMES.find(([p]) => p === o.ptime_us)?.[1] || "—"],
      ["Nadaje", src ? sourceLabel(src) : "nic — połącz źródło w siatce"]],
  };
}

function openStream(id, confirmFirst = false) {
  const info = streamInfo(id);
  const dlg = $("#dlg-stream");
  $("#stream-title").replaceChildren(`${info.title} `, el("small", {}, `${pad2(info.n)} · ${info.name}`));
  $("#stream-facts").replaceChildren(...info.facts.flatMap(([k, v]) => [el("dt", {}, k), el("dd", {}, v)]));
  const showActions = () => {
    $("#stream-confirm").hidden = true;
    $("#stream-actions").replaceChildren(
      el("button", { type: "button", class: "danger", onclick: showConfirm }, info.remove),
      el("span", { class: "spacer" }),
      el("button", { type: "button", class: "ghost", onclick: () => { dlg.close(); if (info.kind === "net-in") openSourceSdp(info.n); else openSdp(info.n); } },
        info.kind === "net-in" ? "SDP źródła…" : "SDP / plik .sdp…"),
      el("button", { value: "cancel", class: "primary" }, "Zamknij"));
  };
  const showConfirm = () => {
    const links = info.feeds.length;
    $("#stream-confirm").textContent = info.kind === "net-in"
      ? `Usunąć źródło „${info.name}”?${links ? ` Rozłączy to ${links} ${plural(links, "połączenie", "połączenia", "połączeń")}: ${info.feeds.join(", ")}.` : ""}`
      : `Usunąć wyjście „${info.name}”? Przestanie nadawać, a odbiorniki (np. Orban) stracą ten sygnał.`;
    $("#stream-confirm").hidden = false;
    $("#stream-actions").replaceChildren(
      el("span", { class: "spacer" }),
      el("button", { type: "button", class: "ghost", onclick: () => (confirmFirst ? dlg.close() : showActions()) }, "Anuluj"),
      el("button", { type: "button", class: "danger solid", onclick: () => removeStream(info) }, "Tak, usuń"));
  };
  if (confirmFirst) showConfirm(); else showActions();
  dlg.showModal();
}

async function removeStream(info) {
  const r = await api(`/api/${info.kind === "net-in" ? "subscriptions" : "outputs"}/${info.n}`, { method: "DELETE" });
  if (!r.ok) { toast(issuesText(r, "Nie można usunąć"), "err"); return; }
  $("#dlg-stream").close();
  applyServerChange(r.data.config, info.kind === "net-in" ? "subscriptions" : "outputs", info.n - 1);
  state.routing = r.data.routing;
  renderMatrix();
  toast(`Usunięto ${info.kind === "net-in" ? "źródło" : "wyjście"} „${info.name}”`);
}

// ---------------------------------------------------------------- destination dialog, mix mode
function groupLabel(group) { return (GROUPS.find(([g]) => g === group) || [group, group])[1]; }

// facts shown above the "Miks" toggle: AoIP outputs reuse streamInfo (address, packet, current
// source — dropped once mixing, since the rows below cover it); other destinations just say what they are.
function destFacts(n) {
  if (n.group === "net") {
    const info = streamInfo(n.id);
    return n.mix ? info.facts.filter(([k]) => k !== "Nadaje") : info.facts;
  }
  const types = { asio: "Wejście ASIO (czyta aplikacja ASIO)", win: "Odtwarzanie Windows" };
  return [["Typ", types[n.group] || n.group]];
}

function fmtDb(v) { return `${Number(v).toFixed(1).replace(".", ",")} dB`; }
function fmtMs(ms) { return `${Number(ms).toLocaleString("pl-PL", { maximumFractionDigits: 1 })} ms`; }

function openDest(id) {
  state.destOpen = id;
  state.destDisabling = null;
  renderDest(id);
  $("#dlg-dest").showModal();
}

function renderDest(id) {
  const dlg = $("#dlg-dest");
  const n = state.routing?.dests.find((d) => d.id === id);
  if (!n) { if (dlg.open) dlg.close(); return; }
  $("#dest-title").replaceChildren(`${groupLabel(n.group)} `, el("small", {}, n.name));
  $("#dest-facts").replaceChildren(...destFacts(n).flatMap(([k, v]) => [el("dt", {}, k), el("dd", {}, v)]));
  const box = $("#dest-mix");
  box.checked = n.mix && state.destDisabling !== id;
  box.onchange = () => onMixToggle(id);
  $("#dest-mix-body").hidden = !n.mix;
  if (n.mix) { if (state.destDisabling === id) renderDisableChooser(id); else renderMixRows(id); }
  if (n.group === "net") renderDestNetActions(id); else renderDestPlainActions();
}

$("#dlg-dest").addEventListener("close", () => { state.destOpen = null; state.destDisabling = null; });

// AoIP outputs: same SDP / Usuń flow as the old per-output dialog (dlg-stream), embedded here.
function renderDestNetActions(id) {
  const info = streamInfo(id);
  const dlg = $("#dlg-dest");
  const showActions = () => {
    $("#dest-confirm").hidden = true;
    $("#dest-actions").replaceChildren(
      el("button", { type: "button", class: "danger", onclick: showConfirm }, info.remove),
      el("span", { class: "spacer" }),
      el("button", { type: "button", class: "ghost", onclick: () => { dlg.close(); openSdp(info.n); } }, "SDP / plik .sdp…"),
      el("button", { value: "cancel", class: "primary" }, "Zamknij"));
  };
  const showConfirm = () => {
    $("#dest-confirm").textContent = `Usunąć wyjście „${info.name}”? Przestanie nadawać, a odbiorniki (np. Orban) stracą ten sygnał.`;
    $("#dest-confirm").hidden = false;
    $("#dest-actions").replaceChildren(
      el("span", { class: "spacer" }),
      el("button", { type: "button", class: "ghost", onclick: showActions }, "Anuluj"),
      el("button", { type: "button", class: "danger solid", onclick: async () => { await removeStream(info); dlg.close(); } }, "Tak, usuń"));
  };
  showActions();
}

function renderDestPlainActions() {
  $("#dest-confirm").hidden = true;
  $("#dest-actions").replaceChildren(el("span", { class: "spacer" }), el("button", { value: "cancel", class: "primary" }, "Zamknij"));
}

function renderMixRows(id) {
  const members = state.routing.mixes[id] || [];
  // mix mode always plays through the mix bus, even with one member: +2 ms over its slowest source
  $("#dest-mix-delay").textContent = `Opóźnienie miksu: ${fmtMs(state.routing.mix_delay_ms[id] || 0)}`;
  const tbody = $("#dest-mix-rows");
  if (!members.length) {
    tbody.replaceChildren(el("tr", {}, el("td", { colSpan: 5, class: "hint" }, "Brak źródeł — kliknij kratkę w siatce, aby dodać.")));
    return;
  }
  tbody.replaceChildren(...members.map((m, i) => {
    const val = el("span", {}, fmtDb(m.gain_db));
    const slider = el("input", { type: "range", min: -60, max: 12, step: 0.5, value: m.gain_db,
      "aria-label": `Głośność: ${sourceLabel(m.source)}`,
      oninput: (e) => { val.textContent = fmtDb(e.target.value); queueMixUpdate(id, i, { gain_db: Number(e.target.value) }); } });
    const mute = el("button", { type: "button", class: "mini", "aria-pressed": String(m.mute), title: m.mute ? "Włącz z powrotem" : "Wycisz",
      onclick: () => applyMixUpdate(id, i, { mute: !m.mute }) }, "Wycisz");
    const remove = el("button", { type: "button", class: "mini", "aria-label": `Usuń ${sourceLabel(m.source)} z miksu`, onclick: () => removeMixMember(id, i) }, "×");
    return el("tr", {},
      el("td", {}, sourceLabel(m.source)),
      el("td", {}, el("span", { class: "range-row" }, slider, val)),
      el("td", {}, mute),
      el("td", {}, meter(`r-${m.source}`)),
      el("td", {}, remove));
  }));
  applyRoutingMeters();
}

// Turning the mix off with ≥ 2 members: ask which one stays as the single route, instead of
// closing the body — the chooser replaces the rows while the mix (still active server-side) waits.
function renderDisableChooser(id) {
  const members = state.routing.mixes[id] || [];
  $("#dest-mix-delay").textContent = "Wyłączanie miksu: wybierz źródło, które zostanie pojedynczym połączeniem.";
  const sel = el("select", { "aria-label": "Źródło do zachowania" }, ...members.map((m) => el("option", { value: m.source }, sourceLabel(m.source))));
  $("#dest-mix-rows").replaceChildren(el("tr", {}, el("td", { colSpan: 5 },
    el("div", { class: "mix-off" }, sel,
      el("button", { type: "button", class: "mini", onclick: () => { state.destDisabling = null; renderDest(id); } }, "Anuluj"),
      el("button", { type: "button", class: "mini primary", onclick: () => finishDisableMix(id, sel.value) }, "Zostaw to źródło")))));
}

async function finishDisableMix(id, keep) {
  state.destDisabling = null;
  await setMix(id, false, undefined, keep);
}

function onMixToggle(id) {
  if ($("#dest-mix").checked) { state.destDisabling = null; setMix(id, true); return; }
  const members = state.routing.mixes[id] || [];
  if (members.length >= 2) { state.destDisabling = id; renderDest(id); return; }
  state.destDisabling = null;
  setMix(id, false);
}

// The member list a new edit for `id` should build on: the outstanding pending edit for this
// destination, if there is one (so a second edit before the first is confirmed keeps building on
// the first, not on whatever the last poll happened to see), else the server's last known list.
function mixPendingSources(id) {
  if (state.mixPending?.dst === id) return state.mixPending.sources.map((m) => ({ ...m }));
  return (state.routing.mixes[id] || []).map((m) => ({ source: m.source, gain_db: m.gain_db, mute: m.mute }));
}

// Replaces a pending destination's mix members in a freshly fetched/returned `routing` with the
// pending edit's own copy — merge, not replace — so a snapshot seen while that edit is unconfirmed
// (a poll result, or an in-flight PUT's own now-stale response) can't show, or go on to send, a
// reverted value.
function overlayMixPending(routing) {
  if (!state.mixPending || !routing.mixes) return;
  routing.mixes[state.mixPending.dst] = state.mixPending.sources.map((m) => ({ ...m }));
}

let mixDebounce = null;
function queueMixUpdate(id, index, patch) {
  const sources = mixPendingSources(id);
  if (!sources[index]) return;
  Object.assign(sources[index], patch);
  state.mixPending = { dst: id, sources };
  clearTimeout(mixDebounce);
  mixDebounce = setTimeout(() => sendMix(id), 150);
}

function applyMixUpdate(id, index, patch) {
  const sources = mixPendingSources(id);
  if (!sources[index]) return;
  Object.assign(sources[index], patch);
  state.mixPending = { dst: id, sources };
  clearTimeout(mixDebounce);
  sendMix(id);
}

function removeMixMember(id, index) {
  const sources = mixPendingSources(id);
  if (index < 0 || index >= sources.length) return;
  sources.splice(index, 1);
  state.mixPending = { dst: id, sources };
  clearTimeout(mixDebounce);
  sendMix(id);
}

// Sends the pending edit for `id`. At most one /api/mixes PUT per destination is ever in flight at
// once: a send that lands while one is already outstanding just returns — sendMixNow's own
// completion below notices the (by then newer) pending edit and chains it. Without this, a slider's
// debounced send and an immediate mute/remove send for the same destination could overlap and reach
// the server out of order, each overwriting the other's full member list.
function sendMix(id) {
  if (state.mixSending === id) return;
  return sendMixNow(id);
}

async function sendMixNow(id) {
  const pending = state.mixPending?.dst === id ? state.mixPending : null;
  const sources = pending ? pending.sources : (state.routing.mixes[id] || []).map((m) => ({ source: m.source, gain_db: m.gain_db, mute: m.mute }));
  state.mixSending = id;
  await setMix(id, true, sources, undefined, pending);
  state.mixSending = null;
  if (state.mixPending?.dst === id) sendMixNow(id);  // a newer edit queued while this PUT was in flight
}

// `pendingRef` is the exact state.mixPending object this call was sent for (only sendMixNow passes
// one). On return, if state.mixPending is still that same object, the edit is confirmed and cleared;
// if it has since been replaced (a newer edit queued while this PUT was in flight), this response is
// for stale data — the newer edit is kept (overlaid back onto the server's answer here) and
// sendMixNow's caller sends it next.
async function setMix(dst, enabled, sources, keep, pendingRef) {
  const body = { dst, enabled };
  if (sources) body.sources = sources;
  if (keep) body.keep = keep;
  const r = await api("/api/mixes", { method: "PUT", body: JSON.stringify(body) });
  if (!r.ok) {
    toast(issuesText(r, "Nie można zmienić miksu"), "err");
    if (pendingRef && state.mixPending === pendingRef) state.mixPending = null;
    loadRouting();
    return false;
  }
  for (const x of r.data.notices || []) toast(x, "warn");
  const routing = r.data.routing;
  if (pendingRef) {
    if (state.mixPending === pendingRef) state.mixPending = null;
    else overlayMixPending(routing);
  } else if (state.mixPending?.dst === dst) {
    // an enable/disable/keep call for a destination that also has a leftover pending member edit
    // (e.g. the mix was turned off while a slider edit was still in flight): the mix identity just
    // changed server-side, so the pending edit no longer applies.
    state.mixPending = null;
  }
  state.routing = routing;
  syncRoutes(routing);
  renderMatrix();
  if ($("#dlg-dest").open) renderDest(dst);
  return true;
}

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

// /api/status carries counters the SSE live feed does not (e.g. mixer_late); poll it only on the
// Statystyki tab, since the rest of that tab already follows the live feed.
async function loadStatus() {
  const r = await api("/api/status");
  if (r.ok) { state.status = r.data; renderStats(); }
}

function startStatsPoll() {
  if (state.statsTimer) return;
  loadStatus();
  state.statsTimer = setInterval(loadStatus, 2000);
}

function stopStatsPoll() {
  clearInterval(state.statsTimer);
  state.statsTimer = null;
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
    ["Miks: spóźnienia", fmtNumber(state.status?.mixer_late || 0)],
    ["Wersja", state.status?.version || "—"],
  ]);
  const cell = (v, bad) => el("td", { class: bad ? "bad" : "" }, fmtNumber(v));
  $("#stats-out-rows").replaceChildren(...live.outputs.map((d) => {
    const c = d.c;
    const id = `net-out:${d.i}`;
    const src = state.config?.routes[id];
    const mix = state.routing?.mixes ? state.routing.mixes[id] : state.config?.mixes?.[id]?.sources;
    return el("tr", {},
      el("td", { class: "c-num" }, pad2(d.i)),
      el("td", {}, state.config?.outputs[d.i - 1]?.name || ""),
      el("td", { class: "src-cell" }, mix ? mixCountText(mix) : src ? sourceName(src) : "—"),
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

// The feed ends on logout (here or in another tab), session expiry, a password change made
// elsewhere or a service restart. The browser reconnects by itself unless the answer was an error
// (401); after every break the panel asks whether it is still logged in: if not, the login screen.
function connectLive() {
  if (state.source) state.source.close();
  clearTimeout(state.liveRetry);
  const src = new EventSource("/api/live");
  src.addEventListener("live", (e) => {
    const bridgeBefore = state.live?.bridge?.connected;
    state.live = JSON.parse(e.data);
    applyLive();
    if (bridgeBefore !== state.live.bridge?.connected && currentTab() === "routing") renderMatrix();
  });
  src.onerror = async () => {
    setPill("#pill-ptp", "brak połączenia z usługą", "err");
    if (state.source !== src) return;
    try {
      const { data } = await api("/api/auth-state");
      if (state.source !== src) return;
      if (data && !data.logged_in) { showAuth(); return; }
    } catch { /* the service is not answering: the browser tries again */ }
    if (src.readyState !== EventSource.CLOSED || state.source !== src || $("#app").classList.contains("hidden")) return;
    state.source = null;  // closed for good (an error answer) while still logged in: try again
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
