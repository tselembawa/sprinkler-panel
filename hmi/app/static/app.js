"use strict";

// ---------------------------------------------------------------- labels
const CAT_SV = { A: "A-larm", B: "B-larm", C: "C-larm", DRIFT: "Drift", FAULT: "Fel" };
const DEVICE_SV = {
  SERVICEVENTIL: "Serviceventil", UNDERSEKTIONSVENTIL: "Undersektionsventil", LARMVENTIL: "Larmventil", PRESSOSTAT: "Larmpressostat",
  FLODESVAKT: "Flödesvakt", PUMP_RUN: "Pump i drift", PUMP_FAULT: "Pumpfel",
  TEMP: "Låg temperatur", GENERIC: "Övrigt",
};
const STATE_SV = { NORMAL: "Normal", PENDING: "Fördröjning", ALARM: "Larm", ACKED: "Kvitterat", LATCHED: "Väntar återställning" };
const FAULT_SV = { OPEN: "Avbrott", SHORT: "Kortslutning", MODULE_LOST: "Modul saknas" };
const ERR_SV = {
  OUT_OF_RANGE: "värdet är utanför tillåtet område", DUPLICATE_ID: "larm-ID används redan",
  DUPLICATE_ADDRESS: "adressen används redan av en annan punkt", INVALID: "ogiltigt val",
  DELAY_NOT_ALLOWED: "fördröjningen får vara högst 60 s", TOO_LONG: "för lång text (max 20 tecken)",
};
const FIELD_SV = {
  id: "Larm-ID", line: "Linje", module: "Modul", channel: "Ingång", category: "Kategori",
  device_type: "Typ av enhet", contact: "Kontakt", delay_s: "Fördröjning", lcd1: "LCD rad 1", lcd2: "LCD rad 2",
};

// ---------------------------------------------------------------- state
const S = {
  meta: null, status: null, points: [], pages: [],
  pageId: null, editing: false, tab: "overview",
  seenAlarms: new Set(), cache: {}, editOriginalId: null,
};

const $ = (sel, root = document) => root.querySelector(sel);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const addr = (p) => `L${p.line}-M${String(p.module).padStart(2, "0")}-IN${p.channel}`;
const fmtTime = (t) => new Date(t * 1000).toLocaleString("sv-SE");

async function api(path, opts = {}) {
  const o = { ...opts };
  if (o.json !== undefined) {
    o.body = JSON.stringify(o.json);
    o.headers = { "Content-Type": "application/json" };
    o.method = o.method || "POST";
    delete o.json;
  }
  const r = await fetch(path, o);
  let data = null;
  try { data = await r.json(); } catch { /* not json */ }
  if (!r.ok) {
    const e = new Error((data && data.error) || `HTTP ${r.status}`);
    e.data = data; e.status = r.status;
    throw e;
  }
  return data;
}

function toast(msg, kind = "") {
  const t = $("#toast");
  t.textContent = msg;
  t.className = `toast ${kind}`;
  t.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (t.hidden = true), 3500);
}

function changed(key, value) {
  const s = JSON.stringify(value);
  if (S.cache[key] === s) return false;
  S.cache[key] = s;
  return true;
}

const isTech = () => !!(S.status && S.status.key_tech);
function requireTech() {
  if (isTech()) return true;
  toast("Vrid nyckelbrytaren till läge TEKNIKER", "bad");
  return false;
}

// ---------------------------------------------------------------- point visuals
const IN_ALARM = new Set(["ALARM", "ACKED", "LATCHED"]);

function visual(p) {
  const rt = p.runtime;
  if (!rt) return { cls: "", text: "Ej i centralen", rank: 99, active: false };
  if (!rt.enabled) return { cls: "disabled", text: "Frånkopplad", rank: 80, active: true, kind: "OFF" };
  const kind = rt.display_category;
  if (IN_ALARM.has(rt.state)) {
    const unacked = !rt.acked && kind !== "DRIFT";
    let text = rt.state === "LATCHED" ? "Orsak borta – väntar återställning" : (unacked ? "Larm – ej kvitterat" : "Kvitterat, orsak kvarstår");
    if (kind === "DRIFT") text = "Aktiv";
    if (rt.fault) text += ` · ${FAULT_SV[rt.fault]}`;
    const rank = { A: 0, FAULT: 2, B: 3, C: 4, DRIFT: 9 }[kind] ?? 5;
    const cls = rt.state === "LATCHED" ? "latched" : `alarm-${kind}${unacked ? " ring" : ""}`;
    return { cls, text, rank: rank + (unacked ? 0 : 0.5), active: true, kind, unacked };
  }
  if (rt.state === "PENDING") {
    return { cls: "pending", text: `Fördröjning ${Math.ceil(rt.pending_left_s ?? 0)} s`, rank: 6, active: true, kind: "PENDING" };
  }
  if (rt.fault) return { cls: "fault", text: FAULT_SV[rt.fault], rank: 1, active: true, kind: "FAULT", unacked: false };
  return { cls: "", text: "Normal", rank: 99, active: false };
}

function markerPageFor(pointId) {
  for (const pg of S.pages) if (pg.markers.some((m) => m.point_id === pointId)) return pg.id;
  return null;
}

// ---------------------------------------------------------------- top bar
function renderTop() {
  const s = S.status;
  const setLed = (id, cls) => { $(id).className = `led ${cls}`; };
  setLed("#led-a", s.leds.red_a ? "on-red flash" : "");
  setLed("#led-b", s.leds.yellow_b === "flash" ? "on-amber flash" : s.leds.yellow_b === "steady" ? "on-amber" : "");
  setLed("#led-fault", s.leds.yellow_fault ? "on-amber" : "");
  setLed("#led-drift", s.leds.green_power ? "on-green" : "");

  const chip = $("#key-chip");
  chip.textContent = s.key_tech ? "Tekniker" : "Användare";
  chip.className = `chip ${s.key_tech ? "tech" : ""}`;
  $("#btn-reset").disabled = !s.key_tech;

  renderStatusBand();
}

// ---------------------------------------------------------------- status band
const ICONS = {
  ok: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="M7.5 12.5l3 3 6-6.5"/></svg>',
  fire: '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M12 2c1 3.5-1.5 5-1.5 7.5 0 1.4 1 2.5 2.3 2.5 1.6 0 2.2-1.5 2-3.5 2.2 1.6 3.7 4.3 3.7 7.1C18.5 19.6 15.6 22 12 22s-6.5-2.4-6.5-6.4C5.5 10 12 7.5 12 2z"/></svg>',
  warn: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3l10 18H2L12 3z"/><path d="M12 10v5M12 18.5v.01"/></svg>',
  info: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><circle cx="12" cy="12" r="10"/><path d="M12 11v6M12 7.5v.01"/></svg>',
  lost: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0M12 19v.01M3 3l18 18"/></svg>',
};

function setBand(kind, icon, title, sub, hint = "", unacked = false) {
  const el = $("#statusband");
  el.className = `statusband sb-${kind} ${unacked ? "unacked" : ""}`;
  if ($("#sb-icon").dataset.icon !== icon) { $("#sb-icon").innerHTML = ICONS[icon]; $("#sb-icon").dataset.icon = icon; }
  $("#sb-title").textContent = title;
  $("#sb-sub").textContent = sub;
  $("#sb-hint").textContent = hint;
  document.body.classList.toggle("fire", kind === "fire");
  document.body.classList.toggle("unacked", kind === "fire" && unacked);
}

function namesOf(list) {
  const names = list.map((x) => x.p.name);
  return names.length > 2 ? `${names.slice(0, 2).join(", ")} +${names.length - 2}` : names.join(", ");
}

function renderStatusBand() {
  const s = S.status;
  const items = S.points.map((p) => ({ p, v: visual(p) })).filter((x) => x.v.active);
  const of = (kind) => items.filter((x) => x.v.kind === kind);
  const lost = s.lost_modules || [];
  const hint = s.buzzer ? "Tryck Kvittera" : "";

  const fire = of("A");
  if (s.relays.K1_A || fire.length) {
    const unacked = fire.some((x) => x.v.unacked) || items.some((x) => x.p.category === "A" && x.p.runtime && x.p.runtime.state === "LATCHED" && !x.p.runtime.acked);
    const latchedOnly = fire.length && fire.every((x) => x.p.runtime.state === "LATCHED");
    setBand("fire", "fire", "Brandlarm",
      `${namesOf(fire)}${latchedOnly ? " – orsak borta, väntar återställning" : " – vattenflöde i sprinklersystemet"}`,
      unacked ? "Tryck Kvittera" : (latchedOnly ? "Återställ med nyckel" : "Kvitterat"), unacked);
    return;
  }
  const faults = of("FAULT");
  if (faults.length || lost.length || !s.relays.K3_FAULT_OK) {
    const n = faults.length + lost.length;
    const what = faults.length ? `${namesOf(faults)}: ${faults[0].v.text}` : `Linje ${lost[0][0]}, modul ${lost[0][1]} saknas`;
    setBand("fault", "warn", n > 1 ? `Fel (${n})` : "Fel", what, hint);
    return;
  }
  const b = of("B");
  if (b.length) { setBand("b", "warn", b.length > 1 ? `B-larm (${b.length})` : "B-larm", namesOf(b), hint, b.some((x) => x.v.unacked)); return; }
  const c = of("C");
  if (c.length) { setBand("c", "info", c.length > 1 ? `C-larm (${c.length})` : "C-larm", `Kvitterat, orsak kvarstår: ${namesOf(c)}`, hint); return; }
  const pending = of("PENDING");
  if (pending.length) { setBand("pending", "info", "Fördröjning pågår", `${namesOf(pending)} – ${pending[0].v.text.toLowerCase()}`, hint); return; }
  const off = of("OFF");
  if (off.length) { setBand("off", "info", "Frånkopplad punkt", `${off.length} punkt(er) frånkopplade: ${namesOf(off)}`, hint); return; }
  if (!s.config_in_sync) { setBand("off", "info", "Kontrollera konfiguration", "Centralen kör en annan konfiguration än den sparade", hint); return; }
  setBand("ok", "ok", "Allt normalt", `Inga larm eller fel · ${S.points.length} larmpunkter övervakas`, hint);
}

// ---------------------------------------------------------------- overview
function renderOverview() {
  const c = S.status.counts;
  const tiles = [
    ["a", "A-larm", c.A], ["b", "B-larm", c.B], ["c", "C-larm", c.C],
    ["f", "Fel", c.FAULT], ["d", "Driftindikering", c.DRIFT], ["o", "Frånkopplade", c.DISABLED],
  ];
  if (changed("summary", c)) {
    $("#summary").innerHTML = tiles.map(([k, l, n]) =>
      `<div class="tile ${k} ${n ? "hot" : ""}"><div class="n">${n}</div><div class="l">${l}</div></div>`).join("");
  }

  const items = S.points.map((p) => ({ p, v: visual(p) })).filter((x) => x.v.active);
  items.sort((a, b) => a.v.rank - b.v.rank || a.p.id - b.p.id);
  const lost = S.status.lost_modules || [];
  const key = { items: items.map((x) => [x.p.id, x.v.text, x.v.cls]), lost };
  if (changed("active", key)) {
    const rows = items.map(({ p, v }) => {
      const k = v.kind === "PENDING" ? "B" : v.kind === "OFF" ? "OFF" : v.kind;
      const label = v.kind === "PENDING" ? "…" : v.kind === "OFF" ? "–" : (k === "FAULT" ? "FEL" : k === "DRIFT" ? "D" : k);
      return `<div class="alarm-row k-${k} ${v.unacked ? "unacked" : ""}" data-point="${p.id}">
        <div class="cat">${esc(label)}</div>
        <div><div class="name">${esc(p.name)}</div><div class="sub">${p.id} · ${addr(p)} · ${esc(DEVICE_SV[p.device_type] || p.device_type)}</div></div>
        <div class="st">${esc(v.text)}</div></div>`;
    });
    for (const [line, mod] of lost) {
      rows.unshift(`<div class="alarm-row k-FAULT unacked"><div class="cat">FEL</div>
        <div><div class="name">Modul saknas</div><div class="sub">Linje ${line}, modul ${mod}</div></div>
        <div class="st">Ingen kommunikation</div></div>`);
    }
    $("#active-list").innerHTML = rows.length ? rows.join("") : `<div class="empty">Inga aktiva larm eller fel. Systemet är i normalläge.</div>`;
  }

  const r = S.status.relays;
  const relayRows = [
    ["K1", "A-summalarm", r.K1_A ? `<span class="state on red">Aktiv</span>` : `<span class="state">Vila</span>`],
    ["K2", "B-summalarm", r.K2_B ? `<span class="state on">Aktiv</span>` : `<span class="state">Vila</span>`],
    ["K3", "Fel (felsäker)", r.K3_FAULT_OK ? `<span class="state ok">OK – dragen</span>` : `<span class="state bad">Fel – släppt</span>`],
    ["K4", "C-summalarm", r.K4_C ? `<span class="state on">Aktiv</span>` : `<span class="state">Vila</span>`],
  ];
  if (changed("relays", r)) {
    $("#relays").innerHTML = relayRows.map(([k, l, st]) => `<div class="relay"><span><b>${k}</b> ${l}</span>${st}</div>`).join("");
  }
  const s = S.status;
  const info = [
    ["Centralen", s.firmware], ["Konfiguration", `v${s.config_version} · ${s.config_crc}`],
    ["Synkroniserad", s.config_in_sync ? "Ja" : "Nej"], ["Summer", s.buzzer ? "På" : "Av"],
    ["Senaste händelse", `#${s.last_event_seq}`], ["Drifttid", `${Math.floor(s.uptime_s / 60)} min`],
  ];
  if (changed("sysinfo", info)) {
    $("#sysinfo").innerHTML = info.map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join("");
  }
}

$("#active-list").addEventListener("click", (e) => {
  const row = e.target.closest("[data-point]");
  if (!row) return;
  const pg = markerPageFor(Number(row.dataset.point));
  if (pg) { S.pageId = pg; showTab("graphics"); }
  else showTab("points");
});

// ---------------------------------------------------------------- graphics
async function loadPages() {
  S.pages = await api("/api/pages");
  if (!S.pages.some((p) => p.id === S.pageId)) S.pageId = S.pages.length ? S.pages[0].id : null;
  S.cache.chips = S.cache.canvas = null;
  renderGraphics();
}

function pageSeverity(pg) {
  let sev = "";
  for (const m of pg.markers) {
    const p = S.points.find((x) => x.id === m.point_id);
    if (!p) continue;
    const v = visual(p);
    if (v.kind === "A") return "a";
    if (v.kind === "FAULT") sev = sev || "f";
    if ((v.kind === "B" || v.kind === "C") && sev !== "f") sev = "b";
  }
  return sev;
}

function renderGraphics() {
  const chipsKey = S.pages.map((p) => [p.id, p.name, pageSeverity(p), p.id === S.pageId]);
  if (changed("chips", chipsKey)) {
    $("#page-chips").innerHTML = S.pages.map((pg) => {
      const sev = pageSeverity(pg);
      return `<button class="page-chip ${pg.id === S.pageId ? "active" : ""}" data-page="${pg.id}">
        ${sev ? `<span class="dot ${sev}"></span>` : ""}${esc(pg.name)}</button>`;
    }).join("");
  }

  const pg = S.pages.find((p) => p.id === S.pageId);
  $("#stage-empty").hidden = !!pg;
  $("#canvas").hidden = !pg;
  $("#edit-hint").hidden = !(S.editing && pg);
  $("#canvas").classList.toggle("editing", S.editing);
  $("#btn-edit-markers").textContent = S.editing ? "Klar" : "Redigera markörer";
  if (!pg) return;

  const img = $("#page-img");
  if (img.getAttribute("src") !== pg.url) { img.src = pg.url; img.alt = pg.name; }

  const markers = pg.markers.map((m) => {
    const p = S.points.find((x) => x.id === m.point_id);
    const v = p ? visual(p) : { cls: "disabled", text: "Okänd punkt" };
    return { m, p, v, kind: markerKind(p, v) };
  });
  const ratio = img.naturalWidth && img.naturalHeight ? img.naturalWidth / img.naturalHeight : 0;
  const key = [pg.id, S.editing, ratio, markers.map(({ m, v, kind }) => [m.id, m.x, m.y, kind, v.text, v.unacked])];
  if (!changed("canvas", key)) return;

  const sides = placeTags(markers.filter((x) => S.editing || SPOT_KINDS.has(x.kind) || x.kind === "pending" || x.kind === "DRIFT"));
  $("#markers").innerHTML = markers.map(({ m, p, v, kind }) => {
    const alarm = SPOT_KINDS.has(kind) || kind === "pending" || kind === "DRIFT";
    const side = sides.get(m.id) || "ru";
    const name = p ? p.name : `Punkt ${m.point_id}`;
    let tag = "";
    if (S.editing) {
      tag = `<span class="mk-tag edit ${side}"><b>${p ? p.id + " " : ""}${esc(name)}</b></span>`;
    } else if (alarm) {
      tag = `<span class="mk-tag ${side}"><span class="mk-ic">${markerIcon(p, kind)}</span>
        <span><b>${esc(name)}</b><small>${esc(v.text)}</small></span></span>`;
    }
    return `<div class="mk k-${kind} ${v.unacked ? "pulse" : ""}" data-marker="${m.id}" data-point="${m.point_id}"
      style="left:${(m.x * 100).toFixed(3)}%;top:${(m.y * 100).toFixed(3)}%">
      <span class="mk-ring"></span><span class="mk-dot"></span>${tag}</div>`;
  }).join("");

  // Spotlight: darken the photo except around devices that need attention.
  const spot = $("#spot");
  const holes = S.editing || !ratio ? [] : markers.filter((x) => SPOT_KINDS.has(x.kind));
  if (holes.length) {
    const rx = 9, ry = rx * ratio;
    const mask = holes.map(({ m }) =>
      `radial-gradient(ellipse ${rx}% ${ry}% at ${(m.x * 100).toFixed(2)}% ${(m.y * 100).toFixed(2)}%, transparent 62%, #000 100%)`).join(", ");
    spot.style.webkitMaskImage = mask;
    spot.style.maskImage = mask;
    spot.classList.add("on");
  } else {
    spot.classList.remove("on");
  }
}

const SPOT_KINDS = new Set(["A", "latched", "B", "C", "FAULT"]);

// Choose a side for each tag so neighbouring tags do not cover each other.
// Positions are fractions of the image; a tag is roughly 22% wide and 9% tall.
function placeTags(list) {
  const W = 0.22, H = 0.09, placed = [], out = new Map();
  const rect = (m, s) => {
    const x0 = s[0] === "r" ? m.x + 0.012 : m.x - 0.012 - W;
    const y0 = s[1] === "u" ? m.y - 0.012 - H : m.y + 0.012;
    return [x0, y0, x0 + W, y0 + H];
  };
  const hits = (a, b) => a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
  const sorted = [...list].sort((a, b) => a.m.y - b.m.y || a.m.x - b.m.x);
  for (const { m } of sorted) {
    const prefX = m.x > 0.62 ? "l" : "r", altX = prefX === "r" ? "l" : "r";
    const prefY = m.y < 0.22 ? "d" : "u", altY = prefY === "u" ? "d" : "u";
    const options = [prefX + prefY, prefX + altY, altX + prefY, altX + altY];
    const choice = options.find((s) => !placed.some((r) => hits(r, rect(m, s)))) || options[0];
    placed.push(rect(m, choice));
    out.set(m.id, choice);
  }
  return out;
}

function markerKind(p, v) {
  if (!p || !p.runtime) return "OFF";
  if (v.kind === "OFF") return "OFF";
  if (v.kind === "PENDING") return "pending";
  if (v.cls === "latched") return "latched";
  if (["A", "B", "C", "FAULT", "DRIFT"].includes(v.kind)) return v.kind;
  return "normal";
}

const SVG = (body, fill = false) =>
  `<svg viewBox="0 0 24 24" fill="${fill ? "currentColor" : "none"}" stroke="${fill ? "none" : "currentColor"}" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${body}</svg>`;
const MK_ICONS = {
  flame: SVG('<path d="M12 2c1 3.5-1.5 5-1.5 7.5 0 1.4 1 2.5 2.3 2.5 1.6 0 2.2-1.5 2-3.5 2.2 1.6 3.7 4.3 3.7 7.1C18.5 19.6 15.6 22 12 22s-6.5-2.4-6.5-6.4C5.5 10 12 7.5 12 2z"/>', true),
  valve: SVG('<path d="M3 8v8M21 8v8M3 12h18M12 12V6M8 4h8"/>'),
  gauge: SVG('<circle cx="12" cy="13" r="8"/><path d="M12 13l4-4M12 5V3"/>'),
  flow: SVG('<path d="M3 9c2 0 2-2 4.5-2S10 9 12 9s2-2 4.5-2S19 9 21 9M3 16c2 0 2-2 4.5-2s2.5 2 4.5 2 2-2 4.5-2 2.5 2 4.5 2"/>'),
  pump: SVG('<circle cx="12" cy="12" r="8"/><path d="M12 8v8M8 12h8"/>'),
  temp: SVG('<path d="M10 14V5a2 2 0 1 1 4 0v9a4 4 0 1 1-4 0z"/>'),
  warn: SVG('<path d="M12 3l10 18H2L12 3z"/><path d="M12 10v5M12 18.5v.01"/>'),
  clock: SVG('<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>'),
  bell: SVG('<path d="M6 16V11a6 6 0 1 1 12 0v5l2 2H4l2-2zM10 21h4"/>'),
};
function markerIcon(p, kind) {
  if (kind === "A" || kind === "latched") return MK_ICONS.flame;
  if (kind === "FAULT") return MK_ICONS.warn;
  if (kind === "pending") return MK_ICONS.clock;
  const t = p ? p.device_type : "";
  if (t.includes("VENTIL")) return MK_ICONS.valve;
  if (t === "PRESSOSTAT") return MK_ICONS.gauge;
  if (t === "FLODESVAKT") return MK_ICONS.flow;
  if (t.startsWith("PUMP")) return MK_ICONS.pump;
  if (t === "TEMP") return MK_ICONS.temp;
  return MK_ICONS.bell;
}

$("#page-img").addEventListener("load", () => { S.cache.canvas = null; renderGraphics(); });

$("#page-chips").addEventListener("click", (e) => {
  const b = e.target.closest("[data-page]");
  if (!b) return;
  S.pageId = Number(b.dataset.page);
  hidePopover();
  renderGraphics();
});

$("#btn-edit-markers").addEventListener("click", () => {
  if (!S.editing && !requireTech()) return;
  S.editing = !S.editing;
  hidePopover();
  renderGraphics();
});

$("#btn-add-page").addEventListener("click", () => {
  if (!requireTech()) return;
  $("#upload-form").hidden = false;
});
$("#upload-cancel").addEventListener("click", () => { $("#upload-form").hidden = true; });

$("#upload-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    const res = await api("/api/pages", { method: "POST", body: new FormData(e.target) });
    e.target.reset();
    e.target.hidden = true;
    S.pageId = res.id;
    S.editing = true;
    await loadPages();
    toast("Bilden är uppladdad. Tryck på bilden för att placera markörer.", "ok");
  } catch (err) { toast(err.message, "bad"); }
});

$("#btn-delete-page").addEventListener("click", async () => {
  const pg = S.pages.find((p) => p.id === S.pageId);
  if (!pg || !confirm(`Ta bort bilden "${pg.name}" och alla dess markörer?`)) return;
  await api(`/api/pages/${pg.id}`, { method: "DELETE" });
  S.pageId = null;
  await loadPages();
});

$("#page-img").addEventListener("click", (e) => {
  if (!S.editing) return;
  const r = e.target.getBoundingClientRect();
  const x = (e.clientX - r.left) / r.width;
  const y = (e.clientY - r.top) / r.height;
  if (!S.points.length) { toast("Skapa först en larmpunkt under Larmpunkter", "bad"); return; }
  const used = new Set((S.pages.find((p) => p.id === S.pageId)?.markers || []).map((m) => m.point_id));
  const options = S.points.map((p) =>
    `<option value="${p.id}" ${used.has(p.id) ? "disabled" : ""}>${p.id} – ${esc(p.name)}${used.has(p.id) ? " (finns redan)" : ""}</option>`).join("");
  showPopover(e.clientX, e.clientY, `
    <h3>Ny markör</h3>
    <label class="hint">Larmpunkt</label>
    <select id="pop-point" style="width:100%;margin:6px 0 12px">${options}</select>
    <div class="row"><button class="btn btn-sm" id="pop-place">Placera</button>
    <button class="btn btn-ghost btn-sm" id="pop-cancel">Avbryt</button></div>`);
  $("#pop-cancel").onclick = hidePopover;
  $("#pop-place").onclick = async () => {
    try {
      await api(`/api/pages/${S.pageId}/markers`, { json: { x, y, point_id: Number($("#pop-point").value) } });
      hidePopover();
      await loadPages();
    } catch (err) { toast(err.message, "bad"); }
  };
});

$("#markers").addEventListener("click", async (e) => {
  if (!e.target.closest(".mk-dot, .mk-tag")) return;
  const el = e.target.closest("[data-marker]");
  if (!el) return;
  e.stopPropagation();
  const p = S.points.find((x) => x.id === Number(el.dataset.point));
  if (S.editing) {
    if (confirm(`Ta bort markören för ${p ? p.name : "punkten"}?`)) {
      await api(`/api/markers/${el.dataset.marker}`, { method: "DELETE" });
      await loadPages();
    }
    return;
  }
  if (!p) return;
  const v = visual(p);
  const rt = p.runtime || {};
  showPopover(e.clientX, e.clientY, `
    <h3>${esc(p.name)}</h3>
    <dl class="kv">
      <dt>Larm-ID</dt><dd>${p.id}</dd>
      <dt>Adress</dt><dd>${addr(p)}</dd>
      <dt>Typ</dt><dd>${esc(DEVICE_SV[p.device_type])}</dd>
      <dt>Kategori</dt><dd>${esc(CAT_SV[rt.display_category] || p.category)}</dd>
      <dt>Status</dt><dd>${esc(v.text)}</dd>
      <dt>Sektion</dt><dd>${p.section}</dd>
    </dl>
    <button class="btn btn-ghost btn-sm" id="pop-close">Stäng</button>`);
  $("#pop-close").onclick = hidePopover;
});

function showPopover(x, y, html) {
  const pop = $("#popover");
  pop.innerHTML = html;
  pop.hidden = false;
  const w = pop.offsetWidth, h = pop.offsetHeight;
  pop.style.left = `${Math.min(x + 10, window.innerWidth - w - 10)}px`;
  pop.style.top = `${Math.min(y + 10, window.innerHeight - h - 10)}px`;
}
function hidePopover() { $("#popover").hidden = true; }
document.addEventListener("keydown", (e) => { if (e.key === "Escape") hidePopover(); });

// ---------------------------------------------------------------- points
function renderPointsTable() {
  const key = S.points.map((p) => [p, visual(p).text, isTech()]);
  if (!changed("ptable", key)) return;
  const tech = isTech();
  $("#points-table tbody").innerHTML = S.points.length ? S.points.map((p) => {
    const v = visual(p);
    const sc = v.kind === "A" ? "alarm" : v.kind === "FAULT" ? "fault" : (v.kind === "B" || v.kind === "C" || v.kind === "PENDING") ? "warn" : v.kind === "OFF" ? "off" : "";
    const enabled = p.runtime ? p.runtime.enabled : true;
    return `<tr>
      <td>${p.id}</td><td>${addr(p)}</td><td>${esc(p.name)}</td>
      <td><span class="tag ${p.category}">${CAT_SV[p.category]}</span>${p.latching ? ` <span class="tag latch" title="Låsande – kräver återställning">Låsande</span>` : ""}</td>
      <td>${esc(DEVICE_SV[p.device_type])}</td><td>${p.delay_s ? p.delay_s + " s" : "–"}</td>
      <td><span class="status-text ${sc}">${esc(v.text)}</span></td>
      <td class="row">
        <button class="btn btn-ghost btn-sm" data-edit="${p.id}" ${tech ? "" : "disabled"}>Ändra</button>
        <button class="btn btn-ghost btn-sm" data-toggle="${p.id}" data-disable="${enabled ? 1 : 0}" ${tech ? "" : "disabled"}>${enabled ? "Koppla från" : "Koppla till"}</button>
      </td></tr>`;
  }).join("") : `<tr><td colspan="8" class="empty">Inga larmpunkter ännu.</td></tr>`;
}

function fillDeviceTypes() {
  $("#device-type").innerHTML = S.meta.device_types.map((d) => `<option value="${d}">${DEVICE_SV[d] || d}</option>`).join("");
}

function updateLatchHint() {
  const f = $("#point-form");
  const latching = S.meta.latching_device_types.includes(f.elements.namedItem("device_type").value);
  $("#latch-hint").textContent = latching
    ? "Låsande larm (brandlarm): ligger kvar tills någon återställer med nyckel, även om orsaken försvinner."
    : "Återgår automatiskt till normal när orsaken försvinner.";
}

function openPointForm(p) {
  if (!requireTech()) return;
  const f = $("#point-form");
  f.reset();
  $("#point-msg").textContent = "";
  $("#point-msg").className = "form-msg";
  if (p) {
    for (const k of ["id", "line", "module", "channel", "name", "lcd1", "lcd2", "category", "device_type", "contact", "delay_s", "section"]) f.elements.namedItem(k).value = p[k];
    $("#point-form-title").textContent = `Ändra larmpunkt ${p.id}`;
    S.editOriginalId = p.id;
  } else {
    const used = new Set(S.points.map((x) => x.id));
    let id = 1; while (used.has(id)) id++;
    f.elements.namedItem("id").value = id;
    $("#point-form-title").textContent = "Ny larmpunkt";
    S.editOriginalId = null;
  }
  $("#point-delete").hidden = !p;
  f.hidden = false;
  updateLatchHint();
  f.scrollIntoView({ behavior: "smooth", block: "start" });
}

$("#btn-new-point").addEventListener("click", () => openPointForm(null));
$("#point-cancel").addEventListener("click", () => { $("#point-form").hidden = true; });
$("#device-type").addEventListener("change", updateLatchHint);

$("#points-table").addEventListener("click", async (e) => {
  const ed = e.target.closest("[data-edit]");
  if (ed) { openPointForm(S.points.find((p) => p.id === Number(ed.dataset.edit))); return; }
  const tg = e.target.closest("[data-toggle]");
  if (tg) {
    try {
      await api("/api/cmd/disable", { json: { point_id: Number(tg.dataset.toggle), disabled: tg.dataset.disable === "1" } });
      await refresh();
    } catch (err) { toast(err.message, "bad"); }
  }
});

$("#point-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = e.target;
  const body = Object.fromEntries(new FormData(f).entries());
  const msg = $("#point-msg");
  msg.className = "form-msg"; msg.textContent = "Skickar till centralen…";
  const q = S.editOriginalId ? `?original_id=${S.editOriginalId}` : "";
  try {
    const res = await api(`/api/points${q}`, { json: body });
    msg.className = "form-msg ok";
    msg.textContent = `Sparad i centralen ✓  (konfiguration v${res.version}, CRC ${res.crc})`;
    S.editOriginalId = Number(body.id);
    $("#point-delete").hidden = false;
    await refresh();
  } catch (err) {
    msg.className = "form-msg bad";
    if (err.data && err.data.errors) {
      msg.textContent = "Ej sparad – centralen avvisade:\n" + err.data.errors.map((x) =>
        `• Punkt ${x.point_id}: ${FIELD_SV[x.field] || x.field} – ${ERR_SV[x.code] || x.code}`).join("\n");
    } else {
      msg.textContent = `Ej sparad – ${err.message}`;
    }
  }
});

$("#point-delete").addEventListener("click", async () => {
  const id = S.editOriginalId;
  if (!id || !confirm(`Ta bort larmpunkt ${id}?`)) return;
  try {
    await api(`/api/points/${id}`, { method: "DELETE" });
    $("#point-form").hidden = true;
    toast(`Larmpunkt ${id} borttagen`, "ok");
    await refresh();
    await loadPages();
  } catch (err) { toast(err.message, "bad"); }
});

// ---------------------------------------------------------------- events
async function loadEvents() {
  const q = $("#event-search").value.trim();
  $("#csv-link").href = `/api/events.csv${q ? `?q=${encodeURIComponent(q)}` : ""}`;
  const rows = await api(`/api/events?limit=300${q ? `&q=${encodeURIComponent(q)}` : ""}`);
  if (!changed("events", rows)) return;
  $("#events-table tbody").innerHTML = rows.length ? rows.map((r) => {
    const src = r.source_kind === "POINT" ? `${r.source_id} ${esc(r.point_name || "")}`
      : r.source_kind === "MODULE" ? `Linje ${Math.floor(r.source_id / 1000)}, modul ${r.source_id % 1000}` : "System";
    const ch = r.old || r.new ? `${esc(STATE_SV[r.old] || r.old || "")} → ${esc(STATE_SV[r.new] || r.new || "")}` : "";
    return `<tr><td>${r.seq}</td><td>${fmtTime(r.time)}</td><td>${esc(r.label)}</td><td>${src}</td><td>${ch}</td></tr>`;
  }).join("") : `<tr><td colspan="5" class="empty">Inga händelser.</td></tr>`;
}
let evTimer;
$("#event-search").addEventListener("input", () => { clearTimeout(evTimer); evTimer = setTimeout(loadEvents, 300); });

// ---------------------------------------------------------------- simulator
function renderSim() {
  if (!S.meta.simulator) return;
  $("#sim-key").checked = isTech();
  const key = S.points.map((p) => [p.id, p.name, p.runtime && p.runtime.condition]);
  if (changed("sim", key)) {
    const conds = [["NORMAL", "Normal", ""], ["ACTIVE", "Aktiv", "active-btn"], ["OPEN", "Avbrott", "fault-btn"], ["SHORT", "Kortslutning", "fault-btn"]];
    $("#sim-inputs").innerHTML = S.points.length ? S.points.map((p) => {
      const cur = p.runtime ? p.runtime.condition : "NORMAL";
      return `<div class="sim-row"><div><b>${p.id}</b> ${esc(p.name)} <span class="hint">${addr(p)}</span></div>
        <div class="seg">${conds.map(([c, l, cls]) =>
          `<button class="${cur === c ? "on " + cls : ""}" data-sim-point="${p.id}" data-cond="${c}">${l}</button>`).join("")}</div></div>`;
    }).join("") : `<div class="empty">Skapa larmpunkter först.</div>`;
  }
  const mods = [...new Map(S.points.map((p) => [`${p.line}-${p.module}`, [p.line, p.module]])).values()];
  const lost = new Set((S.status.lost_modules || []).map(([l, m]) => `${l}-${m}`));
  if (changed("simmods", [mods, [...lost]])) {
    $("#sim-modules").innerHTML = mods.length ? mods.map(([l, m]) =>
      `<label class="switch"><input type="checkbox" data-sim-module="${l}-${m}" ${lost.has(`${l}-${m}`) ? "checked" : ""}><span></span> L${l}-M${m} saknas</label>`).join("")
      : `<div class="hint">Inga moduler.</div>`;
  }
}

$("#sim-inputs").addEventListener("click", async (e) => {
  const b = e.target.closest("[data-sim-point]");
  if (!b) return;
  const p = S.points.find((x) => x.id === Number(b.dataset.simPoint));
  await api("/api/sim/input", { json: { line: p.line, module: p.module, channel: p.channel, condition: b.dataset.cond } });
  await refresh();
});
$("#sim-modules").addEventListener("change", async (e) => {
  const cb = e.target.closest("[data-sim-module]");
  if (!cb) return;
  const [line, module] = cb.dataset.simModule.split("-").map(Number);
  await api("/api/sim/module", { json: { line, module, lost: cb.checked } });
  await refresh();
});
$("#sim-key").addEventListener("change", async (e) => {
  await api("/api/sim/key", { json: { tech: e.target.checked } });
  if (!e.target.checked) S.editing = false;
  await refresh();
});

// ---------------------------------------------------------------- commands
$("#btn-ack").addEventListener("click", async () => {
  try { await api("/api/cmd/ack", { json: {} }); await refresh(); } catch (err) { toast(err.message, "bad"); }
});
$("#btn-reset").addEventListener("click", async () => {
  try {
    const res = await api("/api/cmd/reset", { json: {} });
    if (res.cause_remains && res.cause_remains.length) {
      toast(`Kan inte återställa ${res.cause_remains.join(", ")}: orsaken kvarstår`, "bad");
    } else {
      toast(res.reset.length ? `Återställt: ${res.reset.join(", ")}` : "Inget att återställa", "ok");
    }
    await refresh();
  } catch (err) { toast(err.message, "bad"); }
});

// ---------------------------------------------------------------- tabs
function showTab(name) {
  S.tab = name;
  document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === name));
  document.querySelectorAll(".view").forEach((v) => v.classList.toggle("active", v.id === `view-${name}`));
  hidePopover();
  if (name === "events") loadEvents();
  if (name === "graphics") { S.cache.chips = S.cache.canvas = null; renderGraphics(); }
}
document.querySelector(".tabs").addEventListener("click", (e) => {
  const b = e.target.closest("[data-tab]");
  if (b) showTab(b.dataset.tab);
});

// Show the photo of a new, unacknowledged alarm automatically.
function autoShowNewAlarm() {
  const now = new Set();
  let best = null;
  for (const p of S.points) {
    const v = visual(p);
    if (!v.unacked) continue;
    now.add(p.id);
    if (!S.seenAlarms.has(p.id) && markerPageFor(p.id) && (!best || v.rank < best.v.rank)) best = { p, v };
  }
  S.seenAlarms = now;
  if (best && (S.tab === "overview" || S.tab === "graphics") && !S.editing) {
    S.pageId = markerPageFor(best.p.id);
    showTab("graphics");
  }
}

// ---------------------------------------------------------------- polling
let lastStatusOk = true;
async function refresh() {
  try {
    const [status, points] = await Promise.all([api("/api/status"), api("/api/points")]);
    S.status = status;
    S.points = points;
    if (!status.key_tech && S.editing) S.editing = false;
    if (!lastStatusOk) toast("Kontakt med servern återställd", "ok");
    lastStatusOk = true;
    renderTop();
    renderOverview();
    renderGraphics();
    renderPointsTable();
    renderSim();
    autoShowNewAlarm();
  } catch (err) {
    $("#led-drift").className = "led";
    setBand("lost", "lost", "Ingen kontakt med centralen", "Statusen kan inte visas – kontrollera centralen och nätverket");
    if (lastStatusOk) toast("Ingen kontakt med servern", "bad");
    lastStatusOk = false;
  }
}

async function start() {
  S.meta = await api("/api/meta");
  fillDeviceTypes();
  $("#tab-sim").hidden = !S.meta.simulator;
  await refresh();
  await loadPages();
  setInterval(refresh, 700);
  setInterval(() => { if (S.tab === "events") loadEvents(); }, 3000);
}
start();
