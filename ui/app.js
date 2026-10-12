// QNU16 control surface, laid out and behaving like the Qu-16 top panel:
//   SuperStrip (top left) - one knob per function for the selected channel or master
//   Touch screen (top right) with screen-select keys: Home (Meters / RTA / Drive), Processing,
//     Routing, FX, Scenes, Setup
//   Fader strips (bottom left) with layer select: 1-16, 17-32 (ST, FX returns, FX/mix masters)
//     and Custom; GEQ fader flip
//   Mix keys, Assign / Pre Fade, SoftKeys, Copy / Paste / Reset / Fn, talkback, monitor level,
//     recorder transport and the master strip (bottom right)
// Talks to qnu16d over a WebSocket; see docs/protocol.md.
'use strict';

// ===================================================================================== state
const S = {
  info: null, meta: {}, params: {}, labels: {}, meters: null, rta: null, rec: null,
  ready: false,
  sel: 'ch/1',       // selected channel or master
  layer: '1',        // '1' = Ch 1-16, '2' = 17-32, 'C' = custom
  mix: 'lr',         // mix key: which mix the faders and master strip control
  mode: null,        // latched key: 'assign' | 'prefade' | 'copy' | 'paste' | 'reset'
  fn: false,         // Fn key
  flip: false,       // GEQ fader flip
  screen: 'home',
  tab: {},           // screen -> tab
  scenes: [], scene: '',
  clip: null,        // copy buffer {kind, block, values}
  recMode: 'multi',
};
let ws = null;

const watchers = new Map();
const labelWatchers = new Map();
const meterViews = new Map();
let currentTag = null;

function withTag(tag, build) {
  for (const [k, list] of watchers) watchers.set(k, list.filter(f => f.tag !== tag));
  for (const [k, list] of labelWatchers) labelWatchers.set(k, list.filter(f => f.tag !== tag));
  meterViews.set(tag, []);
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
function watchLabel(key, fn) {
  const f = v => fn(v);
  f.tag = currentTag;
  if (!labelWatchers.has(key)) labelWatchers.set(key, []);
  labelWatchers.get(key).push(f);
  f(S.labels[key] || '');
}
function onFrame(fn) {
  if (!meterViews.has(currentTag)) meterViews.set(currentTag, []);
  meterViews.get(currentTag).push(fn);
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
      for (const [p, val] of pending) lines.push(`set ${p} ${+val.toFixed(4)}`);
      pending.clear();
      if (lines.length) send(lines.join('\n'));
    });
  }
}
const get = p => S.params[p];
const on = p => S.params[p] >= 0.5;
const has = p => p in S.meta;
const toggle = p => setParam(p, on(p) ? 0 : 1);

let toastTimer = 0;
function toast(text) {
  const t = document.getElementById('toast');
  t.textContent = text;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.textContent = ''; }, 2500);
}

// ================================================================================ topology
const inputs = () => (S.info ? S.info.inputs : []);
const buses = () => (S.info ? S.info.buses : []);
const inputById = id => inputs().find(i => i.id === id);
const busById = id => buses().find(b => b.id === id);
const isDca = id => id.startsWith('dca/');
const idx = id => +id.split('/')[1];
function displayName(id) {
  if (S.labels[id]) return S.labels[id];
  if (isDca(id)) return `DCA ${idx(id)}`;
  if (id.startsWith('fxret/') && S.labels['fx/' + idx(id)]) return S.labels['fx/' + idx(id)] + ' Ret';
  const d = inputById(id) || busById(id);
  return d ? d.name : id;
}
function colorOf(id) {
  if (isDca(id)) return 'var(--c-dca)';
  const i = inputById(id);
  if (i) return i.kind === 'mono' ? 'var(--c-in)' : i.kind === 'stereo' ? 'var(--c-st)' : 'var(--c-fxr)';
  const b = busById(id);
  return b ? `var(--c-${b.kind})` : '#555';
}
const mixKind = () => (busById(S.mix) || { kind: 'lr' }).kind;

// Custom layer strip index -> id (mirrors the engine: 0 none, inputs, masters, DCAs).
function customStripId(n) {
  if (n <= 0) return null;
  const list = [...inputs().map(i => i.id), ...buses().map(b => b.id), ...Array.from({ length: S.info.dcas }, (_, i) => `dca/${i + 1}`)];
  return list[n - 1] || null;
}
function customStripNames() {
  return ['None', ...inputs().map(i => i.name), ...buses().map(b => b.name + ' master'), ...Array.from({ length: S.info.dcas }, (_, i) => `DCA ${i + 1}`)];
}

// Meter frame: per input [in, out, gr, gate], per bus [L, R, gr], then PAFL L/R.
function inMeter(id) {
  const i = inputs().findIndex(x => x.id === id);
  if (i < 0 || !S.meters) return null;
  const o = i * 4;
  return { in: S.meters[o], out: S.meters[o + 1], gr: S.meters[o + 2], gate: S.meters[o + 3] };
}
function busMeter(id) {
  const b = buses().findIndex(x => x.id === id);
  if (b < 0 || !S.meters) return null;
  const o = inputs().length * 4 + b * 3;
  return { l: S.meters[o], r: S.meters[o + 1], gr: S.meters[o + 2] };
}
function paflActive() {
  return inputs().some(i => on(`${i.id}/pafl`)) || buses().some(b => on(`${b.id}/pafl`));
}

// ============================================================================ value mapping
const FADER_TAPER = [[0, -90], [0.04, -60], [0.15, -40], [0.32, -25], [0.5, -12], [0.72, 0], [1, 10]];
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
  if (pos <= 0.003) return -90;
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
  if ((m.unit === 'Hz' || m.unit === 'ms' || m.unit === 's' || m.unit === 'oct') && m.min > 0) return 'log';
  if (m.unit === ':1') return 'log';
  return 'lin';
}
function toPos(path, v) {
  const m = S.meta[path];
  if (!m || v == null) return 0;
  switch (taper(path)) {
    case 'fader': return faderPos(v);
    case 'log': return Math.log(v / m.min) / Math.log(m.max / m.min);
    default: return (v - m.min) / (m.max - m.min);
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
const SOFTKEY_FUNCS = ['None', 'Mute Grp 1', 'Mute Grp 2', 'Mute Grp 3', 'Mute Grp 4', 'DCA 1 Mute', 'DCA 2 Mute',
  'DCA 3 Mute', 'DCA 4 Mute', 'Talkback', 'Tap FX1', 'Tap FX2', 'Tap FX3', 'Tap FX4', 'Scene Next', 'Scene Prev'];
function choiceLabels(path) {
  if (/^fx\/\d\/type$/.test(path)) return S.info.fxTypes;
  if (/^fx\/\d\/src$/.test(path)) return ['Off', ...buses().filter(b => b.kind === 'mix').map(b => displayName(b.id))];
  if (path.endsWith('eq/1/type') || path.endsWith('eq/4/type')) return ['Shelf', 'Bell'];
  if (path.endsWith('comp/mode')) return ['Manual Peak', 'Manual RMS', 'Slow Opto', 'Punchbag'];
  if (path === 'gen/type') return ['Sine', 'White', 'Pink'];
  if (path === 'talk/src') return ['None', ...Array.from({ length: S.info.physInputs }, (_, i) => `Input ${i + 1}`)];
  if (path.startsWith('out/')) return S.info.outputSources;
  if (path.startsWith('custom/')) return customStripNames();
  if (path.startsWith('soft/')) return SOFTKEY_FUNCS;
  return null;
}
function fmtWidth(oct) {
  const inv = 1 / oct;
  if (Math.abs(inv - Math.round(inv)) < 0.05 && inv >= 1.5) return `1/${Math.round(inv)}`;
  return oct.toFixed(2);
}
function fmt(path, v) {
  const m = S.meta[path];
  if (v == null || !m) return '';
  if (m.kind === 't') return v >= 0.5 ? 'ON' : 'OFF';
  if (m.kind === 'h') { const l = choiceLabels(path); return l ? l[v] : String(v); }
  if (path.endsWith('/pan') || path.endsWith('/balance'))
    return Math.abs(v) < 0.02 ? 'C' : (v < 0 ? 'L' : 'R') + Math.round(Math.abs(v) * 100);
  switch (m.unit) {
    case 'dB': return v <= -89.9 ? '-∞' : (v > 0.04 && m.min < 0 ? '+' : '') + v.toFixed(1);
    case 'Hz': return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + 'k' : Math.round(v) + '';
    case 'ms': return v < 10 ? v.toFixed(1) : Math.round(v) + '';
    case 's': return v.toFixed(1) + 's';
    case '%': return Math.round(v) + '%';
    case ':1': return v >= 19.95 ? '∞:1' : v.toFixed(1) + ':1';
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
function key(text, cls, onClick) {
  const b = el('button', 'key ' + (cls || ''), text);
  if (onClick) b.addEventListener('click', onClick);
  return b;
}
// IN keys and other toggles. `block` makes it a processing-block key that Copy/Paste/Reset act on.
function tkey(path, text, cls = 'toggle', block = null) {
  const b = key(text, cls, () => {
    if (block && blockAction(block)) return;
    toggle(path);
  });
  watch(path, v => b.classList.toggle('on', v >= 0.5));
  return b;
}
function cycleKey(path) {
  const b = key('', '', () => { const m = S.meta[path]; setParam(path, get(path) >= m.max ? m.min : get(path) + 1); });
  watch(path, v => { b.textContent = fmt(path, v).toUpperCase(); });
  return b;
}
function choiceSelect(path) {
  const s = el('select', 'sel');
  s.id = 'sel-' + path.replace(/\//g, '-');
  (choiceLabels(path) || []).forEach((n, i) => { const o = el('option', '', n); o.value = i; s.append(o); });
  s.addEventListener('change', () => setParam(path, +s.value));
  watch(path, v => { s.value = v; });
  return s;
}

function dragPos(elm, getPos, apply, pixels = 200) {
  let startY = 0, startPos = 0, dragging = false;
  elm.addEventListener('pointerdown', e => {
    dragging = true; elm.setPointerCapture(e.pointerId);
    startY = e.clientY; startPos = getPos();
  });
  elm.addEventListener('pointermove', e => {
    if (!dragging) return;
    apply(startPos + (startY - e.clientY) / pixels * (e.shiftKey ? 0.2 : 1));
  });
  const end = () => { dragging = false; };
  elm.addEventListener('pointerup', end);
  elm.addEventListener('pointercancel', end);
}

function knob(path, label, opts = {}) {
  const k = el('div', 'knob');
  if (!has(path)) { k.classList.add('na'); }
  const NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', '0 0 40 40');
  const mk = (t, a) => { const e = document.createElementNS(NS, t); for (const n in a) e.setAttribute(n, a[n]); svg.append(e); return e; };
  const arcPath = (p0, p1) => {
    const a0 = (-135 + 270 * p0) * Math.PI / 180, a1 = (-135 + 270 * p1) * Math.PI / 180;
    const x = a => 20 + 15 * Math.sin(a), y = a => 20 - 15 * Math.cos(a);
    return `M${x(a0)} ${y(a0)} A15 15 0 ${a1 - a0 > Math.PI ? 1 : 0} 1 ${x(a1)} ${y(a1)}`;
  };
  mk('path', { class: 'track', d: arcPath(0, 1) });
  const arc = mk('path', { class: 'arc' });
  mk('circle', { class: 'cap', cx: 20, cy: 20, r: 10 });
  const ptr = mk('line', { class: 'ptr', x1: 20, y1: 20, x2: 20, y2: 11 });
  const val = el('div', 'kv');
  k.append(svg, val, el('div', 'kl', label));
  const m = S.meta[path];
  const bipolar = m && m.min < 0 && m.max > 0 && taper(path) !== 'fader';
  watch(path, v => {
    const p = Math.min(1, Math.max(0, toPos(path, v)));
    const c = bipolar ? toPos(path, 0) : 0;
    arc.setAttribute('d', p >= c ? arcPath(c, Math.max(p, c + 0.001)) : arcPath(p, c));
    ptr.setAttribute('transform', `rotate(${-135 + 270 * p} 20 20)`);
    val.textContent = fmt(path, v);
  });
  if (opts.enable) watch(opts.enable, v => k.classList.toggle('off', v < 0.5));
  if (opts.disableWhen) opts.disableWhen(k);
  dragPos(k, () => toPos(path, get(path)), p => setParam(path, fromPos(path, p)), 180);
  k.addEventListener('dblclick', () => m && setParam(path, m.def));
  k.addEventListener('wheel', e => {
    e.preventDefault();
    setParam(path, fromPos(path, toPos(path, get(path)) - Math.sign(e.deltaY) * 0.01));
  }, { passive: false });
  return k;
}

// Strip LEDs: Pk (about 3 dB below clip), 0 (0 dBu = -18 dBFS), Sig (-26 dBu = -44 dBFS),
// plus a gain-reduction bar.
function leds() {
  const m = el('div', 'leds');
  const pk = el('i', 'pk'), nom = el('i', 'nom'), sig = el('i', 'sig');
  pk.title = 'Pk'; nom.title = '0 dBu'; sig.title = 'Sig';
  const gr = el('div', 'grl');
  m.append(pk, nom, sig, gr);
  let hold = 0;
  m.set = (db, grDb = 0) => {
    sig.classList.toggle('on', db >= -44);
    nom.classList.toggle('on', db >= -18);
    hold = db >= -3 ? 12 : Math.max(0, hold - 1);  // hold Pk briefly so it is visible
    pk.classList.toggle('on', hold > 0);
    gr.classList.toggle('on', grDb >= 1);
  };
  return m;
}

// Fader. pathFn returns the current parameter; opts.linear for the GEQ (±12 dB, centre 0).
function fader(pathFn, readout, opts = {}) {
  const f = el('div', 'fader');
  f.append(el('div', 'track'));
  const toP = v => (opts.linear ? (v + 12) / 24 : faderPos(v));
  const fromP = p => (opts.linear ? Math.round((Math.min(1, Math.max(0, p)) * 24 - 12) * 2) / 2 : faderDb(p));
  const tickDbs = opts.linear ? [12, 6, 0, -6, -12] : [10, 0, -10, -20, -30, -40, -60];
  const ticks = tickDbs.map(db => { const t = el('div', 'tick' + (db === 0 ? ' u' : '')); t.db = db; f.append(t); return t; });
  const cap = el('div', 'cap');
  f.append(cap);
  const render = () => {
    const path = pathFn();
    const v = path ? get(path) ?? (opts.linear ? 0 : -90) : -90;
    const g = f.clientHeight - 20;
    cap.style.top = (10 + (1 - toP(v)) * g) + 'px';
    ticks.forEach(t => { t.style.top = (10 + (1 - toP(t.db)) * g) + 'px'; });
    if (readout) readout.textContent = path ? fmt(path, v) : '';
  };
  f.render = render;
  const set = v => { const p = pathFn(); if (p) { setParam(p, v); render(); } };
  let dragging = false, startY = 0, startPos = 0;
  f.addEventListener('pointerdown', e => {
    if (!pathFn()) return;
    dragging = true; f.setPointerCapture(e.pointerId);
    startY = e.clientY; startPos = toP(get(pathFn()) ?? -90);
    if (e.target !== cap) {
      const r = f.getBoundingClientRect();
      startPos = 1 - (e.clientY - r.top - 10) / (r.height - 20);
      set(fromP(startPos));
    }
  });
  f.addEventListener('pointermove', e => {
    if (!dragging) return;
    set(fromP(startPos - (e.clientY - startY) / (f.clientHeight - 20) * (e.shiftKey ? 0.25 : 1)));
  });
  const end = () => { dragging = false; };
  f.addEventListener('pointerup', end);
  f.addEventListener('pointercancel', end);
  f.addEventListener('dblclick', () => set(0));
  f.addEventListener('wheel', e => {
    e.preventDefault();
    const cur = get(pathFn()) ?? 0;
    set(opts.linear ? cur - Math.sign(e.deltaY) * 0.5 : Math.min(10, Math.max(-90, (cur <= -89.9 ? -60 : cur) - Math.sign(e.deltaY) * 0.5)));
  }, { passive: false });
  new ResizeObserver(render).observe(f);
  return f;
}

// ======================================================================= copy / paste / reset
// Processing blocks, as addressed by the SuperStrip IN keys.
const BLOCKS = {
  preamp: s => s === 'gain' || s === 'polarity',
  hpf: s => s.startsWith('hpf/'),
  gate: s => s.startsWith('gate/'),
  peq: s => s.startsWith('eq/'),
  geq: s => s.startsWith('geq/'),
  comp: s => s.startsWith('comp/'),
  delay: s => s.startsWith('delay/'),
  strip: s => !/^(pafl|mg\/|dca\/)/.test(s),
};
const kindOf = id => (inputById(id) || busById(id) || { kind: isDca(id) ? 'dca' : '?' }).kind;
function suffixes(id, block) {
  const pre = id + '/';
  return Object.keys(S.meta).filter(p => p.startsWith(pre) && BLOCKS[block](p.slice(pre.length))).map(p => p.slice(pre.length));
}
function doCopy(id, block) {
  const values = {};
  for (const s of suffixes(id, block)) values[s] = get(`${id}/${s}`);
  S.clip = { kind: kindOf(id), block, values, from: displayName(id) };
  toast(`Copied ${block === 'strip' ? 'strip' : block.toUpperCase()} from ${S.clip.from}`);
}
function doPaste(id, block) {
  if (!S.clip) { toast('Nothing copied yet'); return; }
  if (block === 'strip' && S.clip.block === 'strip' && S.clip.kind !== kindOf(id)) { toast('Paste needs the same kind of strip'); return; }
  if (block !== 'strip' && S.clip.block !== block) { toast(`Copy buffer holds ${S.clip.block.toUpperCase()}`); return; }
  let n = 0;
  for (const [s, v] of Object.entries(S.clip.values)) if (has(`${id}/${s}`)) { setParam(`${id}/${s}`, v); n++; }
  toast(`Pasted ${n} settings to ${displayName(id)}`);
}
function doReset(id, block) {
  for (const s of suffixes(id, block)) setParam(`${id}/${s}`, S.meta[`${id}/${s}`].def);
  toast(`Reset ${block === 'strip' ? 'strip' : block.toUpperCase()} on ${displayName(id)}`);
}
// Returns true if a latched Copy/Paste/Reset consumed this press.
function blockAction(block, id = S.sel) {
  if (S.mode !== 'copy' && S.mode !== 'paste' && S.mode !== 'reset') return false;
  ({ copy: doCopy, paste: doPaste, reset: doReset })[S.mode](id, block);
  setMode(null);
  return true;
}
function setMode(m) {
  S.mode = S.mode === m ? null : m;
  renderControls();
  renderBank();
  renderMaster();
}

// ================================================================================ fader bank
function layerStrips() {
  if (S.flip) {
    const n = S.info.geqBands;
    const range = S.layer === '2' ? [17, n] : [1, 16];
    const list = [];
    for (let k = range[0]; k <= range[1]; k++) list.push({ type: 'geq', band: k });
    return list;
  }
  if (S.layer === '1') return inputs().filter(i => i.kind === 'mono').map(i => ({ type: 'input', id: i.id }));
  if (S.layer === '2') {
    return [...inputs().filter(i => i.kind !== 'mono').map(i => ({ type: 'input', id: i.id })),
      ...buses().filter(b => b.kind === 'fx').map(b => ({ type: 'bus', id: b.id })),
      ...buses().filter(b => b.kind === 'mix').map(b => ({ type: 'bus', id: b.id }))];
  }
  const list = [];
  for (let n = 1; n <= 16; n++) {
    const id = customStripId(get(`custom/${n}/strip`));
    list.push(!id ? { type: 'blank' } : isDca(id) ? { type: 'dca', id } : inputById(id) ? { type: 'input', id } : { type: 'bus', id });
  }
  return list;
}

function stripFaderPath(s) {
  if (s.type === 'geq') return `${S.sel}/geq/${s.band}`;
  if (s.type !== 'input') return `${s.id}/fader`;
  const kind = mixKind(), n = idx(S.mix);
  if (kind === 'mix') return `${s.id}/mix/${n}/level`;
  if (kind === 'fx') return has(`${s.id}/fx/${n}/level`) ? `${s.id}/fx/${n}/level` : null;
  return `${s.id}/fader`;
}
// What the Sel key does in Assign / Pre Fade modes, for input strips.
function assignPath(id) {
  const kind = mixKind(), n = idx(S.mix);
  if (kind === 'lr') return `${id}/lr`;
  if (kind === 'mix') return `${id}/mix/${n}/on`;
  if (kind === 'fx') return has(`${id}/fx/${n}/on`) ? `${id}/fx/${n}/on` : null;
  return null;
}
function prePath(id) {
  const kind = mixKind(), n = idx(S.mix);
  if (kind === 'mix') return `${id}/mix/${n}/pre`;
  if (kind === 'fx') return has(`${id}/fx/${n}/pre`) ? `${id}/fx/${n}/pre` : null;
  return null;
}

// Indices into the 31-band RTA for each of the 28 GEQ bands (31.5 Hz ... 16 kHz).
const geqToRta = k => k + 1;  // GEQ band k (1-based) -> RTA index (0-based): 31.5 Hz is RTA[2]
const GEQ_LABELS = ['31', '40', '50', '63', '80', '100', '125', '160', '200', '250', '315', '400', '500', '630', '800', '1k',
  '1k2', '1k6', '2k', '2k5', '3k1', '4k', '5k', '6k3', '8k', '10k', '12k5', '16k'];
function rtaPeakIndex() {
  if (!S.rta) return -1;
  let best = 0;
  for (let k = 1; k < S.rta.length; k++) if (S.rta[k] > S.rta[best]) best = k;
  return S.rta[best] > -70 ? best : -1;
}

function buildStrip(s, opts = {}) {
  const st = el('div', 'strip');
  if (s.type === 'blank') { st.classList.add('blank'); return st; }
  const kind = mixKind();
  const isIn = s.type === 'input';
  const sendMode = isIn && (kind === 'mix' || kind === 'fx');
  st.classList.toggle('sendmode', sendMode);
  st.style.setProperty('--kc', s.type === 'geq' ? 'var(--pafl)' : sendMode ? `var(--c-${kind})` : colorOf(s.id));

  const name = el('div', 'name');
  if (s.type === 'geq') {
    name.textContent = GEQ_LABELS[s.band - 1];
    onFrame(() => name.classList.toggle('pk', rtaPeakIndex() === geqToRta(s.band)));
  } else {
    watchLabel(s.id, () => { name.textContent = displayName(s.id); });
    name.title = 'Double-tap to rename';
    name.addEventListener('dblclick', () => { S.screen = 'setup'; S.tab.setup = 'names'; S.focusName = s.id; renderScreen(); });
  }

  const path = opts.faderPath || stripFaderPath(s);
  if (!path) st.classList.add('disabled');

  // Mute: flashes when a mute-group master mutes the channel.
  let muteKey;
  if (s.type === 'geq') {
    muteKey = key('', ''); muteKey.style.visibility = 'hidden';
  } else {
    muteKey = tkey(`${s.id}/mute`, 'MUTE', 'mute');
    if (isIn) {
      const upd = () => {
        let mg = false;
        for (let m = 1; m <= S.info.muteGroups; m++) if (on(`${s.id}/mg/${m}`) && on(`mg/${m}/mute`)) mg = true;
        muteKey.classList.toggle('mg', mg && !on(`${s.id}/mute`));
      };
      for (let m = 1; m <= S.info.muteGroups; m++) { watch(`${s.id}/mg/${m}`, upd); watch(`mg/${m}/mute`, upd); }
      watch(`${s.id}/mute`, upd);
    }
  }

  // Sel: select, or assign / pre-fade / copy / paste / reset depending on the latched key.
  const selKey = key('SEL', 'sel');
  if (s.type === 'geq') {
    const p = path;
    selKey.addEventListener('click', () => setParam(p, 0));
    watch(p, v => selKey.classList.toggle('on', Math.abs(v) < 0.01));  // lit at flat, as on the Qu
  } else if (isIn && S.mode === 'assign' && !opts.master) {
    const p = assignPath(s.id);
    if (p) { selKey.addEventListener('click', () => toggle(p)); watch(p, v => selKey.classList.toggle('asg', v >= 0.5)); }
    else selKey.disabled = true;
  } else if (isIn && S.mode === 'prefade' && !opts.master) {
    const p = prePath(s.id);
    if (p) { selKey.addEventListener('click', () => toggle(p)); watch(p, v => selKey.classList.toggle('pre', v >= 0.5)); }
    else selKey.disabled = true;
  } else if (S.mode === 'copy' || S.mode === 'paste' || S.mode === 'reset') {
    selKey.textContent = S.mode.toUpperCase();
    selKey.addEventListener('click', () => blockAction('strip', s.id));
  } else if (s.type === 'dca') {
    selKey.style.visibility = 'hidden';
  } else {
    selKey.addEventListener('click', () => select(s.id));
    selKey.classList.toggle('on', S.sel === s.id);
    st.classList.toggle('selected', S.sel === s.id);
  }

  const paflKey = s.type === 'geq' || s.type === 'dca' ? key('', '') : tkey(`${s.id}/pafl`, 'PAFL', 'pafl');
  if (s.type === 'geq' || s.type === 'dca') paflKey.style.visibility = 'hidden';

  const body = el('div', 'body');
  const val = el('div', 'val');
  const fd = fader(() => path, val, { linear: s.type === 'geq' });
  if (path) watch(path, () => fd.render());
  if (s.type === 'geq') {
    const bar = el('div', 'bar-mtr');
    const fill = el('i');
    bar.append(fill);
    onFrame(() => {
      const r = S.rta ? S.rta[geqToRta(s.band)] : -90;
      fill.style.height = Math.min(100, Math.max(0, (r + 60) / 60 * 100)) + '%';
      bar.classList.toggle('pk', rtaPeakIndex() === geqToRta(s.band));
    });
    body.append(bar, fd);
  } else if (s.type === 'dca') {
    body.append(fd);
  } else {
    const m = leds();
    onFrame(() => {
      if (isIn) { const x = inMeter(s.id); m.set(x.in, x.gr); }
      else { const x = busMeter(s.id); m.set(Math.max(x.l, x.r), x.gr); }
    });
    body.append(m, fd);
  }
  st.append(name, muteKey, selKey, paflKey, body, val);
  return st;
}

function renderBank() {
  if (!S.ready) return;
  withTag('bank', () => {
    const root = document.getElementById('strips');
    root.innerHTML = '';
    const list = layerStrips();
    for (const s of list) root.append(buildStrip(s));
    for (let i = list.length; i < 16; i++) root.append(el('div', 'strip blank'));
  });
  withTag('layers', () => {
    const root = document.getElementById('layers');
    root.innerHTML = '';
    for (const [id, big, small] of [['1', '1', 'CH 1-16'], ['2', '2', 'ST · FX MIX MSTR'], ['C', 'C', 'CUSTOM']]) {
      const b = key('', S.layer === id ? 'on' : '', () => { S.layer = id; renderBank(); });
      b.append(el('div', 'lk', big), el('div', 'ls', S.flip && id !== 'C' ? (id === '1' ? 'GEQ 1-16' : 'GEQ 17-28') : small));
      root.append(b);
    }
    if (S.flip) root.append(key('EXIT FLIP', 'flip', () => setFlip(false)));
  });
}

function renderMaster() {
  if (!S.ready) return;
  withTag('master', () => {
    const root = document.getElementById('master');
    root.innerHTML = '';
    root.append(buildStrip({ type: 'bus', id: S.mix }, { master: true }));
  });
}

function setFlip(onOff) {
  const bus = busById(S.sel);
  if (onOff && !(bus && has(`${S.sel}/geq/1`))) { toast('Select LR or a mix master to flip its GEQ'); return; }
  S.flip = onOff;
  if (onOff) S.layer = '1';
  renderBank();
  renderSuperStrip();
}

// ============================================================================ right panel
function renderControls() {
  if (!S.ready) return;
  withTag('controls', () => {
    const root = document.getElementById('controls');
    root.innerHTML = '';

    // Copy / Paste / Reset / Fn
    const cpr = el('div', 'grp c4');
    for (const [m, text] of [['copy', 'COPY'], ['paste', 'PASTE'], ['reset', 'RESET']])
      cpr.append(key(text, 'mode' + (S.mode === m ? ' on' : ''), () => setMode(m)));
    cpr.append(key('Fn', 'mode' + (S.fn ? ' on' : ''), () => { S.fn = !S.fn; renderControls(); renderScreen(); }));
    root.append(cpr);

    // Mix keys, then Assign / Pre Fade
    const mixes = el('div', 'grp c3');
    mixes.append(el('div', 'cap', 'MIX'));
    const order = [...buses().filter(b => b.kind === 'mix'), ...buses().filter(b => b.kind === 'fx'), busById('lr')];
    for (const b of order) {
      const k = key('', 'mixkey' + (S.mix === b.id ? ' on' : ''), () => setMix(b.id));
      k.style.setProperty('--kc', `var(--c-${b.kind})`);
      watchLabel(b.id, () => { k.textContent = (S.labels[b.id] || b.name).toUpperCase(); });
      mixes.append(k);
    }
    root.append(mixes);
    const asg = el('div', 'grp c3');
    asg.append(key('ASSIGN', 'mode' + (S.mode === 'assign' ? ' on' : ''), () => setMode('assign')));
    const pf = key('PRE FADE', 'mode' + (S.mode === 'prefade' ? ' on' : ''), () => setMode('prefade'));
    pf.disabled = mixKind() === 'lr';
    asg.append(pf);
    asg.append(key('GEQ FLIP', 'mode' + (S.flip ? ' on' : ''), () => setFlip(!S.flip)));
    root.append(asg);

    // SoftKeys
    const soft = el('div', 'grp c4 soft');
    soft.append(el('div', 'cap', 'SOFTKEYS'));
    for (let n = 1; n <= 4; n++) soft.append(softKey(n));
    root.append(soft);

    // Engineer's tools: talkback, monitor (PAFL) level, recorder transport
    const tools = el('div', 'tools');
    tools.append(tkey('talk/on', 'TALK', 'toggle'), knob('mon/level', 'PAFL LVL'));
    const drive = el('div', 'drive');
    const dot = el('span', 'rec-dot');
    const time = el('span', 'kv', '0:00');
    const rec = key('REC', '', () => send(`rec ${S.recMode}`));
    const stop = key('STOP', '', () => send('rec stop'));
    drive.append(dot, time, rec, stop);
    onFrame(() => {
      const r = S.rec;
      const recording = !!(r && r.state === 'rec');
      dot.classList.toggle('on', recording);
      time.textContent = r ? clock(r.seconds) : '0:00';
      rec.disabled = recording;
    });
    tools.append(drive);
    root.append(tools);
  });
}
const clock = s => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

const tapTimes = {};
function softKey(n) {
  const path = `soft/${n}/func`;
  const k = key('', '');
  let bound = null;
  const bind = f => {
    bound = f === 0 ? null : f <= 4 ? `mg/${f}/mute` : f <= 8 ? `dca/${f - 4}/mute` : f === 9 ? 'talk/on' : null;
    k.textContent = SOFTKEY_FUNCS[f].toUpperCase();
    k.className = 'key ' + (f >= 1 && f <= 8 ? 'mute' : f === 9 ? 'toggle' : '');
    if (bound) k.classList.toggle('on', on(bound));
  };
  watch(path, v => bind(v));
  for (let m = 1; m <= 4; m++) { watch(`mg/${m}/mute`, () => bind(get(path))); watch(`dca/${m}/mute`, () => bind(get(path))); }
  watch('talk/on', () => bind(get(path)));
  k.addEventListener('click', () => {
    const f = get(path);
    if (f >= 1 && f <= 9) { toggle(bound); return; }
    if (f >= 10 && f <= 13) {  // tap tempo: sets the FX delay time from the last two taps
      const u = f - 9, now = performance.now(), last = tapTimes[u];
      tapTimes[u] = now;
      if (last && now - last < 2000) {
        setParam(`fx/${u}/dly/time`, now - last);
        toast(`FX${u} delay ${Math.round(now - last)} ms`);
      }
      return;
    }
    if (f === 14 || f === 15) stepScene(f === 14 ? 1 : -1);
  });
  return k;
}

function setMix(id) {
  S.mix = id;
  if (S.mode === 'prefade' && mixKind() === 'lr') S.mode = null;
  renderControls();
  renderBank();
  renderMaster();
}

function select(id) {
  S.sel = id;
  if (S.flip && !has(`${id}/geq/1`)) S.flip = false;
  renderBank();
  renderMaster();
  renderSuperStrip();
  renderToolbar();
  renderScreen();
}

// ================================================================================ SuperStrip
function renderSuperStrip() {
  if (!S.ready) return;
  withTag('super', () => {
    const root = document.getElementById('superstrip');
    root.innerHTML = '';
    const id = S.sel;
    const p = s => `${id}/${s}`;
    const inp = inputById(id), bus = busById(id);
    const head = el('div', 'ss-head');
    const h = el('h2');
    watchLabel(id, () => { h.textContent = displayName(id); });
    head.append(h, el('span', 'tag', inp ? `${inp.kind === 'mono' ? 'mono' : inp.kind === 'stereo' ? 'stereo' : 'FX return'} input` : bus ? `${bus.name} master` : ''));
    root.append(head);
    const row = el('div', 'ss-row');
    const sec = (title, inPath, block, knobs, extra) => {
      const s = el('div', 'ss-sec');
      const t = el('h3', '', title);
      if (inPath && has(p(inPath))) t.append(tkey(p(inPath), 'IN', 'toggle', block));
      s.append(t);
      const k = el('div', 'ss-knobs');
      for (const [path, label, opt] of knobs) k.append(knob(p(path), label, { enable: inPath && has(p(inPath)) ? p(inPath) : null, ...(opt || {}) }));
      s.append(k);
      if (extra) s.append(extra);
      row.append(s);
    };
    if (has(p('gain'))) {
      const keys = el('div', 'ss-keys');
      const v48 = key('48V', ''); v48.disabled = true; v48.title = 'Phantom power is switched on the interface';
      const pad = key('PAD', ''); pad.disabled = true; pad.title = 'Pad is on the interface';
      keys.append(v48, pad, tkey(p('polarity'), 'Ø', 'toggle', 'preamp'));
      sec(inp && inp.stereo ? 'TRIM' : 'PREAMP', null, null, [['gain', inp && inp.stereo ? 'TRIM' : 'GAIN']], keys);
    }
    if (has(p('hpf/freq'))) sec('HPF', 'hpf/on', 'hpf', [['hpf/freq', 'FREQ']]);
    if (has(p('gate/thresh'))) sec('GATE', 'gate/on', 'gate', [['gate/thresh', 'THRESH']]);
    if (has(p('eq/1/freq'))) {
      ['LF', 'LM', 'HM', 'HF'].forEach((n, i) => {
        const b = i + 1;
        const shelfable = b === 1 || b === 4;
        const disableWidth = k => { if (shelfable) watch(p(`eq/${b}/type`), v => k.classList.toggle('na', v < 0.5)); };
        sec(n, b === 1 ? 'eq/on' : null, 'peq', [[`eq/${b}/gain`, 'GAIN'], [`eq/${b}/freq`, 'FREQ'], [`eq/${b}/width`, 'WIDTH', { disableWhen: disableWidth }]]);
      });
    }
    if (has(p('comp/thresh'))) sec('COMP', 'comp/on', 'comp', [['comp/thresh', 'THRESH'], ['comp/ratio', 'RATIO'], ['comp/makeup', 'GAIN']]);
    if (has(p('geq/1'))) {
      const keys = el('div', 'ss-keys');
      keys.append(key('FADER FLIP', 'mode' + (S.flip ? ' on' : ''), () => setFlip(!S.flip)));
      sec('GEQ', 'geq/on', 'geq', [], keys);
    }
    const panPath = has(p('pan')) ? 'pan' : has(p('balance')) ? 'balance' : null;
    if (panPath) sec(panPath === 'pan' && !(inp && inp.stereo) ? 'PAN' : 'BAL', null, null, [[panPath, panPath === 'pan' && !(inp && inp.stereo) ? 'PAN' : 'BAL']]);
    root.append(row);
  });
}

// ============================================================================= touch screen
const SCREENS = [['home', 'HOME'], ['proc', 'PROCESS'], ['route', 'ROUTING'], ['fx', 'FX'], ['scenes', 'SCENES'], ['setup', 'SETUP']];

function renderScreenKeys() {
  const root = document.getElementById('screenKeys');
  root.innerHTML = '';
  const brand = el('div', 'brand');
  brand.append('QNU', Object.assign(el('span'), { textContent: '16' }));
  root.append(brand);
  for (const [id, text] of SCREENS)
    root.append(key(text, S.screen === id ? 'on' : '', () => { S.screen = id; renderScreenKeys(); renderScreen(); }));
}

function renderToolbar() {
  const chip = document.getElementById('selChip');
  chip.textContent = displayName(S.sel);
  chip.style.setProperty('--kc', colorOf(S.sel));
  const info = document.getElementById('sceneInfo');
  const i = S.scenes.indexOf(S.scene);
  const next = S.scenes[i + 1] || (i < 0 ? S.scenes[0] : '');
  info.innerHTML = '';
  info.append('Scene ', Object.assign(el('b'), { textContent: S.scene || '—' }), '  ·  Next ', Object.assign(el('b'), { textContent: next || '—' }));
}

function tabs(screen, list) {
  const root = document.getElementById('tabs');
  root.innerHTML = '';
  if (!list.length) return null;
  if (!list.find(t => t[0] === S.tab[screen])) S.tab[screen] = list[0][0];
  for (const [id, text] of list) root.append(key(text, S.tab[screen] === id ? 'on' : '', () => { S.tab[screen] = id; renderScreen(); }));
  return S.tab[screen];
}

function card(title, extra) {
  const c = el('div', 'card');
  const h = el('h4', '', title);
  if (extra) h.append(extra);
  c.append(h);
  return c;
}

function renderScreen() {
  if (!S.ready) return;
  renderScreenKeys();
  withTag('screen', () => {
    const page = document.getElementById('page');
    page.innerHTML = '';
    document.getElementById('tabs').innerHTML = '';
    ({ home: screenHome, proc: screenProcessing, route: screenRouting, fx: screenFx, scenes: screenScenes, setup: screenSetup })[S.screen](page);
  });
}

// ------------------------------------------------------------------------------------ Home
function screenHome(page) {
  const tab = tabs('home', [['meters', 'METERS'], ['rta', 'RTA'], ['drive', 'DRIVE']]);
  if (tab === 'meters') {
    const meterCard = (title, items) => {
      const c = card(title);
      const g = el('div', 'mtr-grid');
      for (const [id, read] of items) {
        const cell = el('div', 'mtr-cell');
        const bar = el('div', 'bar');
        const fill = el('i');
        bar.append(fill);
        const gr = el('div', 'gr');
        const nm = el('span');
        watchLabel(id, () => { nm.textContent = displayName(id); });
        cell.append(bar, gr, nm);
        cell.addEventListener('click', () => select(id));
        g.append(cell);
        let level = 0;
        onFrame(() => {
          const [db, grDb] = read();
          level = Math.max(Math.min(1, Math.max(0, (db + 60) / 60)), level - 0.03);
          fill.style.height = (level * 100) + '%';
          gr.classList.toggle('on', grDb >= 1);
        });
      }
      c.append(g);
      page.append(c);
    };
    meterCard('INPUTS', inputs().map(i => [i.id, () => { const m = inMeter(i.id); return [m.in, m.gr]; }]));
    meterCard('MIXES · FX SENDS', buses().map(b => [b.id, () => { const m = busMeter(b.id); return [Math.max(m.l, m.r), m.gr]; }]));
  } else if (tab === 'rta') {
    const c = card('RTA · 31 BANDS, 1/3 OCTAVE');
    const src = el('span');
    c.querySelector('h4').append(src);
    const cv = canvas('graph tall');
    c.append(cv, el('div', 'note', 'The RTA follows PAFL: press a PAFL key to analyse that channel or mix. With no PAFL selected it shows LR. The peak band is shown in red.'));
    page.append(c);
    const labels = ['20', '25', '31', '40', '50', '63', '80', '100', '125', '160', '200', '250', '315', '400', '500', '630', '800', '1k', '1k25', '1k6', '2k', '2k5', '3k15', '4k', '5k', '6k3', '8k', '10k', '12k5', '16k', '20k'];
    const hold = new Array(31).fill(-90);
    cv.draw = () => {
      const r = prepCanvas(cv);
      if (!r || !S.rta) return;
      const { g, W, H } = r;
      const top = 8, bottom = H - 16, yOf = db => top + (Math.min(0, Math.max(-72, db)) / -72) * (bottom - top);
      g.font = '9px system-ui';
      for (let db = 0; db >= -72; db -= 12) {
        g.strokeStyle = '#1c2128'; g.beginPath(); g.moveTo(26, yOf(db)); g.lineTo(W, yOf(db)); g.stroke();
        g.fillStyle = '#5c626d'; g.fillText(String(db), 2, yOf(db) + 3);
      }
      const n = S.rta.length, bw = (W - 30) / n, pk = rtaPeakIndex();
      for (let k = 0; k < n; k++) {
        hold[k] = Math.max(S.rta[k], hold[k] - 0.4);
        const x = 28 + k * bw;
        g.fillStyle = k === pk ? '#ff3b3b' : '#2bd46f';
        g.fillRect(x, yOf(S.rta[k]), bw - 2, bottom - yOf(S.rta[k]));
        g.fillStyle = '#e8eaed';
        g.fillRect(x, yOf(hold[k]), bw - 2, 1.5);
        if (k % 2 === 0 || bw > 22) { g.fillStyle = k === pk ? '#ff6b6b' : '#8c939e'; g.fillText(labels[k], x, H - 4); }
      }
      if (pk >= 0) { g.fillStyle = '#ff6b6b'; g.font = 'bold 11px system-ui'; g.fillText(`Peak ${labels[pk]} Hz`, W - 96, 18); }
    };
    onFrame(() => {
      const selected = [...inputs(), ...buses()].filter(x => on(`${x.id}/pafl`)).map(x => displayName(x.id));
      src.textContent = selected.length ? `PAFL: ${selected.join(', ')}` : 'LR';
      cv.draw();
    });
  } else {
    const c = card('RECORDER · USB DRIVE');
    const mode = el('div', 'row');
    for (const [m, text] of [['multi', 'MULTITRACK 18'], ['stereo', 'STEREO LR']])
      mode.append(key(text, 'mode' + (S.recMode === m ? ' on' : ''), () => { S.recMode = m; renderScreen(); }));
    const t = el('div', 'big', '0:00');
    const status = el('div', 'note');
    const ctl = el('div', 'row');
    const dot = el('span', 'rec-dot');
    ctl.append(dot, key('● REC', 'mute', () => send(`rec ${S.recMode}`)), key('■ STOP', '', () => send('rec stop')));
    c.append(mode, t, ctl, status,
      el('div', 'note', 'Multitrack records Ch 1-16 (post-preamp direct outs) plus LR as one 18-channel WAV. Stereo records LR. Files go to the engine\'s recordings folder.'));
    page.append(c);
    onFrame(() => {
      const r = S.rec;
      const recording = !!(r && r.state === 'rec');
      dot.classList.toggle('on', recording);
      t.textContent = r ? clock(r.seconds) : '0:00';
      status.textContent = !r || r.state === 'none' ? 'Recorder unavailable' :
        recording ? `Recording ${r.mode === 'multi' ? '18 tracks' : 'stereo'} → ${r.file}${r.dropouts ? ` · ${r.dropouts} dropouts` : ''}` :
        r.file ? `Stopped. Last file: ${r.file}` : 'Ready';
    });
  }
}

// ------------------------------------------------------------------------------ Processing
function canvas(cls = 'graph') {
  const c = el('canvas', cls);
  c.draw = () => {};
  new ResizeObserver(() => c.draw()).observe(c);
  return c;
}
function prepCanvas(c) {
  const dpr = window.devicePixelRatio || 1, W = c.clientWidth, H = c.clientHeight;
  if (!W || !H) return null;
  if (c.width !== Math.round(W * dpr) || c.height !== Math.round(H * dpr)) { c.width = W * dpr; c.height = H * dpr; }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  return { g, W, H };
}

function screenProcessing(page) {
  const id = S.sel;
  const p = s => `${id}/${s}`;
  const list = [];
  if (has(p('gain'))) list.push(['preamp', 'PREAMP']);
  if (has(p('hpf/freq'))) list.push(['hpf', 'HPF']);
  if (has(p('gate/thresh'))) list.push(['gate', 'GATE']);
  if (has(p('eq/1/freq'))) list.push(['peq', 'PEQ']);
  if (has(p('geq/1'))) list.push(['geq', 'GEQ']);
  if (has(p('comp/thresh'))) list.push(['comp', 'COMP']);
  if (has(p('delay/time'))) list.push(['delay', 'DELAY']);
  const tab = tabs('proc', list);
  if (!tab) { page.append(el('div', 'note', 'No processing on this strip.')); return; }
  if (S.fn) { page.append(libraryCard(id, tab)); return; }
  const knobs = (c, items, enable) => {
    const r = el('div', 'row');
    for (const [s, l] of items) if (has(p(s))) r.append(knob(p(s), l, { enable: enable ? p(enable) : null }));
    c.append(r);
    return r;
  };
  if (tab === 'preamp') {
    const inp = inputById(id);
    const c = card(inp && inp.stereo ? 'TRIM' : 'PREAMP');
    const r = knobs(c, [['gain', inp && inp.stereo ? 'TRIM' : 'GAIN']]);
    const v48 = key('48V', ''); v48.disabled = true;
    const pad = key('PAD', ''); pad.disabled = true;
    r.append(v48, pad, tkey(p('polarity'), 'POLARITY Ø', 'toggle', 'preamp'));
    c.append(el('div', 'note', 'Analogue gain, pad and 48V phantom power are set on the audio interface. Gain here is a digital trim (±24 dB).'));
    page.append(c);
  } else if (tab === 'hpf') {
    const c = card('HIGH PASS FILTER · 12 dB/oct', tkey(p('hpf/on'), 'IN', 'toggle', 'hpf'));
    c.append(eqGraph(id, { hpfOnly: true }));
    knobs(c, [['hpf/freq', 'FREQ']], 'hpf/on');
    page.append(c);
  } else if (tab === 'gate') {
    const c = card('GATE', tkey(p('gate/on'), 'IN', 'toggle', 'gate'));
    const row = el('div', 'row');
    row.append(transferGraph(id, 'gate'));
    const k = el('div', 'row grow');
    for (const [s, l] of [['gate/thresh', 'THRESH'], ['gate/range', 'DEPTH'], ['gate/attack', 'ATTACK'], ['gate/hold', 'HOLD'], ['gate/release', 'RELEASE']])
      k.append(knob(p(s), l, { enable: p('gate/on') }));
    row.append(k);
    c.append(row, gateStatus(id));
    page.append(c);
  } else if (tab === 'peq') {
    const c = card('PARAMETRIC EQ', tkey(p('eq/on'), 'IN', 'toggle', 'peq'));
    c.append(eqGraph(id));
    const row = el('div', 'row');
    ['LF', 'LM', 'HM', 'HF'].forEach((n, i) => {
      const b = i + 1;
      const bc = card(n, b === 1 || b === 4 ? cycleKey(p(`eq/${b}/type`)) : null);
      bc.style.flex = '1';
      const r = el('div', 'row');
      const width = knob(p(`eq/${b}/width`), 'WIDTH', { enable: p('eq/on') });
      if (b === 1 || b === 4) watch(p(`eq/${b}/type`), v => width.classList.toggle('na', v < 0.5));
      r.append(knob(p(`eq/${b}/gain`), 'GAIN', { enable: p('eq/on') }), knob(p(`eq/${b}/freq`), 'FREQ', { enable: p('eq/on') }), width);
      bc.append(r);
      row.append(bc);
    });
    c.append(row);
    page.append(c);
  } else if (tab === 'geq') {
    const c = card('GRAPHIC EQ · 28 BANDS ±12 dB', tkey(p('geq/on'), 'IN', 'toggle', 'geq'));
    const g = el('div', 'geq');
    for (let k = 1; k <= S.info.geqBands; k++) {
      const band = el('div', 'band');
      band.append(vbar(p(`geq/${k}`), geqToRta(k)), el('span', '', GEQ_LABELS[k - 1]));
      g.append(band);
    }
    c.append(g);
    const r = el('div', 'row');
    r.append(key('FADER FLIP', 'mode' + (S.flip ? ' on' : ''), () => setFlip(!S.flip)),
      key('FLAT', '', () => { for (let k = 1; k <= S.info.geqBands; k++) setParam(p(`geq/${k}`), 0); }),
      el('span', 'note', 'Background bars: RTA of the PAFL signal. PAFL this master to see its spectrum.'));
    c.append(r);
    page.append(c);
  } else if (tab === 'comp') {
    const c = card('COMPRESSOR', tkey(p('comp/on'), 'IN', 'toggle', 'comp'));
    const typeRow = el('div', 'row');
    typeRow.append(el('span', 'note', 'TYPE'), choiceSelect(p('comp/mode')));
    const row = el('div', 'row');
    row.append(transferGraph(id, 'comp'));
    const k = el('div', 'row grow');
    const auto = kn => watch(p('comp/mode'), v => kn.classList.toggle('na', v >= 2));
    for (const [s, l, a] of [['comp/thresh', 'THRESH'], ['comp/ratio', 'RATIO'], ['comp/attack', 'ATTACK', 1], ['comp/release', 'RELEASE', 1], ['comp/knee', 'KNEE', 1], ['comp/makeup', 'GAIN']])
      k.append(knob(p(s), l, { enable: p('comp/on'), disableWhen: a ? auto : null }));
    row.append(k);
    const gr = el('div', 'row');
    const grText = el('span', 'kv', '0.0 dB');
    const bar = el('div', 'bar-mtr');
    bar.style.cssText = 'width:140px;height:8px;transform:scaleX(-1)';
    const fill = el('i');
    fill.style.cssText = 'top:0;height:100%;width:0;background:var(--gr)';
    bar.append(fill);
    gr.append(el('span', 'note', 'Gain reduction'), bar, grText);
    let hold = 0;
    onFrame(() => {
      const m = inputById(id) ? inMeter(id) : busMeter(id);
      hold = Math.max(m.gr, hold * 0.95);
      fill.style.width = Math.min(100, hold / 24 * 100) + '%';
      grText.textContent = (hold < 0.05 ? '0.0' : '-' + hold.toFixed(1)) + ' dB';
    });
    c.append(typeRow, row, gr, el('div', 'note', 'Slow Opto and Punchbag set their own attack, release and knee.'));
    page.append(c);
  } else if (tab === 'delay') {
    const c = card('DELAY', tkey(p('delay/on'), 'IN', 'toggle', 'delay'));
    knobs(c, [['delay/time', 'TIME ms']], 'delay/on');
    const info = el('div', 'note');
    watch(p('delay/time'), v => { info.textContent = `${(v * 0.343).toFixed(2)} m  ·  ${(v * 1.125).toFixed(2)} ft at 343 m/s`; });
    c.append(info);
    page.append(c);
  }
}

function gateStatus(id) {
  const r = el('div', 'row');
  const dot = el('span', 'rec-dot');
  const t = el('span', 'note', '');
  r.append(dot, t);
  onFrame(() => {
    const m = inMeter(id);
    if (!m) return;
    const open = !!m.gate;
    dot.style.background = on(`${id}/gate/on`) ? (open ? 'var(--ok)' : 'var(--mute)') : '#444';
    t.textContent = !on(`${id}/gate/on`) ? 'Gate out' : open ? 'Open' : 'Closed';
  });
  return r;
}

// Factory processing libraries (Fn on the Processing screen).
const LIBRARY = {
  peq: {
    'Flat': {},
    'Kick': { 'eq/1/type': 0, 'eq/1/freq': 70, 'eq/1/gain': 4, 'eq/2/freq': 350, 'eq/2/gain': -6, 'eq/2/width': 1, 'eq/3/freq': 4000, 'eq/3/gain': 4, 'eq/3/width': 1, 'eq/4/gain': 0 },
    'Snare': { 'eq/1/freq': 100, 'eq/1/gain': -3, 'eq/2/freq': 200, 'eq/2/gain': 3, 'eq/2/width': 1, 'eq/3/freq': 900, 'eq/3/gain': -3, 'eq/3/width': 0.5, 'eq/4/freq': 6000, 'eq/4/gain': 3 },
    'Vocal': { 'eq/1/freq': 120, 'eq/1/gain': -2, 'eq/2/freq': 300, 'eq/2/gain': -3, 'eq/2/width': 1, 'eq/3/freq': 3000, 'eq/3/gain': 3, 'eq/3/width': 1, 'eq/4/freq': 10000, 'eq/4/gain': 2 },
    'Acoustic Gtr': { 'eq/1/freq': 100, 'eq/1/gain': -4, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/2/width': 1, 'eq/3/freq': 3500, 'eq/3/gain': 2, 'eq/3/width': 1, 'eq/4/freq': 9000, 'eq/4/gain': 3 },
    'Bass DI': { 'eq/1/freq': 80, 'eq/1/gain': 3, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/2/width': 1, 'eq/3/freq': 1500, 'eq/3/gain': 2, 'eq/3/width': 1, 'eq/4/freq': 5000, 'eq/4/gain': -3 },
  },
  comp: {
    'Vocal': { 'comp/mode': 2, 'comp/thresh': -18, 'comp/ratio': 3, 'comp/makeup': 3 },
    'Kick / Bass': { 'comp/mode': 3, 'comp/thresh': -15, 'comp/ratio': 4, 'comp/makeup': 2 },
    'Peak limiter': { 'comp/mode': 0, 'comp/thresh': -6, 'comp/ratio': 20, 'comp/attack': 0.5, 'comp/release': 50, 'comp/knee': 0, 'comp/makeup': 0 },
    'Gentle RMS': { 'comp/mode': 1, 'comp/thresh': -20, 'comp/ratio': 2, 'comp/attack': 20, 'comp/release': 200, 'comp/knee': 6, 'comp/makeup': 2 },
  },
  gate: {
    'Kick': { 'gate/thresh': -30, 'gate/range': 40, 'gate/attack': 0.1, 'gate/hold': 40, 'gate/release': 120 },
    'Tom': { 'gate/thresh': -36, 'gate/range': 30, 'gate/attack': 0.5, 'gate/hold': 80, 'gate/release': 300 },
    'Vocal (soft)': { 'gate/thresh': -50, 'gate/range': 12, 'gate/attack': 2, 'gate/hold': 100, 'gate/release': 400 },
  },
};
function libraryCard(id, tab) {
  const block = tab === 'peq' ? 'peq' : tab === 'comp' ? 'comp' : tab === 'gate' ? 'gate' : null;
  const c = card(`LIBRARY · ${tab.toUpperCase()}`, key('CLOSE', '', () => { S.fn = false; renderControls(); renderScreen(); }));
  if (!block) { c.append(el('div', 'note', 'No factory library for this block.')); return c; }
  const list = el('div', 'list');
  for (const [name, values] of Object.entries(LIBRARY[block])) {
    list.append(el('div', 'nm', name), el('span'), el('span'), key('RECALL', '', () => {
      doReset(id, block);
      for (const [s, v] of Object.entries(values)) if (has(`${id}/${s}`)) setParam(`${id}/${s}`, v);
      if (has(`${id}/${block === 'peq' ? 'eq' : block}/on`)) setParam(`${id}/${block === 'peq' ? 'eq' : block}/on`, 1);
      toast(`Recalled ${name} on ${displayName(id)}`);
    }));
  }
  c.append(list, el('div', 'note', 'Press Fn again to return to the processing controls.'));
  return c;
}

// Vertical GEQ slider with the RTA band behind it.
function vbar(path, rtaIndex) {
  const v = el('div', 'vbar');
  const rta = el('div', 'rta');
  const fill = el('i');
  v.append(rta, el('b'), fill);
  const m = S.meta[path];
  watch(path, val => {
    const pos = (val - m.min) / (m.max - m.min);
    fill.style.bottom = (Math.min(pos, 0.5) * 100) + '%';
    fill.style.height = (Math.abs(pos - 0.5) * 100 + 0.6) + '%';
    v.title = fmt(path, val) + ' dB';
  });
  onFrame(() => {
    const db = S.rta ? S.rta[rtaIndex] : -90;
    rta.style.height = Math.min(100, Math.max(0, (db + 60) / 60 * 100)) + '%';
    rta.classList.toggle('pk', rtaPeakIndex() === rtaIndex);
  });
  dragPos(v, () => toPos(path, get(path)), pos => setParam(path, Math.round(fromPos(path, pos) * 2) / 2), 140);
  v.addEventListener('dblclick', () => setParam(path, 0));
  return v;
}

// RBJ biquad magnitude, identical to engine/src/dsp/biquad.h.
function biquadDb(type, f0, gainDb, q, f, fs) {
  const A = Math.pow(10, gainDb / 40), w0 = 2 * Math.PI * Math.min(f0, 0.45 * fs) / fs;
  const cs = Math.cos(w0), sn = Math.sin(w0), al = sn / (2 * q), s2 = 2 * Math.sqrt(A) * al;
  let b0, b1, b2, a0, a1, a2;
  if (type === 'peak') { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
  else if (type === 'lowshelf') { b0 = A * ((A + 1) - (A - 1) * cs + s2); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - s2); a0 = (A + 1) + (A - 1) * cs + s2; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - s2; }
  else if (type === 'highshelf') { b0 = A * ((A + 1) + (A - 1) * cs + s2); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - s2); a0 = (A + 1) - (A - 1) * cs + s2; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - s2; }
  else { b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
  const w = 2 * Math.PI * f / fs, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2w = Math.sin(2 * w);
  const nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2w);
  const dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2w);
  return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}
const widthToQ = oct => { const p = Math.pow(2, oct); return Math.sqrt(p) / (p - 1); };

// PEQ response with draggable band handles (drag: freq/gain, wheel: width).
function eqGraph(id, opts = {}) {
  const c = canvas();
  const p = s => `${id}/${s}`;
  const fs = () => S.info.sampleRate;
  const xOf = (f, W) => Math.log(f / 20) / Math.log(1000) * W;
  const fOf = (x, W) => 20 * Math.pow(1000, x / W);
  const yOf = (db, H) => H / 2 - db / 18 * (H / 2 - 8);
  const dbOf = (y, H) => (H / 2 - y) / (H / 2 - 8) * 18;
  const bandType = b => (b === 1 ? (get(p('eq/1/type')) >= 0.5 ? 'peak' : 'lowshelf') : b === 4 ? (get(p('eq/4/type')) >= 0.5 ? 'peak' : 'highshelf') : 'peak');
  const bandQ = b => (bandType(b) === 'peak' ? widthToQ(get(p(`eq/${b}/width`))) : 0.7071);
  const colors = ['#ff6b6b', '#ffc12e', '#38d97a', '#4aa8ff'];
  c.draw = () => {
    const r = prepCanvas(c);
    if (!r) return;
    const { g, W, H } = r;
    g.strokeStyle = '#1c2128'; g.lineWidth = 1; g.fillStyle = '#5c626d'; g.font = '9px system-ui';
    for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
      g.beginPath(); g.moveTo(xOf(f, W), 0); g.lineTo(xOf(f, W), H); g.stroke();
      g.fillText(f >= 1000 ? f / 1000 + 'k' : f, xOf(f, W) + 2, H - 3);
    }
    for (const db of [-12, -6, 0, 6, 12]) {
      g.strokeStyle = db === 0 ? '#3a404a' : '#1c2128';
      g.beginPath(); g.moveTo(0, yOf(db, H)); g.lineTo(W, yOf(db, H)); g.stroke();
    }
    const eqOn = !opts.hpfOnly && has(p('eq/on')) && on(p('eq/on'));
    const hpf = has(p('hpf/on')) && on(p('hpf/on'));
    g.beginPath();
    for (let x = 0; x <= W; x += 2) {
      const f = fOf(x, W);
      let db = 0;
      if (eqOn) for (let b = 1; b <= 4; b++) db += biquadDb(bandType(b), get(p(`eq/${b}/freq`)), get(p(`eq/${b}/gain`)), bandQ(b), f, fs());
      if (hpf) db += biquadDb('hp', get(p('hpf/freq')), 0, 0.7071, f, fs());
      const y = yOf(Math.max(-18, Math.min(18, db)), H);
      x === 0 ? g.moveTo(x, y) : g.lineTo(x, y);
    }
    g.strokeStyle = eqOn || (opts.hpfOnly && hpf) ? '#3fb6ff' : '#5c626d'; g.lineWidth = 2; g.stroke();
    if (!opts.hpfOnly && has(p('eq/1/freq'))) {
      for (let b = 1; b <= 4; b++) {
        const x = xOf(get(p(`eq/${b}/freq`)), W), y = yOf(get(p(`eq/${b}/gain`)), H);
        g.fillStyle = colors[b - 1];
        g.beginPath(); g.arc(x, y, 6, 0, 2 * Math.PI); g.fill();
        g.fillStyle = '#000'; g.font = 'bold 8px system-ui'; g.fillText(b, x - 2.5, y + 3);
      }
    }
  };
  for (const s of ['eq/on', 'eq/1/type', 'eq/4/type', 'hpf/on', 'hpf/freq']) if (has(p(s))) watch(p(s), () => c.draw());
  if (has(p('eq/1/freq'))) for (let b = 1; b <= 4; b++) for (const s of ['freq', 'gain', 'width']) watch(p(`eq/${b}/${s}`), () => c.draw());
  if (opts.hpfOnly) return c;
  let band = 0;
  c.addEventListener('pointerdown', e => {
    const r = c.getBoundingClientRect();
    let best = 1e9;
    for (let b = 1; b <= 4; b++) {
      const dx = xOf(get(p(`eq/${b}/freq`)), r.width) - (e.clientX - r.left);
      const dy = yOf(get(p(`eq/${b}/gain`)), r.height) - (e.clientY - r.top);
      if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; band = b; }
    }
    if (best > 40 * 40) { band = 0; return; }
    c.setPointerCapture(e.pointerId);
  });
  c.addEventListener('pointermove', e => {
    if (!band || !c.hasPointerCapture(e.pointerId)) return;
    const r = c.getBoundingClientRect();
    setParam(p(`eq/${band}/freq`), fOf(e.clientX - r.left, r.width));
    setParam(p(`eq/${band}/gain`), Math.round(dbOf(e.clientY - r.top, r.height) * 2) / 2);
  });
  c.addEventListener('wheel', e => {
    if (!band) return;
    e.preventDefault();
    const w = p(`eq/${band}/width`);
    setParam(w, get(w) * (e.deltaY > 0 ? 1.1 : 0.9));
  }, { passive: false });
  return c;
}

function transferGraph(id, which) {
  const c = canvas('graph square');
  const p = s => `${id}/${s}`;
  c.draw = () => {
    const r = prepCanvas(c);
    if (!r) return;
    const { g, W, H } = r;
    const xOf = db => (db + 80) / 80 * W, yOf = db => H - (db + 80) / 80 * H;
    g.strokeStyle = '#1c2128';
    for (let db = -60; db <= 0; db += 20) {
      g.beginPath(); g.moveTo(xOf(db), 0); g.lineTo(xOf(db), H); g.stroke();
      g.beginPath(); g.moveTo(0, yOf(db)); g.lineTo(W, yOf(db)); g.stroke();
    }
    const T = get(p(`${which}/thresh`));
    const mode = which === 'comp' ? get(p('comp/mode')) : 0;
    const knee = which === 'comp' ? (mode === 2 ? 10 : mode === 3 ? 0 : get(p('comp/knee'))) : 0;
    g.beginPath();
    for (let db = -80; db <= 0; db += 0.5) {
      let out = db;
      if (which === 'gate') { if (db < T) out = db - get(p('gate/range')); }
      else {
        const R = get(p('comp/ratio')), over = db - T, slope = 1 / R - 1;
        let gr = 0;
        if (knee > 0 && 2 * Math.abs(over) <= knee) gr = slope * Math.pow(over + knee / 2, 2) / (2 * knee);
        else if (over > 0) gr = slope * over;
        out = db + gr + get(p('comp/makeup'));
      }
      const y = yOf(Math.max(-80, out));
      db === -80 ? g.moveTo(xOf(db), y) : g.lineTo(xOf(db), y);
    }
    g.strokeStyle = on(p(`${which}/on`)) ? '#3fb6ff' : '#5c626d'; g.lineWidth = 2; g.stroke();
    // Live input level marker.
    const m = inputById(id) ? inMeter(id) : null;
    if (m && m.in > -80) {
      g.fillStyle = '#ffc12e';
      g.fillRect(xOf(m.in) - 1, H - 6, 3, 6);
    }
  };
  const deps = which === 'gate' ? ['gate/on', 'gate/thresh', 'gate/range'] : ['comp/on', 'comp/thresh', 'comp/ratio', 'comp/knee', 'comp/makeup', 'comp/mode'];
  for (const s of deps) watch(p(s), () => c.draw());
  onFrame(() => c.draw());
  return c;
}

// --------------------------------------------------------------------------------- Routing
function screenRouting(page) {
  const id = S.sel;
  const inp = inputById(id), bus = busById(id);
  if (inp) {
    const tab = tabs('route', [['sends', 'MIX SENDS'], ['assign', 'DCA · MUTE GRP'], ['out', 'DIRECT OUT']]);
    if (tab === 'sends') {
      page.append(card('MAIN LR', tkey(`${id}/lr`, 'ASSIGN')));
      const c = card('MIXES AND FX SENDS');
      const list = el('div', 'list');
      for (const b of buses().filter(x => x.kind === 'mix' || (x.kind === 'fx' && inp.kind !== 'fxret'))) {
        const base = `${id}/${b.kind}/${idx(b.id)}`;
        const nm = el('div', 'nm');
        watchLabel(b.id, () => { nm.textContent = displayName(b.id); });
        nm.style.color = `var(--c-${b.kind})`;
        list.append(nm, knob(`${base}/level`, 'LEVEL'), tkey(`${base}/on`, 'ASSIGN'), tkey(`${base}/pre`, 'PRE'));
      }
      c.append(list);
      page.append(c);
    } else if (tab === 'assign') {
      const c = card('DCA GROUPS');
      const r1 = el('div', 'row');
      for (let d = 1; d <= S.info.dcas; d++) { const k = tkey(`${id}/dca/${d}`, ''); watchLabel(`dca/${d}`, t => { k.textContent = (t || `DCA ${d}`).toUpperCase(); }); r1.append(k); }
      c.append(r1);
      const c2 = card('MUTE GROUPS');
      const r2 = el('div', 'row');
      for (let m = 1; m <= S.info.muteGroups; m++) { const k = tkey(`${id}/mg/${m}`, '', 'mute'); watchLabel(`mg/${m}`, t => { k.textContent = (t || `MG ${m}`).toUpperCase(); }); r2.append(k); }
      c2.append(r2);
      page.append(c, c2);
    } else {
      const c = card('DIRECT OUT');
      c.append(el('div', 'note', inp.kind === 'fxret' ? 'FX returns have no direct out.' :
        'Direct outs are taken post-preamp (after gain and polarity), ready for multitrack recording. Patch them to any output in SETUP → OUTPUTS, or record them with HOME → DRIVE.'));
      page.append(c);
    }
  } else if (bus) {
    tabs('route', []);
    const c = card('PATCHED TO OUTPUTS');
    const legs = legNames(bus);
    const names = [];
    for (let o = 1; o <= S.info.physOutputs; o++) {
      const src = S.info.outputSources[get(`out/${o}/src`)] || '';
      if (legs.includes(src)) names.push(`Out ${o} ← ${src}`);
    }
    c.append(el('div', 'note', names.length ? names.join(' · ') : 'Not patched to any output (see SETUP → OUTPUTS).'));
    page.append(c);
    if (bus.kind === 'fx') page.append(card(`${bus.name} feeds FX engine ${idx(bus.id)}`), el('div', 'note', 'See the FX screen for the effect and its return.'));
  }
}
function legNames(bus) {
  if (bus.kind === 'lr') return ['LR L', 'LR R'];
  const m = bus.name.match(/^(.*) (\d+)-(\d+)$/);
  return m ? [`${m[1]} ${m[2]}`, `${m[1]} ${m[3]}`] : [bus.name];
}

// -------------------------------------------------------------------------------------- FX
function screenFx(page) {
  const tab = tabs('fx', [1, 2, 3, 4].map(n => [String(n), `FX${n}`]));
  const n = +tab, base = `fx/${n}`;
  const src = card('SOURCE');
  if (n <= 2) {
    src.append(el('div', 'note', `FX${n} is fed by the FX${n} send bus. Select the FX${n} mix key to set channel sends on the faders.`));
    const r = el('div', 'row');
    r.append(knob(`fx/${n}/fader`, 'SEND MSTR'), tkey(`fx/${n}/mute`, 'MUTE', 'mute'));
    src.append(r);
  } else {
    const r = el('div', 'row');
    r.append(el('span', 'note', `FX${n} is fed from`), choiceSelect(`${base}/src`));
    src.append(r, el('div', 'note', 'On the Qu-16, FX3 and FX4 take a mix as their input. Set up that mix as an effects send.'));
  }
  page.append(src);
  const typeCard = card('EFFECT TYPE');
  const tr = el('div', 'row');
  S.info.fxTypes.forEach((t, i) => {
    const b = key(t.toUpperCase(), 'sel', () => setParam(`${base}/type`, i));
    watch(`${base}/type`, v => b.classList.toggle('on', v === i));
    tr.append(b);
  });
  typeCard.append(tr);
  page.append(typeCard);
  const params = card('PARAMETERS');
  const pr = el('div', 'row');
  params.append(pr);
  const layouts = {
    0: [['rev/decay', 'DECAY'], ['rev/predelay', 'PRE-DLY'], ['rev/damp', 'HF DAMP'], ['rev/size', 'SIZE']],
    1: [['dly/time', 'TIME'], ['dly/feedback', 'FEEDBK'], ['dly/hicut', 'HI CUT']],
    2: [['cho/rate', 'RATE'], ['cho/depth', 'DEPTH'], ['cho/delay', 'DELAY']],
    3: [['flg/rate', 'RATE'], ['flg/depth', 'DEPTH'], ['flg/delay', 'DELAY'], ['flg/feedback', 'FEEDBK']],
  };
  watch(`${base}/type`, t => {
    pr.innerHTML = '';
    for (const [s, l] of layouts[t] || []) pr.append(knob(`${base}/${s}`, l));
    if (t === 1) pr.append(tkey(`${base}/dly/pingpong`, 'PING-PONG'));
  });
  page.append(params);
  const ret = card(`FX RETURN ${n}`);
  const rr = el('div', 'row');
  rr.append(knob(`fxret/${n}/fader`, 'LEVEL'), tkey(`fxret/${n}/mute`, 'MUTE', 'mute'), knob(`fxret/${n}/pan`, 'BAL'),
    key('SELECT', '', () => select(`fxret/${n}`)));
  ret.append(rr, el('div', 'note', 'FX returns are on layer 2, with their own PEQ and mix sends.'));
  page.append(ret);
}

// ---------------------------------------------------------------------------------- Scenes
let pendingRecall = null;
function stepScene(dir) {
  if (!S.scenes.length) { toast('No scenes stored'); return; }
  const i = S.scenes.indexOf(S.scene);
  const next = S.scenes[Math.min(S.scenes.length - 1, Math.max(0, (i < 0 ? -1 : i) + dir))];
  send(`scene load ${next}`);
  toast(`Recalled ${next}`);
}
function screenScenes(page) {
  tabs('scenes', []);
  const go = card('CURRENT');
  const r = el('div', 'row');
  r.append(el('div', 'grow', ''), key('◀ PREV', '', () => stepScene(-1)), key('GO NEXT ▶', 'toggle on', () => stepScene(1)));
  r.firstChild.append('Scene ', Object.assign(el('b'), { textContent: S.scene || '—' }));
  go.append(r);
  page.append(go);
  const store = card('STORE');
  const sr = el('div', 'row');
  const numIn = el('input', 'txt');
  numIn.id = 'scene-num'; numIn.type = 'number'; numIn.min = 1; numIn.max = 100; numIn.style.width = '64px';
  const used = new Set(S.scenes.map(s => parseInt(s, 10)).filter(n => n > 0));
  let free = 1;
  while (used.has(free) && free < 100) free++;
  numIn.value = free;
  const nameIn = el('input', 'txt grow');
  nameIn.id = 'scene-name'; nameIn.placeholder = 'Scene name';
  sr.append(numIn, nameIn, key('STORE', 'sel on', () => {
    const num = Math.min(100, Math.max(1, parseInt(numIn.value, 10) || 1));
    const nm = nameIn.value.trim().replace(/[^A-Za-z0-9 _-]/g, '') || 'Scene';
    send(`scene save ${String(num).padStart(3, '0')} ${nm}`.slice(0, 54));
    toast(`Stored ${String(num).padStart(3, '0')} ${nm}`);
  }));
  store.append(sr);
  page.append(store);
  const list = card('RECALL');
  if (!S.scenes.length) list.append(el('div', 'note', 'No scenes stored yet.'));
  for (const s of S.scenes) {
    const row = el('div', 'row');
    const confirming = pendingRecall === s;
    row.append(el('div', 'grow nm', (s === S.scene ? '● ' : '') + s),
      key(confirming ? 'CONFIRM RECALL' : 'RECALL', confirming ? 'confirm' : '', () => {
        if (confirming) { pendingRecall = null; send(`scene load ${s}`); toast(`Recalled ${s}`); }
        else { pendingRecall = s; renderScreen(); }
      }));
    list.append(row);
  }
  page.append(list);
}

// ----------------------------------------------------------------------------------- Setup
function screenSetup(page) {
  const tab = tabs('setup', [['outputs', 'OUTPUTS'], ['talk', 'TALK'], ['gen', 'SIG GEN'], ['custom', 'CUSTOM'],
    ['soft', 'SOFTKEYS'], ['names', 'NAMES'], ['system', 'SYSTEM']]);
  if (tab === 'outputs') {
    const c = card('OUTPUT PATCH');
    const list = el('div', 'list');
    for (let o = 1; o <= S.info.physOutputs; o++) list.append(el('div', 'nm', `Output ${o}`), choiceSelect(`out/${o}/src`), el('span'), el('span'));
    c.append(list);
    page.append(c);
  } else if (tab === 'talk' || tab === 'gen') {
    const b = tab === 'talk' ? 'talk' : 'gen';
    const c = card(tab === 'talk' ? 'TALKBACK' : 'SIGNAL GENERATOR', tkey(`${b}/on`, tab === 'talk' ? 'TALK' : 'ON'));
    const r = el('div', 'row');
    if (b === 'talk') r.append(el('span', 'note', 'Source'), choiceSelect('talk/src'), knob('talk/gain', 'GAIN'), tkey('talk/hpf', 'HPF 120 Hz'));
    else {
      r.append(el('span', 'note', 'Type'), choiceSelect('gen/type'), knob('gen/freq', 'FREQ'), knob('gen/level', 'LEVEL'));
    }
    c.append(r);
    const a = el('div', 'row');
    a.append(el('span', 'note', 'Assign'), tkey(`${b}/lr`, 'LR'));
    for (const bus of buses().filter(x => x.kind === 'mix')) {
      const k = tkey(`${b}/mix/${idx(bus.id)}`, '');
      watchLabel(bus.id, () => { k.textContent = displayName(bus.id).toUpperCase(); });
      a.append(k);
    }
    c.append(a);
    if (b === 'gen') c.append(el('div', 'note', 'Start with the level down. Pink noise and the RTA together are a quick way to check a system.'));
    page.append(c);
  } else if (tab === 'custom') {
    const c = card('CUSTOM LAYER · 16 STRIPS');
    const list = el('div', 'list');
    for (let n = 1; n <= 16; n++) list.append(el('div', 'nm', `Strip ${n}`), choiceSelect(`custom/${n}/strip`), el('span'), el('span'));
    c.append(list);
    page.append(c);
    for (let n = 1; n <= 16; n++) watch(`custom/${n}/strip`, () => { if (S.layer === 'C' && S.ready) queueMicrotask(renderBank); });
  } else if (tab === 'soft') {
    const c = card('SOFTKEYS');
    const list = el('div', 'list');
    for (let n = 1; n <= 4; n++) list.append(el('div', 'nm', `SoftKey ${n}`), choiceSelect(`soft/${n}/func`), el('span'), el('span'));
    c.append(list);
    page.append(c);
  } else if (tab === 'names') {
    const c = card('NAMES');
    const list = el('div', 'list');
    const keys = [...inputs().map(i => [i.id, i.name]), ...buses().map(b => [b.id, b.name]),
      ...Array.from({ length: S.info.dcas }, (_, i) => [`dca/${i + 1}`, `DCA ${i + 1}`]),
      ...Array.from({ length: S.info.muteGroups }, (_, i) => [`mg/${i + 1}`, `MG ${i + 1}`])];
    let focus = null;
    for (const [k, def] of keys) {
      const input = el('input', 'txt');
      input.id = 'name-' + k.replace('/', '-');
      input.placeholder = def;
      input.maxLength = 24;
      watchLabel(k, t => { if (document.activeElement !== input) input.value = t; });
      input.addEventListener('change', () => send(`label ${k} ${input.value}`));
      list.append(el('div', 'nm', def), input, el('span'), el('span'));
      if (S.focusName === k) focus = input;
    }
    c.append(list);
    page.append(c);
    if (focus) { S.focusName = null; setTimeout(() => { focus.scrollIntoView({ block: 'center' }); focus.focus(); focus.select(); }, 0); }
  } else {
    const c = card('SYSTEM');
    const i = S.info;
    c.append(el('div', '', `Engine ${i.name} ${i.version} · ${i.backend} · ${i.sampleRate} Hz`),
      el('div', '', `${i.inputs.length} input channels · ${i.buses.length} masters · ${i.physInputs} in / ${i.physOutputs} out`),
      el('div', 'note', `Control surface at ${location.host}. Any number of tablets can connect at once.`));
    page.append(c);
  }
}

// ============================================================================== connection
function handleMessage(text) {
  if (text.startsWith('m ')) { S.meters = text.slice(2).split(',').map(Number); return; }
  if (text.startsWith('r ')) { S.rta = text.slice(2).split(',').map(Number); return; }
  if (text.startsWith('d ')) {
    const [state, mode, secs, drops, ...file] = text.slice(2).split(' ');
    S.rec = { state, mode, seconds: +secs || 0, dropouts: +drops || 0, file: file.join(' ') };
    return;
  }
  if (text.startsWith('i ')) { S.info = JSON.parse(text.slice(2)); return; }
  if (text.startsWith('k\n')) {
    for (const line of text.split('\n').slice(1)) {
      const [path, min, max, def, kind, unit] = line.split(' ');
      S.meta[path] = { min: +min, max: +max, def: +def, kind, unit: unit || '' };
    }
    return;
  }
  if (text.startsWith('s ')) {
    const lines = text.split('\n');
    S.scene = lines[0].slice(2);
    S.scenes = lines.slice(1).filter(Boolean);
    if (S.ready) { renderToolbar(); if (S.screen === 'scenes') renderScreen(); }
    return;
  }
  const first = !S.ready;
  let layoutChanged = false;
  for (const line of text.split('\n')) {
    if (line.startsWith('p ')) {
      const sp = line.indexOf(' ', 2);
      const path = line.slice(2, sp);
      setLocal(path, parseFloat(line.slice(sp + 1)));
      if (path.startsWith('custom/') && S.layer === 'C') layoutChanged = true;
    } else if (line.startsWith('l ')) {
      const sp = line.indexOf(' ', 2);
      const k = sp < 0 ? line.slice(2) : line.slice(2, sp);
      S.labels[k] = sp < 0 ? '' : line.slice(sp + 1);
      (labelWatchers.get(k) || []).forEach(f => f(S.labels[k]));
      if (k === S.sel && S.ready) renderToolbar();
    } else if (line.startsWith('e ')) {
      toast(line.slice(2));
    }
  }
  if (first && S.info && Object.keys(S.params).length) {
    S.ready = true;
    renderControls();
    select(S.sel);
  } else if (layoutChanged) {
    renderBank();
  }
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  const conn = document.getElementById('conn');
  ws.onopen = () => { conn.classList.add('on'); conn.title = `Connected to ${location.host}`; };
  ws.onmessage = e => handleMessage(e.data);
  ws.onclose = () => {
    conn.classList.remove('on'); conn.title = 'Reconnecting…';
    S.ready = false;
    for (const k of Object.keys(S.labels)) delete S.labels[k];
    setTimeout(connect, 1000);
  };
}

function animate() {
  if (S.meters && S.ready) for (const list of meterViews.values()) for (const f of list) f();
  requestAnimationFrame(animate);
}

connect();
requestAnimationFrame(animate);
