"use strict";

// ---------------------------------------------------------------- language
// All texts live in i18n.json (sv, en, fi, no, da, el). t("key", {vars}) returns the text
// in the selected language, falling back to Swedish.
const L = { lang: "sv", dict: null };

function t(key, vars) {
  const s = (L.dict && ((L.dict.strings[L.lang] || {})[key] ?? L.dict.strings.sv[key])) ?? key;
  return vars ? s.replace(/\{(\w+)\}/g, (_, k) => (vars[k] ?? "")) : s;
}
const locale = () => (L.dict ? L.dict.locales[L.lang] : "sv-SE");

function applyStatic() {
  document.documentElement.lang = L.lang;
  document.title = t("app.title");
  document.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
  document.querySelectorAll("[data-i18n-ph]").forEach((el) => { el.placeholder = t(el.dataset.i18nPh); });
}

// ---------------------------------------------------------------- state
const S = {
  meta: null, status: null, points: [], pages: [],
  pageId: null, editing: false, tab: "overview",
  seenAlarms: new Set(), cache: {}, editOriginalId: null,
};

const $ = (sel, root = document) => root.querySelector(sel);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const addr = (p) => `L${p.line}-M${String(p.module).padStart(2, "0")}-IN${p.channel}`;
const fmtTime = (ts) => new Date(ts * 1000).toLocaleString(locale(), { hourCycle: "h23" });

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
  const el = $("#toast");
  el.textContent = msg;
  el.className = `toast ${kind}`;
  el.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (el.hidden = true), 3500);
}
// Server errors carry a code; show it in the selected language.
const errText = (err) => (err.data && err.data.code && L.dict && L.dict.strings.sv[`errc.${err.data.code}`]) ? t(`errc.${err.data.code}`) : err.message;

function changed(key, value) {
  const s = JSON.stringify(value);
  if (S.cache[key] === s) return false;
  S.cache[key] = s;
  return true;
}

const isTech = () => !!(S.status && S.status.key_tech);
function fmtDur(sec) {
  const m = Math.ceil(sec / 60);
  if (m < 60) return t("dur.min", { n: m });
  const h = Math.floor(m / 60), r = m % 60;
  return r ? `${t("dur.h", { n: h })} ${t("dur.min", { n: r })}` : t("dur.h", { n: h });
}
function requireTech() {
  if (isTech()) return true;
  toast(t("need_key"), "bad");
  return false;
}

// ---------------------------------------------------------------- point visuals
const IN_ALARM = new Set(["ALARM", "ACKED", "LATCHED"]);

function visual(p) {
  const rt = p.runtime;
  if (!rt) return { cls: "", text: t("v.not_in_ctrl"), rank: 99, active: false };
  if (!rt.enabled) return { cls: "disabled", text: t("v.disabled"), rank: 80, active: true, kind: "OFF" };
  const kind = rt.display_category;
  if (IN_ALARM.has(rt.state)) {
    const unacked = !rt.acked && kind !== "DRIFT";
    let text = rt.state === "LATCHED" ? t("v.latched") : (unacked ? t("v.unacked") : t("v.acked"));
    if (kind === "DRIFT") text = t("v.active");
    if (rt.fault) text += ` · ${t("fault." + rt.fault)}`;
    const rank = { A: 0, FAULT: 2, B: 3, C: 4, DRIFT: 9 }[kind] ?? 5;
    const cls = rt.state === "LATCHED" ? "latched" : `alarm-${kind}${unacked ? " ring" : ""}`;
    return { cls, text, rank: rank + (unacked ? 0 : 0.5), active: true, kind, unacked };
  }
  if (rt.state === "PENDING") {
    return { cls: "pending", text: t("v.pending", { s: Math.ceil(rt.pending_left_s ?? 0) }), rank: 6, active: true, kind: "PENDING" };
  }
  if (rt.fault) return { cls: "fault", text: t("fault." + rt.fault), rank: 1, active: true, kind: "FAULT", unacked: false };
  return { cls: "", text: t("v.normal"), rank: 99, active: false };
}

function markerPageFor(pointId) {
  for (const pg of S.pages) if (pg.markers.some((m) => m.point_id === pointId)) return pg.id;
  return null;
}

// ---------------------------------------------------------------- buzzer sound
// Plays the panel buzzer through the browser (Web Audio). Browsers only allow sound
// after a click on the page, so the audio is unlocked on the first pointer press.
const BUZ = { ctx: null, gain: null, on: true, pattern: null, timer: null };
try { BUZ.on = localStorage.getItem("buzzerSound") !== "0"; } catch { /* storage blocked */ }

function buzzerUnlock() {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  if (!BUZ.ctx) {
    BUZ.ctx = new AC();
    const osc = BUZ.ctx.createOscillator();
    BUZ.gain = BUZ.ctx.createGain();
    osc.type = "square";
    osc.frequency.value = 2700;
    BUZ.gain.gain.value = 0;
    osc.connect(BUZ.gain).connect(BUZ.ctx.destination);
    osc.start();
  }
  if (BUZ.ctx.state === "suspended") BUZ.ctx.resume();
  const want = BUZ.wanted; BUZ.pattern = undefined; buzzerSet(want);
}
document.addEventListener("pointerdown", buzzerUnlock, { capture: true });

// pattern: null (silent) | "fire" (fast) | "normal" (one beep per second)
function buzzerSet(pattern) {
  BUZ.wanted = pattern;
  if (!BUZ.on || !BUZ.ctx || BUZ.ctx.state !== "running") pattern = null;
  if (pattern === BUZ.pattern) return;
  BUZ.pattern = pattern;
  clearInterval(BUZ.timer);
  BUZ.timer = null;
  if (!BUZ.gain) return;
  const g = BUZ.gain.gain, ctx = BUZ.ctx;
  g.cancelScheduledValues(ctx.currentTime);
  g.setValueAtTime(0, ctx.currentTime);
  if (!pattern) return;
  const [on, period] = pattern === "fire" ? [0.18, 0.36] : [0.15, 1.0];
  const beep = () => { const t = ctx.currentTime; g.setValueAtTime(0.05, t); g.setValueAtTime(0, t + on); };
  beep();
  BUZ.timer = setInterval(beep, period * 1000);
}

function updateBuzzer() {
  const s = S.status;
  if (!s || !s.buzzer) { buzzerSet(null); return; }
  const fire = S.points.some((p) => { const v = visual(p); return v.kind === "A" && v.unacked; });
  buzzerSet(fire ? "fire" : "normal");
}

// Speaker icons for the "Tysta larmdon" button: waves = sounders normal, X = silenced.
const SPEAKER_ON = '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 9h4l5-4v14l-5-4H4z" fill="currentColor"/><path d="M16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"/></svg>';
const SPEAKER_OFF = '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 9h4l5-4v14l-5-4H4z" fill="currentColor"/><path d="M16 9l6 6M22 9l-6 6"/></svg>';

// ---------------------------------------------------------------- top bar
function renderTop() {
  const s = S.status;
  const setLed = (id, cls) => { $(id).className = `led ${cls}`; };
  setLed("#led-a", s.leds.red_a ? "on-red flash" : "");
  setLed("#led-b", s.leds.yellow_b === "flash" ? "on-amber flash" : s.leds.yellow_b === "steady" ? "on-amber" : "");
  setLed("#led-fault", s.leds.yellow_fault ? "on-amber" : "");
  setLed("#led-drift", s.leds.green_power ? "on-green" : "");
  setLed("#led-service", s.leds.yellow_service ? "on-amber" : "");
  const sil = $("#btn-silence");
  // The label stays the same (so the button never changes size); the icon and colour show the state.
  const silKey = (s.silenced ? "off" : "on") + L.lang;
  if (sil.dataset.key !== silKey) {
    sil.innerHTML = `${s.silenced ? SPEAKER_OFF : SPEAKER_ON}<span class="lbl-long">${t("btn.silence")}</span><span class="lbl-short">${t("btn.silence_short")}</span>`;
    sil.title = s.silenced ? t("btn.unsilence") : t("btn.silence");
    sil.dataset.key = silKey;
  }
  sil.classList.toggle("active", !!s.silenced);

  const chip = $("#key-chip");
  chip.textContent = s.key_tech ? t("key.tech") : t("key.user");
  chip.className = `chip ${s.key_tech ? "tech" : ""}`;
  $("#btn-reset").disabled = !s.key_tech;

  renderStatusBand();
  updateBuzzer();
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
  const service = s.service && s.service.active;
  const hint = service ? t("band.tx_blocked") : (s.buzzer ? t("band.press_ack") : "");

  const fire = of("A");
  if (s.relays.K1_A || fire.length) {
    const unacked = fire.some((x) => x.v.unacked) || items.some((x) => x.p.category === "A" && x.p.runtime && x.p.runtime.state === "LATCHED" && !x.p.runtime.acked);
    const latchedOnly = fire.length && fire.every((x) => x.p.runtime.state === "LATCHED");
    setBand("fire", "fire", t("band.fire"),
      t(latchedOnly ? "band.fire_latched_sub" : "band.fire_sub", { names: namesOf(fire) }),
      service ? t("band.tx_blocked") : (unacked ? t("band.press_ack") : (s.sounders === "silenced" ? t("band.silenced") : (latchedOnly ? t("band.reset_key") : t("band.acked")))), unacked);
    return;
  }
  // a point can be in alarm AND have a line fault (valve opened, then the cable was cut)
  const faults = items.filter((x) => x.v.kind === "FAULT" || (x.p.runtime && x.p.runtime.fault));
  if (faults.length || lost.length || !s.relays.K3_FAULT_OK) {
    const n = faults.length + lost.length;
    const what = faults.length ? `${namesOf(faults)}: ${t("fault." + faults[0].p.runtime.fault)}`
      : lost.length ? t("band.module_missing", { l: lost[0][0], m: lost[0][1] }) : "";
    setBand("fault", "warn", n > 1 ? `${t("band.fault")} (${n})` : t("band.fault"), what, hint);
    return;
  }
  const b = of("B");
  if (b.length) { setBand("b", "warn", b.length > 1 ? `${t("cat.B")} (${b.length})` : t("cat.B"), namesOf(b), hint, b.some((x) => x.v.unacked)); return; }
  const c = of("C");
  if (c.length) { setBand("c", "info", c.length > 1 ? `${t("cat.C")} (${c.length})` : t("cat.C"), t("band.c_sub", { names: namesOf(c) }), hint); return; }
  const pending = of("PENDING");
  if (pending.length) { setBand("pending", "info", t("band.pending"), `${namesOf(pending)} – ${pending[0].v.text.toLowerCase()}`, hint); return; }
  const off = of("OFF");
  if (off.length) { setBand("off", "info", t("band.off"), t("band.off_sub", { n: off.length, names: namesOf(off) }), hint); return; }
  if (!s.config_in_sync) { setBand("off", "info", t("band.unsync"), t("band.unsync_sub"), hint); return; }
  if (service) { setBand("service", "info", t("band.service"), t("band.service_sub", { t: fmtDur(s.service.remaining_s) })); return; }
  setBand("ok", "ok", t("band.ok"), t("band.ok_sub", { n: S.points.length }), hint);
}

// ---------------------------------------------------------------- overview
// Summary tiles are also filters: tap one to list only its points in "Active alarms and faults",
// tap again (or "Show all") to see everything. The filter resets by itself after 2 minutes
// and whenever a new alarm comes in, so a new alarm is never hidden.
const OV_FILTERS = {
  a: (v) => v.kind === "A",
  b: (v) => v.kind === "B",
  c: (v) => v.kind === "C",
  f: (v, p) => v.kind === "FAULT" || !!(p.runtime && p.runtime.fault),
  d: (v) => v.kind === "DRIFT",
  o: (v) => v.kind === "OFF",
};
let ovFilterTimer = 0;
function setOvFilter(k) {
  S.ovFilter = k;
  clearTimeout(ovFilterTimer);
  if (k) ovFilterTimer = setTimeout(() => setOvFilter(null), 120000);
  if (S.status) renderOverview();
  const list = $("#active-list");
  if (list) list.scrollTop = 0;
}

function renderOverview() {
  const c = S.status.counts;
  const tiles = [
    ["a", t("cat.A"), c.A], ["b", t("cat.B"), c.B], ["c", t("cat.C"), c.C],
    ["f", t("cat.FAULT"), c.FAULT], ["d", t("cat.DRIFT"), c.DRIFT], ["o", t("ov.tile_off"), c.DISABLED],
  ];
  const F = S.ovFilter || null;
  if (changed("summary", [c, L.lang, F])) {
    $("#summary").innerHTML = tiles.map(([k, l, n]) =>
      `<button type="button" class="tile ${k} ${n ? "hot" : ""} ${F === k ? "sel" : ""}" data-filter="${k}" title="${esc(t("ov.tile_hint"))}">
        <div class="n">${n}</div><div class="l">${l}</div></button>`).join("");
  }
  const tile = tiles.find((x) => x[0] === F);
  if (changed("activeTitle", [F, L.lang, tile && tile[2]])) {
    $("#active-title").innerHTML = F
      ? `<span>${esc(tile[1])} · ${tile[2]}</span><button type="button" class="chip-clear" data-filter="">✕ ${esc(t("ov.show_all"))}</button>`
      : esc(t("ov.active"));
  }

  const items = S.points.map((p) => ({ p, v: visual(p) }))
    .filter((x) => (F ? x.v.active !== undefined && OV_FILTERS[F](x.v, x.p) : x.v.active));
  items.sort((a, b) => a.v.rank - b.v.rank || a.p.id - b.p.id);
  const lost = !F || F === "f" ? (S.status.lost_modules || []) : [];
  const key = { items: items.map((x) => [x.p.id, x.v.text, x.v.cls]), lost, F, lang: L.lang };
  if (changed("active", key)) {
    const rows = items.map(({ p, v }) => {
      const k = v.kind === "PENDING" ? "B" : v.kind === "OFF" ? "OFF" : v.kind;
      const label = v.kind === "PENDING" ? "…" : v.kind === "OFF" ? "–" : (k === "FAULT" ? t("short.fault") : k === "DRIFT" ? t("short.drift") : k);
      return `<div class="alarm-row k-${k} ${v.unacked ? "unacked" : ""}" data-point="${p.id}">
        <div class="cat">${esc(label)}</div>
        <div><div class="name">${esc(p.name)}</div><div class="sub">${p.id} · ${addr(p)} · ${esc(t("dev." + p.device_type))}</div></div>
        <div class="st">${esc(v.text)}</div></div>`;
    });
    for (const [line, mod] of lost) {
      rows.unshift(`<div class="alarm-row k-FAULT unacked"><div class="cat">${t("short.fault")}</div>
        <div><div class="name">${t("fault.MODULE_LOST")}</div><div class="sub">${t("ov.line_module", { l: line, m: mod })}</div></div>
        <div class="st">${t("ov.no_comm")}</div></div>`);
    }
    $("#active-list").innerHTML = rows.length ? rows.join("") : `<div class="empty">${t(F ? "ov.filter_empty" : "ov.empty")}</div>`;
    markMore($("#active-list"));
  }

  const r = S.status.relays;
  const lg = S.status.relays_logical || r, blocked = !!S.status.transmission_blocked;
  const blk = `<span class="state blocked">${t("relay.blocked")}</span>`;
  const on = (cls) => `<span class="state on ${cls}">${t("relay.active")}</span>`, idle = `<span class="state">${t("relay.idle")}</span>`;
  const out = (key, cls) => (r[key] ? on(cls) : (blocked && lg[key] ? blk : idle));
  const relayRows = [
    ["K1", t("relay.k1"), out("K1_A", "red")],
    ["K2", t("relay.k2"), out("K2_B", "")],
    ["K3", t("relay.k3"), r.K3_FAULT_OK
      ? (blocked && !lg.K3_FAULT_OK ? `<span class="state blocked">${t("relay.bad_blocked")}</span>` : `<span class="state ok">${t("relay.ok")}</span>`)
      : `<span class="state bad">${t("relay.bad")}</span>`],
    ["K4", t("relay.k4"), out("K4_C", "")],
    ["", t("relay.sounders"), S.status.sounders === "sounding" ? `<span class="state on red">${t("sounders.sounding")}</span>`
      : S.status.sounders === "silenced" ? `<span class="state silenced">${t("sounders.silenced")}</span>` : idle],
  ];
  if (changed("relays", [r, lg, blocked, S.status.sounders, L.lang])) {
    $("#relays").innerHTML = relayRows.map(([k, l, st]) => `<div class="relay"><span><b>${k}</b> ${l}</span>${st}</div>`).join("");
  }
  const s = S.status;
  const info = [
    [t("sys.ctrl"), s.firmware], [t("sys.config"), `v${s.config_version} · ${s.config_crc}`],
    [t("sys.synced"), s.config_in_sync ? t("common.yes") : t("common.no")], [t("sys.buzzer"), s.buzzer ? t("common.on") : t("common.off")],
    [t("sys.last"), `#${s.last_event_seq}`], [t("sys.uptime"), `${Math.floor(s.uptime_s / 60)} min`],
    [t("sys.time"), s.time ? fmtTime(s.time) : "–"],
  ];
  if (changed("sysinfo", info)) {
    $("#sysinfo").innerHTML = info.map(([k, v]) => `<dt>${k}</dt><dd>${esc(v)}</dd>`).join("");
  }
}

// No scrollbars on the panel: a list that continues below the visible part fades out at the bottom.
function markMore(el) {
  el.classList.toggle("more", el.scrollHeight - el.scrollTop - el.clientHeight > 2);
}
$("#active-list").addEventListener("scroll", (e) => markMore(e.currentTarget));
window.addEventListener("resize", () => markMore($("#active-list")));

$("#summary").addEventListener("click", (e) => {
  const b = e.target.closest("[data-filter]");
  if (b) setOvFilter(S.ovFilter === b.dataset.filter ? null : b.dataset.filter);
});
$("#active-title").addEventListener("click", (e) => {
  if (e.target.closest("[data-filter]")) setOvFilter(null);
});

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
  $("#btn-edit-markers").textContent = S.editing ? t("gr.done") : t("gr.edit");
  if (!pg) return;

  const img = $("#page-img");
  if (img.getAttribute("src") !== pg.url) { img.src = pg.url; img.alt = pg.name; }

  const markers = pg.markers.map((m) => {
    const p = S.points.find((x) => x.id === m.point_id);
    const v = p ? visual(p) : { cls: "disabled", text: t("gr.unknown_point") };
    return { m, p, v, kind: markerKind(p, v) };
  });
  const ratio = img.naturalWidth && img.naturalHeight ? img.naturalWidth / img.naturalHeight : 0;
  const key = [pg.id, S.editing, ratio, L.lang, markers.map(({ m, v, kind }) => [m.id, m.x, m.y, kind, v.text, v.unacked])];
  if (!changed("canvas", key)) return;

  const sides = placeTags(markers.filter((x) => S.editing || SPOT_KINDS.has(x.kind) || x.kind === "pending" || x.kind === "DRIFT"));
  $("#markers").innerHTML = markers.map(({ m, p, v, kind }) => {
    const alarm = SPOT_KINDS.has(kind) || kind === "pending" || kind === "DRIFT";
    const side = sides.get(m.id) || "ru";
    const name = p ? p.name : t("gr.point", { id: m.point_id });
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
    toast(t("gr.uploaded"), "ok");
  } catch (err) { toast(errText(err), "bad"); }
});

$("#btn-delete-page").addEventListener("click", async () => {
  const pg = S.pages.find((p) => p.id === S.pageId);
  if (!pg || !confirm(t("gr.confirm_delete_page", { name: pg.name }))) return;
  await api(`/api/pages/${pg.id}`, { method: "DELETE" });
  S.pageId = null;
  await loadPages();
});

$("#page-img").addEventListener("click", (e) => {
  if (!S.editing) return;
  const r = e.target.getBoundingClientRect();
  const x = (e.clientX - r.left) / r.width;
  const y = (e.clientY - r.top) / r.height;
  if (!S.points.length) { toast(t("gr.need_point"), "bad"); return; }
  const used = new Set((S.pages.find((p) => p.id === S.pageId)?.markers || []).map((m) => m.point_id));
  const options = S.points.map((p) =>
    `<option value="${p.id}" ${used.has(p.id) ? "disabled" : ""}>${p.id} – ${esc(p.name)}${used.has(p.id) ? " " + t("gr.exists") : ""}</option>`).join("");
  showPopover(e.clientX, e.clientY, `
    <h3>${t("gr.new_marker")}</h3>
    <label class="hint">${t("gr.point_label")}</label>
    <select id="pop-point" style="width:100%;margin:6px 0 12px">${options}</select>
    <div class="row"><button class="btn btn-sm" id="pop-place">${t("gr.place")}</button>
    <button class="btn btn-ghost btn-sm" id="pop-cancel">${t("common.cancel")}</button></div>`);
  $("#pop-cancel").onclick = hidePopover;
  $("#pop-place").onclick = async () => {
    try {
      await api(`/api/pages/${S.pageId}/markers`, { json: { x, y, point_id: Number($("#pop-point").value) } });
      hidePopover();
      await loadPages();
    } catch (err) { toast(errText(err), "bad"); }
  };
});

$("#markers").addEventListener("click", async (e) => {
  if (!e.target.closest(".mk-dot, .mk-tag")) return;
  const el = e.target.closest("[data-marker]");
  if (!el) return;
  e.stopPropagation();
  const p = S.points.find((x) => x.id === Number(el.dataset.point));
  if (S.editing) {
    if (confirm(t("gr.confirm_delete_marker", { name: p ? p.name : "?" }))) {
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
      <dt>${t("f.id")}</dt><dd>${p.id}</dd>
      <dt>${t("th.address")}</dt><dd>${addr(p)}</dd>
      <dt>${t("th.type")}</dt><dd>${esc(t("dev." + p.device_type))}</dd>
      <dt>${t("f.category")}</dt><dd>${esc(t("cat." + (rt.display_category || p.category)))}</dd>
      <dt>${t("th.status")}</dt><dd>${esc(v.text)}</dd>
      <dt>${t("f.section")}</dt><dd>${p.section}</dd>
    </dl>
    <button class="btn btn-ghost btn-sm" id="pop-close">${t("common.close")}</button>`);
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
// Row buttons: icon + text; on the small panel screen only the icon is shown (text in the tooltip).
const IC = (d) => `<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true"><path d="${d}" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>`;
const IC_EDIT = IC("M4 20h4L19 9l-4-4L4 16v4zM13.5 6.5l4 4");
const IC_OFF = IC("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM5.6 5.6l12.8 12.8");
const IC_ON = IC("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8 12.5l3 3 5-6");

function renderPointsTable() {
  const key = [L.lang, S.points.map((p) => [p, visual(p).text, isTech()])];
  if (!changed("ptable", key)) return;
  const tech = isTech();
  $("#points-table tbody").innerHTML = S.points.length ? S.points.map((p) => {
    const v = visual(p);
    const sc = v.kind === "A" ? "alarm" : v.kind === "FAULT" ? "fault" : (v.kind === "B" || v.kind === "C" || v.kind === "PENDING") ? "warn" : v.kind === "OFF" ? "off" : "";
    const enabled = p.runtime ? p.runtime.enabled : true;
    return `<tr>
      <td>${p.id}</td><td>${addr(p)}</td><td>${esc(p.name)}</td>
      <td><span class="tag ${p.category}">${t("cat." + p.category)}</span>${p.latching ? ` <span class="tag latch" title="${t("pt.latching_title")}">${t("pt.latching")}</span>` : ""}</td>
      <td>${esc(t("dev." + p.device_type))}</td><td>${p.delay_s ? p.delay_s + " s" : "–"}</td>
      <td><span class="status-text ${sc}">${esc(v.text)}</span></td>
      <td class="row">
        <button class="btn btn-ghost btn-sm btn-ic" data-edit="${p.id}" title="${t("pt.edit")}" ${tech ? "" : "disabled"}>${IC_EDIT}<span class="lbl">${t("pt.edit")}</span></button>
        <button class="btn btn-ghost btn-sm btn-ic" data-toggle="${p.id}" data-disable="${enabled ? 1 : 0}" title="${enabled ? t("pt.disable") : t("pt.enable")}" ${tech ? "" : "disabled"}>${enabled ? IC_OFF : IC_ON}<span class="lbl">${enabled ? t("pt.disable") : t("pt.enable")}</span></button>
      </td></tr>`;
  }).join("") : `<tr><td colspan="8" class="empty">${t("pt.empty")}</td></tr>`;
}

function fillDeviceTypes() {
  const sel = $("#device-type"), cur = sel.value;
  sel.innerHTML = S.meta.device_types.map((d) => `<option value="${d}">${esc(t("dev." + d))}</option>`).join("");
  if (cur) sel.value = cur;
}

function updateLatchHint() {
  const f = $("#point-form");
  const latching = S.meta.latching_device_types.includes(f.elements.namedItem("device_type").value);
  $("#latch-hint").textContent = latching ? t("pt.latch_yes") : t("pt.latch_no");
}

function openPointForm(p) {
  if (!requireTech()) return;
  const f = $("#point-form");
  f.reset();
  $("#point-msg").textContent = "";
  $("#point-msg").className = "form-msg";
  if (p) {
    for (const k of ["id", "line", "module", "channel", "name", "lcd1", "lcd2", "category", "device_type", "contact", "delay_s", "section"]) f.elements.namedItem(k).value = p[k];
    $("#point-form-title").textContent = t("pt.form_edit", { id: p.id });
    S.editOriginalId = p.id;
  } else {
    const used = new Set(S.points.map((x) => x.id));
    let id = 1; while (used.has(id)) id++;
    f.elements.namedItem("id").value = id;
    $("#point-form-title").textContent = t("pt.form_new");
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
    } catch (err) { toast(errText(err), "bad"); }
  }
});

$("#point-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = e.target;
  const body = Object.fromEntries(new FormData(f).entries());
  const msg = $("#point-msg");
  msg.className = "form-msg"; msg.textContent = t("pt.sending");
  const q = S.editOriginalId ? `?original_id=${S.editOriginalId}` : "";
  try {
    const res = await api(`/api/points${q}`, { json: body });
    msg.className = "form-msg ok";
    msg.textContent = t("pt.saved", { v: res.version, crc: res.crc });
    S.editOriginalId = Number(body.id);
    $("#point-delete").hidden = false;
    await refresh();
  } catch (err) {
    msg.className = "form-msg bad";
    if (err.data && err.data.errors) {
      msg.textContent = t("pt.rejected") + "\n" + err.data.errors.map((x) =>
        t("pt.err_line", { id: x.point_id, field: t(x.field === "delay_s" ? "f.delay_s" : "f." + x.field), err: t("err." + x.code) })).join("\n");
    } else {
      msg.textContent = t("pt.not_saved", { msg: errText(err) });
    }
  }
});

$("#point-delete").addEventListener("click", async () => {
  const id = S.editOriginalId;
  if (!id || !confirm(t("pt.confirm_delete", { id }))) return;
  try {
    await api(`/api/points/${id}`, { method: "DELETE" });
    $("#point-form").hidden = true;
    toast(t("pt.deleted", { id }), "ok");
    await refresh();
    await loadPages();
  } catch (err) { toast(errText(err), "bad"); }
});

// ---------------------------------------------------------------- events
async function loadEvents() {
  const q = $("#event-search").value.trim();
  $("#csv-link").href = `/api/events.csv${q ? `?q=${encodeURIComponent(q)}` : ""}`;
  const rows = await api(`/api/events?limit=300${q ? `&q=${encodeURIComponent(q)}` : ""}`);
  if (!changed("events", [rows, L.lang])) return;
  $("#events-table tbody").innerHTML = rows.length ? rows.map((r) => {
    const src = r.source_kind === "POINT" ? `${r.source_id} ${esc(r.point_name || "")}`
      : r.source_kind === "MODULE" ? t("ov.line_module", { l: Math.floor(r.source_id / 1000), m: r.source_id % 1000 }) : t("ev.system");
    const st = (x) => (r.type === "TIME_SET" && x ? fmtTime(Number(x)) : (x && L.dict.strings.sv["state." + x] ? t("state." + x) : x || ""));
    const ch = r.old || r.new ? `${esc(st(r.old))} → ${esc(st(r.new))}` : "";
    return `<tr><td>${r.seq}</td><td>${fmtTime(r.time)}</td><td>${esc(t("ev.type." + r.type) === "ev.type." + r.type ? r.type : t("ev.type." + r.type))}</td><td>${src}</td><td>${ch}</td></tr>`;
  }).join("") : `<tr><td colspan="5" class="empty">${t("ev.empty")}</td></tr>`;
}
let evTimer;
$("#event-search").addEventListener("input", () => { clearTimeout(evTimer); evTimer = setTimeout(loadEvents, 300); });

// ---------------------------------------------------------------- simulator
function renderSim() {
  if (!S.meta.simulator && !S.meta.dev_key) return;
  $("#sim-key").checked = isTech();
  if (!S.meta.simulator) return;   // real controller: only the key switch is played from here
  const key = [L.lang, S.points.map((p) => [p.id, p.name, p.runtime && p.runtime.condition])];
  if (changed("sim", key)) {
    const conds = [["NORMAL", t("cond.NORMAL"), ""], ["ACTIVE", t("cond.ACTIVE"), "active-btn"], ["OPEN", t("cond.OPEN"), "fault-btn"], ["SHORT", t("cond.SHORT"), "fault-btn"]];
    $("#sim-inputs").innerHTML = S.points.length ? S.points.map((p) => {
      const cur = p.runtime ? p.runtime.condition : "NORMAL";
      return `<div class="sim-row"><div><b>${p.id}</b> ${esc(p.name)} <span class="hint">${addr(p)}</span></div>
        <div class="seg">${conds.map(([c, l, cls]) =>
          `<button class="${cur === c ? "on " + cls : ""}" data-sim-point="${p.id}" data-cond="${c}">${l}</button>`).join("")}</div></div>`;
    }).join("") : `<div class="empty">${t("sim.empty")}</div>`;
  }
  const mods = [...new Map(S.points.map((p) => [`${p.line}-${p.module}`, [p.line, p.module]])).values()];
  const lost = new Set((S.status.lost_modules || []).map(([l, m]) => `${l}-${m}`));
  if (changed("simmods", [mods, [...lost], L.lang])) {
    $("#sim-modules").innerHTML = mods.length ? mods.map(([l, m]) =>
      `<label class="switch"><input type="checkbox" data-sim-module="${l}-${m}" ${lost.has(`${l}-${m}`) ? "checked" : ""}><span></span> ${t("sim.module_missing", { l, m })}</label>`).join("")
      : `<div class="hint">${t("sim.no_modules")}</div>`;
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
$("#sim-sound").checked = BUZ.on;
$("#sim-sound").addEventListener("change", (e) => {
  BUZ.on = e.target.checked;
  try { localStorage.setItem("buzzerSound", BUZ.on ? "1" : "0"); } catch { /* storage blocked */ }
  buzzerUnlock();
  updateBuzzer();
});
$("#sim-key").addEventListener("change", async (e) => {
  await api("/api/sim/key", { json: { tech: e.target.checked } });
  if (!e.target.checked) S.editing = false;
  await refresh();
});

// ---------------------------------------------------------------- commands
$("#btn-ack").addEventListener("click", async () => {
  try { await api("/api/cmd/ack", { json: {} }); await refresh(); } catch (err) { toast(errText(err), "bad"); }
});
$("#btn-silence").addEventListener("click", async () => {
  if (!requireTech()) return;
  try {
    const res = await api("/api/cmd/silence", { json: { silence: !S.status.silenced } });
    toast(res.silenced ? t("cmd.silenced") : t("cmd.unsilenced"), "ok");
    await refresh();
  } catch (err) { toast(errText(err), "bad"); }
});
$("#btn-reset").addEventListener("click", async () => {
  try {
    const res = await api("/api/cmd/reset", { json: {} });
    if (res.cause_remains && res.cause_remains.length) {
      toast(t("cmd.cannot_reset", { ids: res.cause_remains.join(", ") }), "bad");
    } else {
      toast(res.reset.length ? t("cmd.reset_done", { ids: res.reset.join(", ") }) : t("cmd.nothing"), "ok");
    }
    await refresh();
  } catch (err) { toast(errText(err), "bad"); }
});

// ---------------------------------------------------------------- tabs
function showTab(name) {
  S.tab = name;
  document.querySelectorAll(".tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === name));
  document.querySelectorAll(".view").forEach((v) => v.classList.toggle("active", v.id === `view-${name}`));
  hidePopover();
  if (name === "events") loadEvents();
  if (name === "graphics") { S.cache.chips = S.cache.canvas = null; renderGraphics(); }
  if (name === "settings") renderSettings(true);
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
    if (!S.seenAlarms.has(p.id) && S.ovFilter) setOvFilter(null);  // never hide a new alarm
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
    if (!lastStatusOk) toast(t("conn.restored"), "ok");
    lastStatusOk = true;
    // A mistake in one part of the page must not blank the rest, and it is not a lost
    // connection: it is logged in the browser console instead.
    for (const draw of [renderTop, renderOverview, renderGraphics, renderPointsTable, renderSim,
                        () => renderSettings(false), autoShowNewAlarm]) {
      try { draw(); } catch (e) { console.error("render failed:", draw.name || "settings", e); }
    }
  } catch (err) {
    $("#led-drift").className = "led";
    buzzerSet(null);
    setBand("lost", "lost", t("band.lost"), t("band.lost_sub"));
    const ctrl = err.data && err.data.code === "CONTROLLER_UNAVAILABLE";
    if (lastStatusOk) toast(ctrl ? errText(err) : t("conn.lost"), "bad");
    lastStatusOk = false;
  }
}

// ---------------------------------------------------------------- settings
function renderSettings(force) {
  if (S.tab !== "settings" && !force) return;
  const tech = isTech();
  $("#settings-lock").hidden = tech;
  $("#btn-set-time").disabled = !tech;
  $("#btn-sync-time").disabled = !tech;
  $("#new-time").disabled = !tech;
  if (S.status && S.status.time) {
    const d = new Date(S.status.time * 1000);
    $("#ctrl-clock").textContent = d.toLocaleTimeString(locale(), { hourCycle: "h23" });
    $("#ctrl-date").textContent = d.toLocaleDateString(locale(), { weekday: "long", year: "numeric", month: "long", day: "numeric" });
    if (force || !$("#new-time").value) $("#new-time").value = toLocalInput(d);
  }
  const sv = S.status && S.status.service;
  const st = $("#service-state");
  st.textContent = sv && sv.active ? t("set.service_active", { t: fmtDur(sv.remaining_s) }) : t("set.service_inactive");
  st.className = `service-state ${sv && sv.active ? "on" : ""}`;
  $("#btn-service-end").hidden = !(sv && sv.active);
  $("#btn-service-end").disabled = !tech;
  $("#btn-reboot").disabled = !tech;
  if (changed("svcbtns", [L.lang, tech])) {
    $("#service-btns").innerHTML = [30, 60, 120, 240, 480].map((m) =>
      `<button class="btn btn-ghost" data-service="${m}" ${tech ? "" : "disabled"}>${fmtDur(m * 60)}</button>`).join("");
  }
  if (changed("langs", [L.lang, tech])) {
    $("#lang-grid").innerHTML = Object.entries(L.dict.languages).map(([code, name]) =>
      `<button class="lang-btn ${code === L.lang ? "active" : ""}" data-lang="${code}" ${tech ? "" : "disabled"}>
        <span class="lang-code">${code.toUpperCase()}</span><span>${esc(name)}</span></button>`).join("");
  }
}

function toLocalInput(d) {
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

async function setControllerTime(unix) {
  try {
    await api("/api/time", { json: { time: unix } });
    toast(t("set.time_saved"), "ok");
    await refresh();
    renderSettings(true);
  } catch (err) { toast(errText(err), "bad"); }
}
$("#btn-set-time").addEventListener("click", () => {
  const v = $("#new-time").value;
  const ms = v ? new Date(v).getTime() : NaN;
  if (Number.isNaN(ms)) { toast(t("errc.BAD_TIME"), "bad"); return; }
  setControllerTime(ms / 1000);
});
$("#btn-sync-time").addEventListener("click", () => setControllerTime(Date.now() / 1000));

async function setService(minutes) {
  try {
    await api("/api/cmd/service", { json: { minutes } });
    toast(minutes ? t("set.service_started") : t("set.service_ended"), "ok");
    await refresh();
    renderSettings(true);
  } catch (err) { toast(errText(err), "bad"); }
}
$("#service-btns").addEventListener("click", (e) => {
  const b = e.target.closest("[data-service]");
  if (b) setService(Number(b.dataset.service));
});
$("#btn-service-end").addEventListener("click", () => {
  const lg = (S.status && S.status.relays_logical) || {};
  const n = (S.status.counts.A || 0) + (S.status.counts.B || 0) + (S.status.counts.C || 0) + (lg.K3_FAULT_OK === false ? (S.status.counts.FAULT || 1) : 0);
  if (n && !confirm(t("set.service_end_confirm", { n }))) return;
  setService(0);
});

// Restart of the panel computer: technician key only (also checked by the server).
$("#btn-reboot").addEventListener("click", async () => {
  if (!confirm(t("set.reboot_confirm"))) return;
  try {
    await api("/api/system/reboot", { json: {} });
    $("#reboot-overlay").hidden = false;
    setTimeout(() => { $("#reboot-overlay").hidden = true; }, 180000);  // fallback if it never went down
  } catch (err) { toast(errText(err), "bad"); }
});

$("#lang-grid").addEventListener("click", async (e) => {
  const b = e.target.closest("[data-lang]");
  if (!b || b.dataset.lang === L.lang) return;
  try {
    const res = await api("/api/settings", { json: { language: b.dataset.lang } });
    setLanguage(res.language);
    toast(t("set.lang_saved"), "ok");
  } catch (err) { toast(errText(err), "bad"); }
});

function setLanguage(lang) {
  L.lang = L.dict.languages[lang] ? lang : "sv";
  S.cache = {};
  applyStatic();
  fillDeviceTypes();
  const f = $("#point-form");
  if (!f.hidden) {
    updateLatchHint();
    $("#point-form-title").textContent = S.editOriginalId ? t("pt.form_edit", { id: S.editOriginalId }) : t("pt.form_new");
  }
  if (S.status) { renderTop(); renderOverview(); renderGraphics(); renderPointsTable(); renderSim(); }
  renderSettings(true);
  if (S.tab === "events") loadEvents();
}

// ---------------------------------------------------------------- scroll rail ("elevator")
// The panel has no browser scrollbars. Long lists get this rail instead: arrow buttons
// (tap = one page, hold = keep going), a draggable glowing handle, tap on the track to jump,
// and a label showing which rows are on screen.
const CHEV = (d) => `<svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="${d}" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/></svg>`;

function attachRail(el, itemSel) {
  const host = document.createElement("div");
  host.className = "rail-host";
  el.parentNode.insertBefore(host, el);
  host.appendChild(el);
  const rail = document.createElement("div");
  rail.className = "rail";
  rail.hidden = true;
  rail.innerHTML = `<button class="rail-btn" type="button" data-dir="-1">${CHEV("M6 15l6-6 6 6")}</button>
    <div class="rail-track"><div class="rail-thumb"><span class="rail-tip"></span></div></div>
    <button class="rail-btn" type="button" data-dir="1">${CHEV("M6 9l6 6 6-6")}</button>`;
  host.appendChild(rail);
  const [up, down] = rail.querySelectorAll(".rail-btn");
  const track = rail.querySelector(".rail-track");
  const thumb = rail.querySelector(".rail-thumb");
  const tip = rail.querySelector(".rail-tip");
  let thumbH = 40, trackH = 0, frame = 0, idle = 0;

  const max = () => el.scrollHeight - el.clientHeight;
  function label() {
    const items = el.querySelectorAll(itemSel);
    if (!items.length) return "";
    const top = el.getBoundingClientRect().top, bottom = top + el.clientHeight;
    const head = el.querySelector("thead");
    const visTop = top + (head ? head.offsetHeight : 0);
    let first = 0, last = 0;
    items.forEach((it, i) => {
      const r = it.getBoundingClientRect();
      if (r.bottom > visTop + 4 && r.top < bottom - 4) { if (!first) first = i + 1; last = i + 1; }
    });
    return first ? `${first}–${last} / ${items.length}` : "";
  }
  function update() {
    frame = 0;
    const m = max();
    const over = m > 2;
    rail.hidden = !over;
    host.classList.toggle("has-rail", over);
    if (!over) return;
    up.title = up.ariaLabel = t("rail.up");
    down.title = down.ariaLabel = t("rail.down");
    trackH = track.clientHeight;
    thumbH = Math.min(trackH, Math.max(44, trackH * el.clientHeight / el.scrollHeight));
    const y = (trackH - thumbH) * Math.min(1, el.scrollTop / m);
    thumb.style.height = `${thumbH}px`;
    thumb.style.transform = `translate(-50%, ${y}px)`;
    up.disabled = el.scrollTop <= 1;
    down.disabled = el.scrollTop >= m - 1;
    if (rail.classList.contains("moving")) tip.textContent = label();
  }
  const schedule = () => { if (!frame) frame = requestAnimationFrame(update); };
  function moving() {
    rail.classList.add("moving");
    clearTimeout(idle);
    idle = setTimeout(() => rail.classList.remove("moving"), 900);
  }
  el.addEventListener("scroll", () => { moving(); schedule(); }, { passive: true });
  new ResizeObserver(schedule).observe(el);
  new ResizeObserver(schedule).observe(track);
  new MutationObserver(schedule).observe(el, { childList: true, subtree: true });

  // arrows: tap = one page, hold = repeat
  let repeat = 0;
  const page = (dir) => el.scrollBy({ top: dir * el.clientHeight * 0.85, behavior: "smooth" });
  const stop = () => { clearTimeout(repeat); clearInterval(repeat); repeat = 0; };
  [up, down].forEach((b) => {
    b.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      const dir = Number(b.dataset.dir);
      page(dir);
      stop();
      repeat = setTimeout(() => { repeat = setInterval(() => page(dir), 260); }, 450);
    });
    ["pointerup", "pointerleave", "pointercancel"].forEach((ev) => b.addEventListener(ev, stop));
  });

  // handle: drag
  thumb.addEventListener("pointerdown", (e) => {
    e.preventDefault(); e.stopPropagation();
    thumb.setPointerCapture(e.pointerId);
    thumb.classList.add("drag");
    const y0 = e.clientY, s0 = el.scrollTop, k = max() / Math.max(1, trackH - thumbH);
    const move = (ev) => { el.scrollTop = s0 + (ev.clientY - y0) * k; moving(); };
    const end = () => {
      thumb.classList.remove("drag");
      thumb.removeEventListener("pointermove", move);
      thumb.removeEventListener("pointerup", end);
      thumb.removeEventListener("pointercancel", end);
    };
    thumb.addEventListener("pointermove", move);
    thumb.addEventListener("pointerup", end);
    thumb.addEventListener("pointercancel", end);
  });
  // track: tap = jump there
  track.addEventListener("pointerdown", (e) => {
    if (e.target !== track) return;
    const r = track.getBoundingClientRect();
    const ratio = Math.min(1, Math.max(0, (e.clientY - r.top - thumbH / 2) / Math.max(1, trackH - thumbH)));
    el.scrollTo({ top: ratio * max(), behavior: "smooth" });
  });
  schedule();
  return { update: schedule };
}

attachRail($("#active-list"), ".alarm-row");
document.querySelectorAll("#view-points .table-wrap, #view-events .table-wrap")
  .forEach((w) => attachRail(w, "tbody tr"));

async function start() {
  L.dict = await api("/i18n.json");
  S.meta = await api("/api/meta");
  const settings = await api("/api/settings");
  L.lang = L.dict.languages[settings.language] ? settings.language : "sv";
  applyStatic();
  fillDeviceTypes();
  $("#tab-sim").hidden = !S.meta.simulator && !S.meta.dev_key;
  if (!S.meta.simulator) {
    $("#sim-inputs-card").hidden = true;
    $("#sim-modules-card").hidden = true;
    $("#sim-hint").dataset.i18n = "sim.dev_hint";
    $("#sim-hint").textContent = t("sim.dev_hint");
  }
  await refresh();
  await loadPages();
  setInterval(refresh, 700);
  setInterval(() => { if (S.tab === "events") loadEvents(); }, 3000);
}
start();
