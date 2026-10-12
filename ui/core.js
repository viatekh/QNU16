// QNU16 control surface: shared state, the protocol, value formatting and widgets.
// Loaded before surface.js (the physical panel) and screens.js (the touch screen).
// Talks to qnu16d over a WebSocket; see docs/protocol.md.
'use strict';

// ===================================================================================== state
const S = {
  info: null, meta: {}, params: {}, labels: {}, meters: null, rta: null, amm: null,
  drive: null, driveFiles: [], libs: [],
  ready: false,
  sel: 'ch/1',        // channel or master whose Sel key is lit
  layer: 'lower',     // 'lower' CH1-16, 'upper' ST/FX/masters, 'custom'
  mix: 'lr',          // Mix select key: what the faders and master strip control
  hold: null,         // held key: 'assign' | 'prefade' | 'copy' | 'paste' | 'reset' | 'fn'
  flip: 0,            // GEQ fader flip: 0 off, 1 lower bands, 2 upper bands
  screen: 'proc',     // proc | route | home | fx | scenes | setup
  lastSel: 'proc',    // the Sel screen Home returns to
  tab: {},            // screen -> tab, and sub-tabs
  block: 'peq',       // Processing screen block: pre | gate | peq | geq | comp | mix
  fnOpen: false,      // the Fn page of the current screen is open
  focus: null,        // parameter box highlighted orange: {path, el}
  scenes: [], scene: 0, next: 1,
  clip: null,         // Copy buffer
  user: 'admin',
  locked: false,
  prefs: { fill: true, follow: false },
};
try { Object.assign(S.prefs, JSON.parse(localStorage.getItem('qnu16.prefs') || '{}')); } catch (e) { /* no storage */ }
try { S.user = localStorage.getItem('qnu16.user') || 'admin'; } catch (e) { /* no storage */ }
function savePrefs() {
  try { localStorage.setItem('qnu16.prefs', JSON.stringify(S.prefs)); localStorage.setItem('qnu16.user', S.user); } catch (e) { /* no storage */ }
}
let ws = null;

// Views register watchers under a tag; rebuilding a view drops its old watchers.
const watchers = new Map();
const labelWatchers = new Map();
const frameViews = new Map();
let currentTag = null;

function withTag(tag, build) {
  for (const [k, list] of watchers) watchers.set(k, list.filter(f => f.tag !== tag));
  for (const [k, list] of labelWatchers) labelWatchers.set(k, list.filter(f => f.tag !== tag));
  frameViews.set(tag, []);
  const prev = currentTag;
  currentTag = tag;
  try { build(); } finally { currentTag = prev; }
}
function watch(path, fn) {
  const f = v => fn(v);
  f.tag = currentTag;
  if (!watchers.has(path)) watchers.set(path, []);
  watchers.get(path).push(f);
  if (path in S.params) f(S.params[path]);
}
function watchAll(paths, fn) { for (const p of paths) if (has(p)) watch(p, () => fn()); fn(); }
function watchLabel(key, fn) {
  const f = v => fn(v);
  f.tag = currentTag;
  if (!labelWatchers.has(key)) labelWatchers.set(key, []);
  labelWatchers.get(key).push(f);
  f(S.labels[key] || '');
}
function onFrame(fn) {
  if (!frameViews.has(currentTag)) frameViews.set(currentTag, []);
  frameViews.get(currentTag).push(fn);
}
function setLocal(path, v) {
  S.params[path] = v;
  const list = watchers.get(path);
  if (list) for (const f of list) f(v);
}
function send(text) { if (ws && ws.readyState === 1) ws.send(text); }

const pending = new Map();
let flushQueued = false;
function setParam(path, v) {
  const m = S.meta[path];
  if (!m) return;
  v = Math.min(m.max, Math.max(m.min, v));
  if (m.kind === 't') v = v >= 0.5 ? 1 : 0;
  if (m.kind === 'h') v = Math.round(v);
  setLocal(path, v);
  pending.set(path, v);
  if (!flushQueued) {
    flushQueued = true;
    requestAnimationFrame(() => {
      flushQueued = false;
      const lines = [];
      for (const [p, val] of pending) lines.push(`set ${p} ${+val.toFixed(5)}`);
      pending.clear();
      if (lines.length) send(lines.join('\n'));
    });
  }
}
const get = p => S.params[p];
const on = p => S.params[p] >= 0.5;
const has = p => p in S.meta;
const toggle = p => setParam(p, on(p) ? 0 : 1);
const def = p => (S.meta[p] ? S.meta[p].def : 0);

// The status bar's system message area (overwrites the scene info for a moment).
let msgTimer = 0;
function message(text) {
  const t = document.getElementById('sysmsg');
  if (!t) return;
  t.textContent = text;
  t.parentElement.classList.add('msg');
  clearTimeout(msgTimer);
  msgTimer = setTimeout(() => { t.textContent = ''; t.parentElement.classList.remove('msg'); }, 2200);
}

// ================================================================================ topology
const inputs = () => (S.info ? S.info.inputs : []);
const buses = () => (S.info ? S.info.buses : []);
const inputById = id => inputs().find(i => i.id === id);
const busById = id => buses().find(b => b.id === id);
const isDca = id => /^dca\//.test(id);
const num = id => +id.split('/')[1];
const kindOf = id => (inputById(id) || busById(id) || { kind: isDca(id) ? 'dca' : '?' }).kind;
const isInput = id => !!inputById(id);
const isChannel = id => /^(ch|st)\//.test(id);   // CH and ST: full input processing
const isMaster = id => /^(lr|mix\/)/.test(id);    // LR and mixes: PEQ, GEQ, comp, delay

// Qu names: CH1, ST1, FX1 Ret, FX1 (send), Mix1, Mix5-6, LR, DCA1.
function quName(id) {
  if (id === 'lr') return 'LR';
  const [k, n] = id.split('/');
  switch (k) {
    case 'ch': return 'CH' + n;
    case 'st': return 'ST' + n;
    case 'fxret': return `FX${n} Ret`;
    case 'fx': return `FX${n}`;
    case 'dca': return 'DCA' + n;
    case 'mg': return 'Mute Grp ' + n;
    case 'mix': { const b = busById(id); return b ? b.name.replace(' ', '') : 'Mix' + n; }
  }
  return id;
}
// Channel header: "CH 7" over the name, as at the top left of the Processing screen.
function headerName(id) {
  const q = quName(id);
  return q.replace(/^(CH|ST|Mix|FX)(\d)/, '$1 $2');
}
const userName = id => S.labels[id] || '';
const stripLabel = id => userName(id) || quName(id);

// Custom layer strips (engine numbering: 0 X, 1..23 inputs, 24..33 masters, 34..37 DCAs).
function customStripId(n) {
  if (n <= 0) return null;
  if (n <= inputs().length) return inputs()[n - 1].id;
  if (n <= inputs().length + buses().length) return buses()[n - inputs().length - 1].id;
  return 'dca/' + (n - inputs().length - buses().length);
}
function customStripNames() {
  const n = 1 + inputs().length + buses().length + S.info.dcas;
  return Array.from({ length: n }, (_, i) => (i ? quName(customStripId(i)) : 'X'));
}

// =============================================================================== meters
// Meter frame: per input [in, out, gr, gate], per bus [L, R, gr], then PAFL L/R. dBFS.
function inMeter(id) {
  const i = inputs().findIndex(x => x.id === id);
  if (i < 0 || !S.meters) return { in: -90, out: -90, gr: 0, gate: 1 };
  const m = S.meters;
  return { in: m[i * 4], out: m[i * 4 + 1], gr: m[i * 4 + 2], gate: m[i * 4 + 3] };
}
function busMeter(id) {
  const b = buses().findIndex(x => x.id === id);
  if (b < 0 || !S.meters) return { l: -90, r: -90, gr: 0 };
  const base = inputs().length * 4 + b * 3;
  return { l: S.meters[base], r: S.meters[base + 1], gr: S.meters[base + 2] };
}
function paflMeter() {
  if (!S.meters) return { l: -90, r: -90 };
  const n = S.meters.length;
  return { l: S.meters[n - 2], r: S.meters[n - 1] };
}
// Pre-fader channel level (inputs), post-fader (masters), as the strip LEDs.
function stripLevel(id) {
  if (isInput(id)) return inMeter(id).in;
  if (busById(id)) { const m = busMeter(id); return Math.max(m.l, m.r); }
  return -90;
}
function paflActive() {
  for (const p in S.params) if (p.endsWith('/pafl') && S.params[p] >= 0.5) return true;
  return false;
}

// ============================================================================ value mapping
// Fader law: positions of the Qu's fader scale marks (+10 at the top, -inf at the bottom).
const FADER_TAPER = [[0, -90], [0.06, -50], [0.15, -40], [0.27, -30], [0.40, -20], [0.53, -10], [0.63, -5], [0.73, 0], [0.865, 5], [1, 10]];
function faderPos(db) {
  if (db <= -89.9) return 0;
  for (let i = 1; i < FADER_TAPER.length; i++) {
    const [p1, d1] = FADER_TAPER[i - 1], [p2, d2] = FADER_TAPER[i];
    if (db <= d2) return p1 + (db - d1) / (d2 - d1) * (p2 - p1);
  }
  return 1;
}
function faderDb(pos) {
  pos = Math.min(1, Math.max(0, pos));
  if (pos <= 0.004) return -90;
  for (let i = 1; i < FADER_TAPER.length; i++) {
    const [p1, d1] = FADER_TAPER[i - 1], [p2, d2] = FADER_TAPER[i];
    if (pos <= p2) return d1 + (pos - p1) / (p2 - p1) * (d2 - d1);
  }
  return 10;
}
function taper(path) {
  const m = S.meta[path];
  if (!m || m.kind !== 'c') return 'lin';
  if (m.unit === 'dB' && m.min <= -89) return 'fader';
  if (['Hz', 'ms', 's', 'oct', ':1'].includes(m.unit) && m.min > 0) return 'log';
  return 'lin';
}
function toPos(path, v) {
  const m = S.meta[path];
  if (!m || v == null) return 0;
  switch (taper(path)) {
    case 'fader': return faderPos(v);
    case 'log': return Math.log(v / m.min) / Math.log(m.max / m.min);
    default: return (v - m.min) / (m.max - m.min || 1);
  }
}
function fromPos(path, p) {
  const m = S.meta[path];
  p = Math.min(1, Math.max(0, p));
  switch (taper(path)) {
    case 'fader': return faderDb(p);
    case 'log': return m.min * Math.pow(m.max / m.min, p);
    default: return m.min + p * (m.max - m.min);
  }
}
// One rotary detent (or wheel notch) for a parameter.
function nudge(path, dir, fine) {
  const m = S.meta[path];
  if (!m) return;
  const v = get(path);
  if (m.kind === 'h') { setParam(path, Math.min(m.max, Math.max(m.min, v + dir))); return; }
  if (m.kind === 't') { setParam(path, dir > 0 ? 1 : 0); return; }
  if (m.unit === 'dB' && taper(path) !== 'fader') { setParam(path, Math.round((v + dir * (fine ? 0.1 : 0.5)) * 10) / 10); return; }
  if (taper(path) === 'fader' && v <= -89.9 && dir > 0) { setParam(path, -50); return; }
  setParam(path, fromPos(path, toPos(path, v) + dir * (fine ? 0.002 : 0.01)));
}

// ======================================================================= Qu-style formatting
const FX_TARGET_INPUTS = 19;  // CH1-16, ST1-3 in fxTargets
const SOFT_FUNCS = ['Unassigned', 'Channel Mute', 'Mute Group', 'DCA Mute', 'Tap Tempo L+R', 'Tap Tempo L',
  'Tap Tempo R', 'Recall Scene', 'Scene Store Current', 'Scene Recall Go', 'Scene Next', 'Scene Previous',
  'PAFL Clear', 'QuDrive Stereo Play', 'QuDrive Stereo Rec', 'QuDrive Multitrack', 'Talkback'];
const COMP_TYPES = ['Manual Peak', 'Manual RMS', 'Auto Slow Opto', 'Auto Punchbag'];
const GEN_TYPES = ['Sine', 'White noise', 'Pink noise', 'Band-pass noise'];
const PATCH_TYPES = ['Unassigned', 'Insert', 'Mix>Return', 'Ch>Return'];
const DOUT_SRC = ['Post Preamp', 'Post Insert', 'Post PEQ', 'Post Comp', 'Post Delay'];
const MIX_SRC = ['Post-Preamp', 'Pre-EQ', 'Post-EQ', 'Post-All'];

function choiceLabels(path) {
  if (/^fx\/\d\/type$/.test(path)) return S.info.fxTypes;
  if (/^fx\/\d\/rev\/model$/.test(path)) return S.info.reverbModels;
  if (/^fx\/\d\/patch$/.test(path)) return PATCH_TYPES;
  if (/^fx\/\d\/src$/.test(path)) return S.info.fxTargets.map((n, i) => (i ? fxTargetName(i) : 'None'));
  if (/^fx\/\d\/pha\/stages$/.test(path)) return ['4', '6', '8', '12'];
  if (path.endsWith('comp/type')) return COMP_TYPES;
  if (path.endsWith('insert/type')) return ['FX Insert', 'Ducker Insert'];
  if (path.endsWith('duck/trigger')) return ['None', ...inputs().slice(0, FX_TARGET_INPUTS).map(i => quName(i.id))];
  if (path.endsWith('/src') && /^(ch|st)\//.test(path)) return ['Local', 'USB'];
  if (path.endsWith('/presrc')) return MIX_SRC;
  if (path === 'dout/src') return DOUT_SRC;
  if (path === 'gen/type') return GEN_TYPES;
  if (path === 'usb/chsrc') return ['Insert Sends', 'Direct Outs'];
  if (path === 'talk/src') return ['None', ...Array.from({ length: S.info.physInputs }, (_, i) => `Input ${i + 1}`)];
  if (path.startsWith('out/')) return S.info.outputSources;
  if (path.startsWith('usb/')) return S.info.usbSources;
  if (path.startsWith('custom/')) return customStripNames();
  if (/^soft\/\d\/func$/.test(path)) return SOFT_FUNCS;
  return null;
}
function fxTargetName(i) {
  if (i <= 0) return 'None';
  if (i <= FX_TARGET_INPUTS) return quName(inputs()[i - 1].id);
  return quName(buses()[i - 1 - FX_TARGET_INPUTS].id);
}

function fmtHz(v) {
  if (v >= 10000) return (v / 1000).toFixed(1) + 'kHz';
  if (v >= 1000) return (v / 1000).toFixed(2) + 'kHz';
  if (v >= 100) return Math.round(v) + 'Hz';
  return v.toFixed(1) + 'Hz';
}
function fmtMs(v) {
  if (v > 0 && v < 1) return Math.round(v * 1000) + ' µs';
  if (v >= 1000) return (v / 1000).toFixed(2) + ' s';
  if (v >= 100) return Math.round(v) + ' ms';
  return v.toFixed(1) + ' ms';
}
const WIDTH_NAMES = [[1.5, '1.5'], [1, '1'], [0.75, '3/4'], [2 / 3, '2/3'], [0.5, '1/2'], [1 / 3, '1/3'], [0.25, '1/4'], [1 / 6, '1/6'], [1 / 9, '1/9']];
function fmtWidth(oct) {
  if (oct > 1.5) return 'Shelf';
  for (const [w, n] of WIDTH_NAMES) if (Math.abs(oct - w) < 0.004) return n;
  return oct.toFixed(2);
}
function fmtPan(v) {
  if (Math.abs(v) < 0.01) return 'C';
  return (v < 0 ? 'L' : 'R') + Math.round(Math.abs(v) * 100);
}
function fmt(path, v) {
  const m = S.meta[path];
  if (v == null || !m) return '';
  if (m.kind === 't') return v >= 0.5 ? 'On' : 'Off';
  if (m.kind === 'h') { const l = choiceLabels(path); return l ? l[v] : String(v); }
  if (/\/(pan|balance)$/.test(path)) return fmtPan(v);
  switch (m.unit) {
    case 'dB': return (v <= -89.9 ? '-inf' : v.toFixed(1)) + ' dB';
    case 'Hz': return fmtHz(v);
    case 'ms': return fmtMs(v);
    case 's': return v.toFixed(2) + ' s';
    case '%': return Math.round(v) + ' %';
    case ':1': return v >= 49.9 ? 'Inf:1' : v.toFixed(1) + ':1';
    case 'oct': return fmtWidth(v);
  }
  return v.toFixed(2);
}

// =================================================================================== widgets
function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}
function svgEl(tag, attrs, parent) {
  const e = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const a in attrs) e.setAttribute(a, attrs[a]);
  if (parent) parent.append(e);
  return e;
}
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

// Vertical drag in screen pixels (works with the scaled console).
function dragY(elm, begin, move, pixels = 160) {
  let startY = 0, start = 0, active = false;
  elm.addEventListener('pointerdown', e => {
    if (e.button > 0) return;
    const b = begin(e);
    if (b === false) return;
    active = true; elm.setPointerCapture(e.pointerId);
    startY = e.clientY; start = b;
  });
  elm.addEventListener('pointermove', e => {
    if (!active) return;
    move(start + (startY - e.clientY) / (pixels * scale()) * (e.shiftKey ? 0.2 : 1), e);
  });
  const end = () => { active = false; };
  elm.addEventListener('pointerup', end);
  elm.addEventListener('pointercancel', end);
}
let consoleScale = 1;
const scale = () => consoleScale;

// ---- Panel (physical) controls ----

// Panel key with an LED. cls: colour class (white, green, grey, blue, mix).
function pkey(text, cls, onClick) {
  const b = el('button', 'pk ' + (cls || ''));
  b.append(el('i', 'led'), el('span', 'kt', text));
  if (onClick) b.addEventListener('click', onClick);
  return b;
}
// Panel rotary (SuperStrip). pathFn gives the current parameter (or null: inactive).
function rotary(pathFn, label, opts = {}) {
  const k = el('div', 'rot' + (opts.cls ? ' ' + opts.cls : ''));
  const svg = svgEl('svg', { viewBox: '0 0 44 44' });
  const arcPath = (p0, p1) => {
    const a0 = (-135 + 270 * p0) * Math.PI / 180, a1 = (-135 + 270 * p1) * Math.PI / 180;
    const x = a => 22 + 19 * Math.sin(a), y = a => 22 - 19 * Math.cos(a);
    return `M${x(a0).toFixed(2)} ${y(a0).toFixed(2)} A19 19 0 ${a1 - a0 > Math.PI ? 1 : 0} 1 ${x(a1).toFixed(2)} ${y(a1).toFixed(2)}`;
  };
  svgEl('path', { class: 'trk', d: arcPath(0, 1) }, svg);
  const arc = svgEl('path', { class: 'arc' }, svg);
  svgEl('circle', { class: 'body', cx: 22, cy: 22, r: 14 }, svg);
  const ptr = svgEl('line', { class: 'ptr', x1: 22, y1: 22, x2: 22, y2: 10 }, svg);
  k.append(svg);
  if (label) k.append(el('div', 'rl', label));
  k.render = () => {
    const path = pathFn();
    k.classList.toggle('na', !path || !has(path));
    if (!path || !has(path)) { arc.setAttribute('d', ''); ptr.setAttribute('transform', 'rotate(-135 22 22)'); k.title = ''; return; }
    const v = get(path), m = S.meta[path];
    const p = clamp(toPos(path, v), 0, 1);
    const c = m.min < 0 && m.max > 0 && taper(path) !== 'fader' ? toPos(path, 0) : 0;
    arc.setAttribute('d', p >= c ? arcPath(c, Math.max(p, c + 0.001)) : arcPath(p, c));
    ptr.setAttribute('transform', `rotate(${-135 + 270 * p} 22 22)`);
    k.title = `${label || ''} ${fmt(path, v)}`;
  };
  dragY(k, () => { const p = pathFn(); if (!p || !has(p)) return false; panelTouched(p); return toPos(p, get(p)); },
    pos => { const p = pathFn(); setParam(p, fromPos(p, pos)); }, 200);
  k.addEventListener('wheel', e => { const p = pathFn(); if (!p) return; e.preventDefault(); nudge(p, -Math.sign(e.deltaY), e.shiftKey); }, { passive: false });
  k.addEventListener('dblclick', () => { const p = pathFn(); if (p) setParam(p, def(p)); });
  return k;
}
// Hook: surface controls tell the screen which block they belong to (Processing follows surface).
let panelTouched = () => {};

// Strip LEDs: Pk (-3 dBFS, held), 0 (0 dBu = -18 dBFS), Sig (-26 dBu = -44 dBFS).
function stripLeds() {
  const m = el('div', 'sleds');
  const mk = (cls, t) => { const r = el('div', 'lrow'); const i = el('i', cls); r.append(el('span', '', t), i); m.append(r); return i; };
  const pk = mk('red', 'Pk'), nom = mk('yel', '0'), sig = mk('grn', 'Sig');
  let hold = 0;
  m.set = db => {
    sig.classList.toggle('on', db >= -44);
    nom.classList.toggle('on', db >= -18);
    hold = db >= -3 ? 14 : Math.max(0, hold - 1);
    pk.classList.toggle('on', hold > 0);
  };
  m.raw = (p, n, s) => { pk.classList.toggle('on', p); nom.classList.toggle('on', n); sig.classList.toggle('on', s); };
  return m;
}

// Motorised fader. pathFn: current parameter (or null). opts.geq: ±12 dB linear law.
function fader(pathFn, opts = {}) {
  const f = el('div', 'fdr');
  const slot = el('div', 'slot');
  f.append(slot);
  const scaleMarks = opts.geq ? [12, 6, 0, -6, -12] : [10, 5, 0, -5, -10, -20, -30, -40, -90];
  const toP = v => (opts.geq ? (v + 12) / 24 : faderPos(v));
  const fromP = p => (opts.geq ? Math.round((clamp(p, 0, 1) * 24 - 12) * 2) / 2 : faderDb(p));
  for (const db of scaleMarks) {
    const t = el('div', 'mark' + (db === 0 ? ' z' : ''));
    t.style.bottom = `calc(14px + (100% - 28px) * ${toP(db)})`;
    t.dataset.t = db <= -89 ? '∞' : Math.abs(db);
    f.append(t);
  }
  const cap = el('div', 'cap');
  f.append(cap);
  f.render = () => {
    const path = pathFn();
    const v = path && has(path) ? get(path) : (opts.geq ? 0 : -90);
    cap.style.bottom = `calc(14px + (100% - 28px) * ${toP(v)})`;
    f.classList.toggle('na', !path);
  };
  const set = v => { const p = pathFn(); if (p) { setParam(p, v); f.render(); } };
  const posAt = e => {
    const r = f.getBoundingClientRect(), pad = 14 * scale();
    return (r.bottom - pad - e.clientY) / (r.height - 2 * pad);
  };
  let dragging = false, startY = 0, startPos = 0;
  f.addEventListener('pointerdown', e => {
    const p = pathFn();
    if (!p) return;
    dragging = true; f.setPointerCapture(e.pointerId);
    startY = e.clientY;
    startPos = toP(get(p));
    if (e.target !== cap) { startPos = posAt(e); set(fromP(startPos)); }
  });
  f.addEventListener('pointermove', e => {
    if (!dragging) return;
    const r = f.getBoundingClientRect();
    set(fromP(startPos + (startY - e.clientY) / (r.height - 28 * scale()) * (e.shiftKey ? 0.25 : 1)));
  });
  const end = () => { dragging = false; };
  f.addEventListener('pointerup', end);
  f.addEventListener('pointercancel', end);
  f.addEventListener('dblclick', () => set(0));
  f.addEventListener('wheel', e => { const p = pathFn(); if (!p) return; e.preventDefault(); nudge(p, -Math.sign(e.deltaY), e.shiftKey); f.render(); }, { passive: false });
  return f;
}

// ---- Touch screen controls ----

// Screen button. Toggle buttons show On/In (green) and Off/Out (grey).
function sbtn(text, onClick, cls = '') {
  const b = el('button', 'sb ' + cls, text);
  if (onClick) b.addEventListener('click', onClick);
  return b;
}
function tbtn(path, onText = 'On', offText = 'Off', opts = {}) {
  const b = sbtn('', () => {
    if (opts.hold) return;
    if (S.hold === 'reset') { setParam(path, def(path)); return; }
    toggle(path);
  }, 'tog ' + (opts.cls || ''));
  if (opts.hold) holdPress(b, () => toggle(path));
  watch(path, v => { b.classList.toggle('on', v >= 0.5); b.textContent = v >= 0.5 ? onText : offText; });
  return b;
}
// Buttons that need a 1 second touch (48V, Reset Mix Settings).
function holdPress(b, fn) {
  let t = 0;
  b.addEventListener('pointerdown', () => { b.classList.add('holding'); t = setTimeout(() => { b.classList.remove('holding'); fn(); }, 1000); });
  const cancel = () => { clearTimeout(t); b.classList.remove('holding'); };
  b.addEventListener('pointerup', cancel);
  b.addEventListener('pointerleave', cancel);
}

// Parameter box: touch to highlight it orange, then turn the screen rotary (or drag / wheel).
function pbox(path, label, opts = {}) {
  const b = el('div', 'pbox' + (opts.cls ? ' ' + opts.cls : ''));
  const l = el('div', 'pl', label), v = el('div', 'pv');
  b.append(l, v);
  b.dataset.path = path;
  if (!has(path)) { b.classList.add('na'); return b; }
  watch(path, x => { v.textContent = opts.format ? opts.format(x) : fmt(path, x); });
  b.addEventListener('pointerdown', () => {
    if (S.hold === 'reset') { setParam(path, def(path)); return; }
    focusBox(path, b);
  });
  dragY(b, () => toPos(path, get(path)), pos => setParam(path, fromPos(path, pos)), 220);
  b.addEventListener('wheel', e => { e.preventDefault(); focusBox(path, b); nudge(path, -Math.sign(e.deltaY), e.shiftKey); }, { passive: false });
  if (S.focus && S.focus.path === path) { S.focus.el = b; b.classList.add('focus'); }
  return b;
}
function focusBox(path, b) {
  if (S.focus && S.focus.el) S.focus.el.classList.remove('focus');
  S.focus = { path, el: b };
  b.classList.add('focus');
}
// The screen rotary turns the highlighted box.
function rotaryTurn(dir, fine) {
  if (S.focus && has(S.focus.path)) nudge(S.focus.path, dir, fine);
  else if (S.focus && S.focus.turn) S.focus.turn(dir);
}

// Option menu: a box with ▽ that opens a list (choices).
function omenu(path, opts = {}) {
  const b = el('button', 'om' + (opts.cls ? ' ' + opts.cls : ''));
  const t = el('span', 'omt');
  b.append(t, el('i', 'tri'));
  const labels = () => opts.labels || choiceLabels(path) || [];
  watch(path, v => { t.textContent = labels()[v] ?? ''; });
  b.addEventListener('click', () => {
    const items = labels().map((n, i) => ({ text: n, value: i })).filter(x => !opts.filter || opts.filter(x.value));
    pickList(items, get(path), v => setParam(path, v), b);
  });
  return b;
}
// List overlay inside the screen; also used for libraries and tracks.
function pickList(items, current, choose, anchor, title) {
  const ov = overlay('picklist');
  if (title) ov.box.append(el('div', 'ovt', title));
  const list = el('div', 'plist');
  for (const it of items) {
    const r = el('button', 'pli' + (it.value === current ? ' cur' : ''), it.text);
    r.addEventListener('click', () => { choose(it.value); ov.close(); });
    list.append(r);
  }
  ov.box.append(list);
  const cur = list.querySelector('.cur');
  if (cur) cur.scrollIntoView({ block: 'center' });
  // The rotary scrolls the list.
  S.focus = { path: null, el: null, turn: d => { list.scrollTop += d * 34; } };
  return ov;
}
// Modal panel over the screen content (keeps the status bar visible).
function overlay(cls = '') {
  const host = document.getElementById('scr');
  const ov = el('div', 'ov ' + cls);
  const box = el('div', 'ovb');
  ov.append(box);
  ov.addEventListener('pointerdown', e => { if (e.target === ov) ov.close(); });
  ov.close = () => { ov.remove(); };
  ov.box = box;
  host.append(ov);
  return ov;
}

// Screen keypad for names (channel names 6 characters, scenes and libraries 14).
function keypad(title, initial, maxLen, done) {
  const ov = overlay('kpad');
  ov.box.append(el('div', 'ovt', title));
  const field = el('div', 'kfield');
  let text = initial || '', shift = 0;  // 0 lower, 1 once, 2 caps lock
  const cursor = el('i', 'kcur');
  const draw = () => { field.textContent = text; field.append(cursor); };
  draw();
  ov.box.append(field);
  const rows = [['1', '2', '3', '4', '5', '6', '7', '8', '9', '0'],
    ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'],
    ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '⌫'],
    ['⇧', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '-', ':'],
    ['Cancel', '␣', 'OK']];
  const keys = [];
  for (const r of rows) {
    const row = el('div', 'krow');
    for (const k of r) {
      const b = el('button', 'kk' + (k.length > 1 ? ' wide' : '') + (k === '␣' ? ' space' : ''), k === '␣' ? '' : k);
      b.addEventListener('click', () => {
        if (k === '⌫') text = text.slice(0, -1);
        else if (k === '⇧') shift = shift === 0 ? 1 : shift === 1 ? 2 : 0;
        else if (k === 'Cancel') { ov.close(); return; }
        else if (k === 'OK') { ov.close(); done(text.trim()); return; }
        else if (text.length < maxLen) {
          text += k === '␣' ? ' ' : shift ? k.toUpperCase() : k;
          if (shift === 1) shift = 0;
        }
        keys.forEach(x => { if (x.dataset.k.length === 1) x.textContent = shift ? x.dataset.k.toUpperCase() : x.dataset.k; });
        draw();
      });
      b.dataset.k = k;
      keys.push(b);
      row.append(b);
    }
    ov.box.append(row);
  }
  const onKey = e => {
    if (!ov.isConnected) { window.removeEventListener('keydown', onKey); return; }
    if (e.key === 'Enter') { ov.close(); done(text.trim()); }
    else if (e.key === 'Escape') ov.close();
    else if (e.key === 'Backspace') { text = text.slice(0, -1); draw(); }
    else if (e.key.length === 1 && text.length < maxLen) { text += e.key; draw(); }
    else return;
    e.preventDefault();
  };
  window.addEventListener('keydown', onKey);
}
function rename(id) {
  keypad('Enter Channel Name:', userName(id), S.info.nameChars || 6, t => send(`label ${id} ${t}`));
}

// Small meter bar (screen). getDb returns dBFS; Qu colours: green, yellow above 0 dBu, red near clip.
function mbar(getDb, cls = '') {
  const m = el('div', 'mbar ' + cls);
  const fill = el('i', 'fill'), pk = el('i', 'pkh');
  m.append(fill, pk);
  let peak = -90, hold = 0;
  onFrame(() => {
    const db = getDb();
    const p = clamp((db + 60) / 60, 0, 1);
    fill.style.height = (p * 100) + '%';
    fill.className = 'fill' + (db >= -3 ? ' red' : db >= -18 ? ' yel' : '');
    if (db >= peak) { peak = db; hold = 30; } else if (--hold < 0) peak = Math.max(-90, peak - 1);
    pk.style.bottom = (clamp((peak + 60) / 60, 0, 1) * 100) + '%';
  });
  return m;
}
// Gain-reduction meter (red, from the top).
function grbar(getGr) {
  const m = el('div', 'mbar gr');
  const fill = el('i', 'fill red');
  m.append(fill);
  onFrame(() => { fill.style.height = clamp(getGr() / 30, 0, 1) * 100 + '%'; });
  return m;
}

// ============================================================================== connection
function handleMessage(text) {
  const c = text[0];
  if (text.startsWith('m ')) { S.meters = text.slice(2).split(',').map(Number); return; }
  if (text.startsWith('r ')) { S.rta = text.slice(2).split(',').map(Number); spectroPush(); return; }
  if (text.startsWith('a ')) { S.amm = text.slice(2).split(',').map(Number); return; }
  if (text.startsWith('d ')) {
    const [state, mode, secs, len, drops, ...file] = text.slice(2).split(' ');
    S.drive = { state, mode, seconds: +secs || 0, length: +len || 0, dropouts: +drops || 0, file: file.join(' ') };
    if (S.ready) statusIcons();
    return;
  }
  if (c === 'q' && (text.length === 1 || text[1] === '\n')) { S.driveFiles = text.split('\n').slice(1).filter(Boolean); if (S.ready) refreshScreen('home'); return; }
  if (c === 'b' && (text.length === 1 || text[1] === '\n')) {
    S.libs = text.split('\n').slice(1).filter(Boolean).map(l => { const [n, type, ...name] = l.split(' '); return { n: +n, type, name: name.join(' ') }; });
    if (S.ready && S.fnOpen) refreshScreen();
    return;
  }
  if (text.startsWith('B ')) { const cb = libWaiters.shift(); if (cb) cb(text.slice(2)); return; }
  if (text.startsWith('i ')) { S.info = JSON.parse(text.slice(2)); return; }
  if (text.startsWith('k\n')) {
    for (const line of text.split('\n').slice(1)) {
      const [path, min, max, d, kind, unit] = line.split(' ');
      S.meta[path] = { min: +min, max: +max, def: +d, kind, unit: unit || '' };
    }
    return;
  }
  if (text.startsWith('s ')) {
    const lines = text.split('\n');
    S.scene = +lines[0].slice(2) || 0;
    S.scenes = lines.slice(1).filter(Boolean).map(l => {
      const [n, filter, ...name] = l.split(' ');
      return { n: +n, filter: filter === '-' ? [] : filter.split(','), name: name.join(' ') };
    });
    if (S.ready) { statusScene(); refreshScreen('scenes'); }
    return;
  }
  const first = !S.ready;
  let layout = false;
  for (const line of text.split('\n')) {
    if (line.startsWith('p ')) {
      const sp = line.indexOf(' ', 2);
      const path = line.slice(2, sp);
      setLocal(path, parseFloat(line.slice(sp + 1)));
      if (path.startsWith('custom/') || path.startsWith('link/')) layout = true;
    } else if (line.startsWith('l ')) {
      const sp = line.indexOf(' ', 2);
      const k = sp < 0 ? line.slice(2) : line.slice(2, sp);
      S.labels[k] = sp < 0 ? '' : line.slice(sp + 1);
      (labelWatchers.get(k) || []).forEach(f => f(S.labels[k]));
    } else if (line.startsWith('e ')) {
      message(line.slice(2));
    }
  }
  if (first && S.info && Object.keys(S.params).length) {
    S.ready = true;
    buildSurface();
  } else if (layout && S.ready) {
    renderBank();
  }
}
const libWaiters = [];
function libGet(n, cb) { libWaiters.push(cb); send(`lib get ${n}`); }

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => document.body.classList.add('online');
  ws.onmessage = e => handleMessage(e.data);
  ws.onclose = () => {
    document.body.classList.remove('online');
    S.ready = false;
    for (const k of Object.keys(S.labels)) delete S.labels[k];
    setTimeout(connect, 1000);
  };
}

function animate() {
  if (S.meters && S.ready) for (const list of frameViews.values()) for (const f of list) f();
  requestAnimationFrame(animate);
}
