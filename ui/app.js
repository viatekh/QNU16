// QNU16 control surface. A Qu-16-style layout: SuperStrip (top left), touchscreen (top right),
// fader bank with layers (bottom left), mix keys + master strip (bottom right).
// Talks to qnu16d over a WebSocket; see docs/protocol.md.
'use strict';

// ===================================================================================== state
const S = {
  info: null,          // topology from the engine ("i" message)
  meta: {},            // path -> {min, max, def, kind: 'c'|'t'|'h', unit}
  params: {},          // path -> value
  labels: {},          // key -> name
  meters: null,        // latest meter frame (array of numbers)
  sel: 'ch/1',         // selected input or bus id
  layer: 'A',          // A: Ch 1-16, B: ST/FX ret/Groups/DCA, C: masters
  mix: 'lr',           // bus id on the mix keys: which mix the faders control
  page: 'home',        // touchscreen page
  sub: {},             // page -> sub tab
  scenes: [],
  scene: '',
};
let ws = null;

// Watchers are grouped by tag so a view can be rebuilt without leaking old handlers.
const watchers = new Map();       // path -> [fn]
const labelWatchers = new Map();  // key -> [fn]
const meterViews = new Map();     // tag -> [fn(meters)]
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
function onMeters(fn) {
  if (!meterViews.has(currentTag)) meterViews.set(currentTag, []);
  meterViews.get(currentTag).push(fn);
}
function setLocal(path, v) {
  S.params[path] = v;
  const list = watchers.get(path);
  if (list) for (const f of list) f(v);
}
function send(text) { if (ws && ws.readyState === 1) ws.send(text); }

// Coalesce outgoing changes to one message per animation frame.
const pending = new Map();
let flushQueued = false;
function setParam(path, v) {
  const m = S.meta[path];
  if (m) {
    v = Math.min(m.max, Math.max(m.min, v));
    if (m.kind === 't') v = v >= 0.5 ? 1 : 0;
    if (m.kind === 'h') v = Math.round(v);
  }
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
const get = path => S.params[path];
const on = path => S.params[path] >= 0.5;
const toggle = path => setParam(path, on(path) ? 0 : 1);

// ================================================================================ topology
const inputs = () => (S.info ? S.info.inputs : []);
const buses = () => (S.info ? S.info.buses : []);
const inputById = id => inputs().find(i => i.id === id);
const busById = id => buses().find(b => b.id === id);
const isInput = id => !!inputById(id);
function displayName(id) {
  if (S.labels[id]) return S.labels[id];
  // FX returns take the name of their FX unit ("Hall") until named themselves.
  if (id.startsWith('fxret/') && S.labels['fx/' + id.split('/')[1]]) return S.labels['fx/' + id.split('/')[1]] + ' Rt';
  return (inputById(id) || busById(id) || { name: id }).name;
}
const KIND_COLOR = { lr: 'var(--c-lr)', mix: 'var(--c-mix)', grp: 'var(--c-grp)', fx: 'var(--c-fx)', mtx: 'var(--c-mtx)' };

// Meter frame layout: per input [in, out, gr, gate], per bus [L, R, gr], then PAFL L/R.
function inputMeter(id) {
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
  if ((m.unit === 'Hz' || m.unit === 'ms' || m.unit === 's') && m.min > 0) return 'log';
  if (m.unit === ':1' || path.endsWith('/q')) return 'log';
  return 'lin';
}
function toPos(path, v) {
  const m = S.meta[path];
  if (!m) return 0;
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
function choiceLabels(path) {
  if (path.endsWith('/type') && path.startsWith('fx/')) return S.info.fxTypes;
  if (path.endsWith('eq/1/type') || path.endsWith('eq/4/type')) return ['Shelf', 'Bell'];
  if (path.endsWith('comp/mode')) return ['Peak', 'RMS'];
  if (path.startsWith('out/')) return S.info.outputSources;
  return null;
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

// A toggle key bound to a parameter.
function tkey(path, text, cls = 'toggle') {
  const b = key(text, cls, () => toggle(path));
  watch(path, v => b.classList.toggle('on', v >= 0.5));
  return b;
}

// Vertical-drag helper: calls apply(startPos + delta) in 0..1 position space.
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

// Rotary encoder bound to a parameter. Drag up/down, wheel, double-click resets to default.
function knob(path, label, opts = {}) {
  const k = el('div', 'knob');
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
  const lab = el('div', 'kl', label);
  const val = el('div', 'kv');
  k.append(svg, val, lab);
  const m = S.meta[path];
  const bipolar = m && m.min < 0 && m.max > 0 && taper(path) !== 'fader';
  const render = v => {
    const p = Math.min(1, Math.max(0, toPos(path, v)));
    const c = bipolar ? toPos(path, 0) : 0;
    arc.setAttribute('d', p >= c ? arcPath(c, Math.max(p, c + 0.001)) : arcPath(p, c));
    ptr.setAttribute('transform', `rotate(${-135 + 270 * p} 20 20)`);
    val.textContent = fmt(path, v);
  };
  watch(path, render);
  if (opts.enable) watch(opts.enable, v => k.classList.toggle('off', v < 0.5));
  dragPos(k, () => toPos(path, get(path)), p => setParam(path, fromPos(path, p)), 180);
  k.addEventListener('dblclick', () => m && setParam(path, m.def));
  k.addEventListener('wheel', e => {
    e.preventDefault();
    setParam(path, fromPos(path, toPos(path, get(path)) - Math.sign(e.deltaY) * 0.01));
  }, { passive: false });
  return k;
}

// LED ladder meter: dB thresholds bottom to top.
const LED_DB = [-48, -30, -18, -12, -6, -3, 0];
function leds(cls = '') {
  const m = el('div', 'leds ' + cls);
  const items = LED_DB.map((db, i) => { const e = el('i'); if (i >= 5) e.classList.add(i === 6 ? 'r' : 'y'); m.append(e); return e; });
  m.set = db => items.forEach((e, i) => e.classList.toggle('on', db >= LED_DB[i]));
  m.setGr = gr => items.forEach((e, i) => e.classList.toggle('on', gr >= [1, 2, 4, 6, 9, 12, 18][i]));
  return m;
}

// Fader bound to a (changeable) parameter path.
function fader(pathFn, readout) {
  const f = el('div', 'fader');
  f.append(el('div', 'track'));
  const ticks = [10, 0, -10, -20, -30, -40, -60].map(db => { const t = el('div', 'tick' + (db === 0 ? ' u' : '')); t.db = db; f.append(t); return t; });
  const cap = el('div', 'cap');
  f.append(cap);
  const render = () => {
    const path = pathFn();
    const v = path ? get(path) ?? -90 : -90;
    const g = f.clientHeight - 20;
    cap.style.top = (10 + (1 - faderPos(v)) * g) + 'px';
    ticks.forEach(t => { t.style.top = (10 + (1 - faderPos(t.db)) * g) + 'px'; });
    if (readout) readout.textContent = path ? fmt(path, v) : '';
  };
  f.render = render;
  const set = v => { const p = pathFn(); if (p) { setParam(p, v); render(); } };
  let dragging = false, startY = 0, startPos = 0;
  f.addEventListener('pointerdown', e => {
    if (!pathFn()) return;
    dragging = true; f.setPointerCapture(e.pointerId);
    startY = e.clientY; startPos = faderPos(get(pathFn()) ?? -90);
    if (e.target !== cap) {  // tap the track: jump there
      const r = f.getBoundingClientRect();
      startPos = 1 - (e.clientY - r.top - 10) / (r.height - 20);
      set(faderDb(startPos));
    }
  });
  f.addEventListener('pointermove', e => {
    if (!dragging) return;
    const h = f.clientHeight - 20;
    set(faderDb(startPos - (e.clientY - startY) / h * (e.shiftKey ? 0.25 : 1)));
  });
  const end = () => { dragging = false; };
  f.addEventListener('pointerup', end);
  f.addEventListener('pointercancel', end);
  f.addEventListener('dblclick', () => set(0));
  f.addEventListener('wheel', e => {
    e.preventDefault();
    const cur = get(pathFn()) ?? -90;
    set(Math.min(10, Math.max(-90, (cur <= -89.9 ? -60 : cur) - Math.sign(e.deltaY) * 0.5)));
  }, { passive: false });
  new ResizeObserver(render).observe(f);
  return f;
}

function rename(id) {
  const t = prompt(`Name for ${displayName(id)}`, S.labels[id] || '');
  if (t !== null) send(`label ${id} ${t}`);
}

// ================================================================================ fader bank
// What a mix key does to the input faders:
//   LR / Grp  -> channel faders (Grp: Sel keys become group assign keys)
//   Mix / FX  -> sends on faders
//   Mtx       -> faders become matrix source levels for LR, mixes and groups
function mixKind() { const b = busById(S.mix); return b ? b.kind : 'lr'; }
function mixIndex() { return S.mix.split('/')[1]; }

function layerStrips() {
  const kind = mixKind();
  if (kind === 'mtx') {
    const srcs = buses().filter(b => b.kind === 'lr' || b.kind === 'mix' || b.kind === 'grp');
    return srcs.map(b => ({ type: 'mtxsrc', id: b.id }));
  }
  if (S.layer === 'A') return inputs().filter(i => i.kind === 'mono').map(i => ({ type: 'input', id: i.id }));
  if (S.layer === 'B') {
    const list = inputs().filter(i => i.kind !== 'mono').map(i => ({ type: 'input', id: i.id }));
    buses().filter(b => b.kind === 'grp').forEach(b => list.push({ type: 'bus', id: b.id }));
    for (let d = 1; d <= S.info.dcas; d++) list.push({ type: 'dca', id: `dca/${d}` });
    return list;
  }
  return buses().filter(b => b.kind !== 'grp').map(b => ({ type: 'bus', id: b.id }));
}

// Parameter a strip's fader controls under the current mix.
function stripFaderPath(s) {
  const kind = mixKind();
  if (s.type === 'mtxsrc') return `${S.mix}/${s.id}`;
  if (s.type === 'dca') return `${s.id}/fader`;
  if (s.type === 'bus') return `${s.id}/fader`;
  const inp = inputById(s.id);
  if (kind === 'mix') return `${s.id}/mix/${mixIndex()}/level`;
  if (kind === 'fx') return inp.kind === 'fxret' ? null : `${s.id}/fx/${mixIndex()}/level`;
  return `${s.id}/fader`;
}

function buildStrip(s, opts = {}) {
  const st = el('div', 'strip');
  const kind = mixKind();
  const isIn = s.type === 'input';
  const bus = busById(s.id);
  const sendMode = isIn && (kind === 'mix' || kind === 'fx');
  st.classList.toggle('sendmode', sendMode || s.type === 'mtxsrc');
  const color = s.type === 'dca' ? 'var(--c-dca)' : bus ? KIND_COLOR[bus.kind] : sendMode || s.type === 'mtxsrc' ? KIND_COLOR[kind] : '#4b525c';
  st.style.setProperty('--kc', color);

  const name = el('div', 'name');
  const defaultName = s.type === 'dca' ? `DCA ${s.id.split('/')[1]}` : null;
  watchLabel(s.id, t => { name.textContent = defaultName ? t || defaultName : displayName(s.id); });
  name.addEventListener('dblclick', () => rename(s.id));

  const path = opts.faderPath || stripFaderPath(s);
  if (!path) st.classList.add('disabled');

  // Keys: Mute / Sel / PAFL (Sel becomes the group-assign key while a group is on the mix keys).
  const mutePath = s.type === 'mtxsrc' ? null : `${s.id}/mute`;
  const muteKey = mutePath ? tkey(mutePath, 'MUTE', 'mute') : key('', '');
  if (!mutePath) muteKey.style.visibility = 'hidden';
  let selKey;
  if (isIn && kind === 'grp' && !opts.master) {
    const g = mixIndex();
    selKey = tkey(`${s.id}/grp/${g}`, 'ASSIGN', 'assign');
  } else if (s.type === 'dca') {
    selKey = key('', '');
    selKey.style.visibility = 'hidden';
  } else {
    selKey = key('SEL', 'sel', () => select(s.id));
    selKey.classList.toggle('on', S.sel === s.id);
    st.classList.toggle('selected', S.sel === s.id);
  }
  const paflKey = s.type === 'dca' || s.type === 'mtxsrc' ? key('', '') : tkey(`${s.id}/pafl`, 'PAFL', 'pafl');
  if (s.type === 'dca' || s.type === 'mtxsrc') paflKey.style.visibility = 'hidden';

  const body = el('div', 'body');
  const meter = leds();
  const gr = leds('gr');
  const val = el('div', 'val');
  const fd = fader(() => path, val);
  if (path) watch(path, () => fd.render());
  body.append(meter, fd);
  if (isIn && inputById(s.id).kind !== 'fxret') body.insertBefore(gr, fd);
  if (s.type !== 'dca') {
    onMeters(() => {
      if (isIn) {
        const m = inputMeter(s.id);
        meter.set(m.in);
        gr.setGr(m.gr);
      } else {
        const m = busMeter(s.id);
        if (m) meter.set(Math.max(m.l, m.r));
      }
    });
  } else {
    meter.style.visibility = 'hidden';
  }

  st.append(name, muteKey, selKey, paflKey, body, val);
  return st;
}

function renderBank() {
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
    const mtx = mixKind() === 'mtx';
    for (const [id, text] of [['A', 'CH 1-16'], ['B', 'ST FX GRP DCA'], ['C', 'MSTR']]) {
      const b = key('', mtx ? '' : (S.layer === id ? 'on' : ''), () => { S.layer = id; renderBank(); });
      b.append(el('div', 'lk', id), el('div', 'ls', text));
      if (mtx) b.disabled = true;
      root.append(b);
    }
  });
}

function renderMaster() {
  withTag('master', () => {
    const root = document.getElementById('master');
    root.innerHTML = '';
    root.append(buildStrip({ type: 'bus', id: S.mix }, { master: true }));
  });
}

function renderMixKeys() {
  withTag('mixkeys', () => {
    const root = document.getElementById('mixKeys');
    root.innerHTML = '';
    const groups = [['MIX', ['lr', 'mix']], ['GROUP / MATRIX', ['grp', 'mtx']], ['FX SENDS', ['fx']]];
    for (const [caption, kinds] of groups) {
      root.append(el('div', 'cap', caption));
      for (const b of buses().filter(x => kinds.includes(x.kind))) {
        const k = key(b.name.replace('Mix ', b.kind === 'mix' ? 'MIX ' : '').toUpperCase(), S.mix === b.id ? 'on' : '', () => setMix(b.id));
        k.style.setProperty('--kc', KIND_COLOR[b.kind]);
        watchLabel(b.id, t => { k.textContent = (t || b.name).toUpperCase(); });
        root.append(k);
      }
    }
    const mg = document.getElementById('muteGroups');
    mg.innerHTML = '';
    for (let m = 1; m <= S.info.muteGroups; m++) {
      const k = tkey(`mg/${m}/mute`, `MG${m}`, 'mute');
      watchLabel(`mg/${m}`, t => { k.textContent = t || `MG${m}`; });
      mg.append(k);
    }
  });
}

function setMix(id) {
  S.mix = id;
  renderMixKeys();
  renderBank();
  renderMaster();
}

function select(id) {
  S.sel = id;
  document.getElementById('selName').textContent = displayName(id);
  renderBank();
  renderMaster();
  renderSuperStrip();
  renderScreen();
}

// ================================================================================ SuperStrip
function renderSuperStrip() {
  withTag('super', () => {
    const root = document.getElementById('superstrip');
    root.innerHTML = '';
    const id = S.sel;
    const p = s => `${id}/${s}`;
    const has = s => p(s) in S.meta;
    const head = el('div', 'ss-head');
    const h = el('h2');
    watchLabel(id, () => { h.textContent = displayName(id); });
    const inp = inputById(id), bus = busById(id);
    head.append(h, el('span', 'tag', inp ? `${inp.kind === 'mono' ? 'Mono' : inp.kind === 'stereo' ? 'Stereo' : 'FX return'} input` : `${bus.name} master`));
    root.append(head);

    const row = el('div', 'ss-row');
    const sec = (title, enablePath, knobs) => {
      const s = el('div', 'ss-sec');
      const t = el('h3', '', title);
      if (enablePath && has(enablePath)) t.append(tkey(p(enablePath), 'IN'));
      s.append(t);
      const k = el('div', 'ss-knobs');
      for (const [path, label] of knobs) if (has(path)) k.append(knob(p(path), label, { enable: enablePath && has(enablePath) ? p(enablePath) : null }));
      s.append(k);
      row.append(s);
      return s;
    };
    if (has('gain')) {
      const s = sec('PREAMP', null, [['gain', 'GAIN']]);
      s.append(tkey(p('polarity'), 'Ø'));
    }
    if (has('hpf/freq')) sec('HPF', 'hpf/on', [['hpf/freq', 'FREQ']]);
    if (has('gate/thresh')) sec('GATE', 'gate/on', [['gate/thresh', 'THRESH'], ['gate/range', 'DEPTH']]);
    if (has('eq/1/freq')) {
      const names = ['LF', 'LM', 'HM', 'HF'];
      for (let b = 1; b <= 4; b++)
        sec(names[b - 1], b === 1 ? 'eq/on' : null, [[`eq/${b}/gain`, 'GAIN'], [`eq/${b}/freq`, 'FREQ'], [`eq/${b}/q`, 'Q']]);
    }
    if (has('comp/thresh')) sec('COMP', 'comp/on', [['comp/thresh', 'THRESH'], ['comp/ratio', 'RATIO'], ['comp/makeup', 'GAIN']]);
    const panPath = has('pan') ? 'pan' : has('balance') ? 'balance' : null;
    if (panPath) sec(panPath === 'pan' && !(inp && inp.stereo) ? 'PAN' : 'BAL', null, [[panPath, panPath === 'pan' && !(inp && inp.stereo) ? 'PAN' : 'BAL']]);
    root.append(row);
  });
}

// ============================================================================= touchscreen
const PAGES = [['home', 'PROC'], ['route', 'ROUTE'], ['fx', 'FX'], ['scenes', 'SCENE'], ['setup', 'SETUP'], ['meters', 'METER']];

function renderScreenTabs() {
  const root = document.getElementById('screenTabs');
  root.innerHTML = '';
  for (const [id, text] of PAGES) root.append(key(text, S.page === id ? 'on' : '', () => { S.page = id; renderScreenTabs(); renderScreen(); }));
}

function subTabs(page, tabs) {
  const root = document.getElementById('screenSub');
  root.innerHTML = '';
  if (!tabs.length) return null;
  if (!tabs.find(t => t[0] === S.sub[page])) S.sub[page] = tabs[0][0];
  for (const [id, text] of tabs)
    root.append(key(text, S.sub[page] === id ? 'on' : '', () => { S.sub[page] = id; renderScreen(); }));
  return S.sub[page];
}

function card(title, extra) {
  const c = el('div', 'card');
  const h = el('h4', '', title);
  if (extra) h.append(extra);
  c.append(h);
  return c;
}

function renderScreen() {
  withTag('screen', () => {
    const body = document.getElementById('screenBody');
    body.innerHTML = '';
    document.getElementById('screenSub').innerHTML = '';
    ({ home: pageProcessing, route: pageRouting, fx: pageFx, scenes: pageScenes, setup: pageSetup, meters: pageMeters })[S.page](body);
  });
}

// ---------------------------------------------------------------------------- processing
function pageProcessing(body) {
  const id = S.sel;
  const p = s => `${id}/${s}`;
  const has = s => p(s) in S.meta;
  const tabs = [];
  if (has('gain')) tabs.push(['preamp', 'PREAMP']);
  if (has('gate/thresh')) tabs.push(['gate', 'GATE']);
  if (has('eq/1/freq')) tabs.push(['peq', 'PEQ']);
  if (has('geq/1')) tabs.push(['geq', 'GEQ']);
  if (has('comp/thresh')) tabs.push(['comp', 'COMP']);
  if (has('delay/time')) tabs.push(['delay', 'DELAY']);
  const tab = subTabs('home', tabs);
  const knobs = (c, list, enable) => {
    const r = el('div', 'row');
    for (const [s, l] of list) if (has(s)) r.append(knob(p(s), l, { enable: enable ? p(enable) : null }));
    c.append(r);
  };
  if (!tab) { body.append(el('div', '', 'No processing on this channel.')); return; }

  if (tab === 'preamp') {
    const c = card('PREAMP');
    knobs(c, [['gain', 'GAIN']]);
    c.append(el('div', 'row'));
    c.lastChild.append(tkey(p('polarity'), 'POLARITY Ø'));
    c.append(el('div', 'kl', 'Analogue gain and 48V are set on the interface. This gain is digital trim.'));
    body.append(c);
    const h = card('HIGH-PASS FILTER', tkey(p('hpf/on'), 'IN'));
    knobs(h, [['hpf/freq', 'FREQ']], 'hpf/on');
    body.append(h);
  } else if (tab === 'gate') {
    const c = card('GATE', tkey(p('gate/on'), 'IN'));
    const row = el('div', 'row');
    const g = transferGraph(id, 'gate');
    row.append(g);
    const k = el('div', 'row grow');
    for (const [s, l] of [['gate/thresh', 'THRESH'], ['gate/range', 'DEPTH'], ['gate/attack', 'ATTACK'], ['gate/hold', 'HOLD'], ['gate/release', 'RELEASE']])
      k.append(knob(p(s), l, { enable: p('gate/on') }));
    row.append(k);
    c.append(row);
    body.append(c);
  } else if (tab === 'peq') {
    const c = card('PARAMETRIC EQ', tkey(p('eq/on'), 'IN'));
    c.append(eqGraph(id));
    const row = el('div', 'row');
    const names = ['LF', 'LM', 'HM', 'HF'];
    for (let b = 1; b <= 4; b++) {
      const bc = card(names[b - 1], b === 1 || b === 4 ? typeKey(p(`eq/${b}/type`)) : null);
      bc.style.flex = '1';
      const r = el('div', 'row');
      r.append(knob(p(`eq/${b}/gain`), 'GAIN', { enable: p('eq/on') }), knob(p(`eq/${b}/freq`), 'FREQ', { enable: p('eq/on') }), knob(p(`eq/${b}/q`), 'Q', { enable: p('eq/on') }));
      bc.append(r);
      row.append(bc);
    }
    c.append(row);
    body.append(c);
  } else if (tab === 'geq') {
    const c = card('GRAPHIC EQ', tkey(p('geq/on'), 'IN'));
    const g = el('div', 'geq');
    const freqs = ['31', '40', '50', '63', '80', '100', '125', '160', '200', '250', '315', '400', '500', '630', '800', '1k', '1k2', '1k6', '2k', '2k5', '3k1', '4k', '5k', '6k3', '8k', '10k', '12k', '16k'];
    for (let k = 1; k <= S.info.geqBands; k++) {
      const band = el('div', 'band');
      band.append(vbar(p(`geq/${k}`)), el('span', '', freqs[k - 1]));
      g.append(band);
    }
    c.append(g);
    const r = el('div', 'row');
    r.append(key('FLAT', '', () => { for (let k = 1; k <= S.info.geqBands; k++) setParam(p(`geq/${k}`), 0); }));
    c.append(r);
    body.append(c);
  } else if (tab === 'comp') {
    const c = card('COMPRESSOR', tkey(p('comp/on'), 'IN'));
    const row = el('div', 'row');
    row.append(transferGraph(id, 'comp'));
    const k = el('div', 'row grow');
    for (const [s, l] of [['comp/thresh', 'THRESH'], ['comp/ratio', 'RATIO'], ['comp/attack', 'ATTACK'], ['comp/release', 'RELEASE'], ['comp/knee', 'KNEE'], ['comp/makeup', 'GAIN']])
      k.append(knob(p(s), l, { enable: p('comp/on') }));
    row.append(k);
    c.append(row);
    const r2 = el('div', 'row');
    r2.append(typeKey(p('comp/mode')), el('span', 'kl', 'Gain reduction'));
    const grMeter = leds('gr');
    grMeter.style.cssText = 'flex-direction:row;width:140px;height:10px';
    const grText = el('span', 'kv', '0.0 dB');
    r2.append(grMeter, grText);
    let hold = 0;
    onMeters(() => {
      const m = isInput(id) ? inputMeter(id) : busMeter(id);
      hold = Math.max(m.gr, hold * 0.95);
      grMeter.setGr(hold);
      grText.textContent = (hold < 0.05 ? '0.0' : '-' + hold.toFixed(1)) + ' dB';
    });
    c.append(r2);
    body.append(c);
  } else if (tab === 'delay') {
    const c = card('DELAY', tkey(p('delay/on'), 'IN'));
    knobs(c, [['delay/time', 'TIME ms']], 'delay/on');
    const info = el('div', 'kl');
    watch(p('delay/time'), v => { info.textContent = `${(v * 0.343).toFixed(2)} m at 343 m/s`; });
    c.append(info);
    body.append(c);
  }
}

// Cycling key for a choice parameter (Shelf/Bell, Peak/RMS, ...).
function typeKey(path) {
  const b = key('', '', () => {
    const m = S.meta[path];
    setParam(path, get(path) >= m.max ? m.min : get(path) + 1);
  });
  watch(path, v => { b.textContent = fmt(path, v).toUpperCase(); });
  return b;
}

// Vertical bar slider for the GEQ (bipolar around 0 dB).
function vbar(path) {
  const v = el('div', 'vbar');
  const fill = el('i');
  v.append(el('b'), fill);
  const m = S.meta[path];
  watch(path, val => {
    const pos = (val - m.min) / (m.max - m.min);
    const mid = 0.5;
    fill.style.bottom = (Math.min(pos, mid) * 100) + '%';
    fill.style.height = (Math.abs(pos - mid) * 100 + 0.5) + '%';
    v.title = fmt(path, val) + ' dB';
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
  else { b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }  // highpass
  const w = 2 * Math.PI * f / fs, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2w = Math.sin(2 * w);
  const nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2w);
  const dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2w);
  return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

function canvas(cls = 'graph') {
  const c = el('canvas', cls);
  c.draw = () => {};
  new ResizeObserver(() => c.draw()).observe(c);
  return c;
}
function prepCanvas(c) {
  const dpr = window.devicePixelRatio || 1, W = c.clientWidth, H = c.clientHeight;
  if (!W || !H) return null;
  c.width = W * dpr; c.height = H * dpr;
  const g = c.getContext('2d');
  g.scale(dpr, dpr);
  return { g, W, H };
}

// PEQ response with draggable band handles (drag: freq/gain, wheel: Q).
function eqGraph(id) {
  const c = canvas();
  const p = s => `${id}/${s}`;
  const fs = () => S.info.sampleRate;
  const xOf = (f, W) => Math.log(f / 20) / Math.log(1000) * W;
  const fOf = (x, W) => 20 * Math.pow(1000, x / W);
  const yOf = (db, H) => H / 2 - db / 18 * (H / 2 - 8);
  const dbOf = (y, H) => (H / 2 - y) / (H / 2 - 8) * 18;
  const bandType = b => (b === 1 ? (get(p('eq/1/type')) >= 0.5 ? 'peak' : 'lowshelf') : b === 4 ? (get(p('eq/4/type')) >= 0.5 ? 'peak' : 'highshelf') : 'peak');
  const colors = ['#ff6b6b', '#ffc12e', '#38d97a', '#4aa8ff'];
  c.draw = () => {
    const r = prepCanvas(c);
    if (!r) return;
    const { g, W, H } = r;
    g.strokeStyle = '#23272e'; g.lineWidth = 1; g.fillStyle = '#5c626d'; g.font = '9px system-ui';
    for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
      g.beginPath(); g.moveTo(xOf(f, W), 0); g.lineTo(xOf(f, W), H); g.stroke();
      g.fillText(f >= 1000 ? f / 1000 + 'k' : f, xOf(f, W) + 2, H - 3);
    }
    for (const db of [-12, -6, 0, 6, 12]) {
      g.strokeStyle = db === 0 ? '#3a404a' : '#23272e';
      g.beginPath(); g.moveTo(0, yOf(db, H)); g.lineTo(W, yOf(db, H)); g.stroke();
    }
    const eqOn = on(p('eq/on'));
    const hpf = `${id}/hpf/on` in S.meta && on(p('hpf/on'));
    g.beginPath();
    for (let x = 0; x <= W; x += 2) {
      const f = fOf(x, W);
      let db = 0;
      if (eqOn) for (let b = 1; b <= 4; b++) db += biquadDb(bandType(b), get(p(`eq/${b}/freq`)), get(p(`eq/${b}/gain`)), get(p(`eq/${b}/q`)), f, fs());
      if (hpf) db += biquadDb('hp', get(p('hpf/freq')), 0, 0.7071, f, fs());
      const y = yOf(Math.max(-18, Math.min(18, db)), H);
      x === 0 ? g.moveTo(x, y) : g.lineTo(x, y);
    }
    g.strokeStyle = eqOn ? '#3fb6ff' : '#5c626d'; g.lineWidth = 2; g.stroke();
    for (let b = 1; b <= 4; b++) {
      g.fillStyle = colors[b - 1];
      g.beginPath();
      g.arc(xOf(get(p(`eq/${b}/freq`)), W), yOf(get(p(`eq/${b}/gain`)), H), 6, 0, 2 * Math.PI);
      g.fill();
      g.fillStyle = '#000'; g.font = 'bold 8px system-ui';
      g.fillText(b, xOf(get(p(`eq/${b}/freq`)), W) - 2.5, yOf(get(p(`eq/${b}/gain`)), H) + 3);
    }
  };
  for (const s of ['eq/on', 'eq/1/type', 'eq/4/type', 'hpf/on', 'hpf/freq'])
    if (p(s) in S.meta) watch(p(s), () => c.draw());
  for (let b = 1; b <= 4; b++) for (const s of ['freq', 'gain', 'q']) watch(p(`eq/${b}/${s}`), () => c.draw());
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
    const q = p(`eq/${band}/q`);
    setParam(q, get(q) * (e.deltaY > 0 ? 0.9 : 1.1));
  }, { passive: false });
  return c;
}

// Static transfer curve (input level -> output level) for gate or compressor.
function transferGraph(id, which) {
  const c = canvas('graph square');
  const p = s => `${id}/${s}`;
  c.draw = () => {
    const r = prepCanvas(c);
    if (!r) return;
    const { g, W, H } = r;
    const xOf = db => (db + 80) / 80 * W, yOf = db => H - (db + 80) / 80 * H;
    g.strokeStyle = '#23272e';
    for (let db = -60; db <= 0; db += 20) {
      g.beginPath(); g.moveTo(xOf(db), 0); g.lineTo(xOf(db), H); g.stroke();
      g.beginPath(); g.moveTo(0, yOf(db)); g.lineTo(W, yOf(db)); g.stroke();
    }
    g.beginPath();
    const T = get(p(`${which}/thresh`));
    for (let db = -80; db <= 0; db += 0.5) {
      let out = db;
      if (which === 'gate') { if (db < T) out = db - get(p('gate/range')); }
      else {
        const R = get(p('comp/ratio')), K = get(p('comp/knee')), over = db - T, slope = 1 / R - 1;
        let gr = 0;
        if (K > 0 && 2 * Math.abs(over) <= K) gr = slope * Math.pow(over + K / 2, 2) / (2 * K);
        else if (over > 0) gr = slope * over;
        out = db + gr + get(p('comp/makeup'));
      }
      const y = yOf(Math.max(-80, out));
      db === -80 ? g.moveTo(xOf(db), y) : g.lineTo(xOf(db), y);
    }
    g.strokeStyle = on(p(`${which}/on`)) ? '#3fb6ff' : '#5c626d'; g.lineWidth = 2; g.stroke();
  };
  const deps = which === 'gate' ? ['gate/on', 'gate/thresh', 'gate/range'] : ['comp/on', 'comp/thresh', 'comp/ratio', 'comp/knee', 'comp/makeup'];
  for (const s of deps) watch(p(s), () => c.draw());
  return c;
}

// ------------------------------------------------------------------------------- routing
function pageRouting(body) {
  const id = S.sel;
  const inp = inputById(id), bus = busById(id);
  subTabs('route', []);
  if (inp) {
    const lr = card('MAIN LR', tkey(`${id}/lr`, 'ASSIGN'));
    body.append(lr);
    const mixes = card('MIX SENDS');
    const list = el('div', 'list');
    for (const b of buses().filter(x => x.kind === 'mix' || (x.kind === 'fx' && inp.kind !== 'fxret'))) {
      const n = b.id.split('/')[1];
      const base = b.kind === 'mix' ? `${id}/mix/${n}` : `${id}/fx/${n}`;
      const nm = el('div', 'nm');
      watchLabel(b.id, () => { nm.textContent = displayName(b.id); });
      nm.style.color = KIND_COLOR[b.kind];
      list.append(nm, knob(`${base}/level`, 'LEVEL'), tkey(`${base}/pre`, 'PRE'));
    }
    mixes.append(list);
    body.append(mixes);
    const assign = card('GROUPS · DCA · MUTE GROUPS');
    const r1 = el('div', 'row');
    for (const b of buses().filter(x => x.kind === 'grp')) r1.append(tkey(`${id}/grp/${b.id.split('/')[1]}`, b.name.toUpperCase(), 'assign'));
    const r2 = el('div', 'row');
    for (let d = 1; d <= S.info.dcas; d++) r2.append(tkey(`${id}/dca/${d}`, `DCA ${d}`));
    const r3 = el('div', 'row');
    for (let m = 1; m <= S.info.muteGroups; m++) r3.append(tkey(`${id}/mg/${m}`, `MG ${m}`, 'mute'));
    assign.append(r1, r2, r3);
    body.append(assign);
  } else if (bus) {
    if (bus.kind === 'grp') body.append(card('ROUTE TO LR', tkey(`${id}/lr`, 'ASSIGN')));
    if (bus.kind === 'mtx') {
      const c = card('MATRIX SOURCES (post-fader)');
      const list = el('div', 'list');
      for (const b of buses().filter(x => x.kind === 'lr' || x.kind === 'mix' || x.kind === 'grp')) {
        list.append(el('div', 'nm', displayName(b.id)), knob(`${id}/${b.id}`, 'LEVEL'), el('span'));
      }
      c.append(list);
      body.append(c);
    }
    const outs = card('PATCHED TO OUTPUTS');
    const r = el('div', 'kl');
    const names = [];
    const legs = legNames(bus);
    for (let o = 1; o <= S.info.physOutputs; o++) {
      const src = S.info.outputSources[get(`out/${o}/src`)] || '';
      if (legs.includes(src)) names.push(`Out ${o} ← ${src}`);
    }
    r.textContent = names.length ? names.join(' · ') : 'Not patched to any output (see SETUP).';
    outs.append(r);
    body.append(outs);
  }
}

// Output-patch source names that carry a bus: "Mix 5-6" -> ["Mix 5", "Mix 6"].
function legNames(bus) {
  if (bus.kind === 'lr') return ['LR L', 'LR R'];
  const m = bus.name.match(/^(.*) (\d+)-(\d+)$/);
  return m ? [`${m[1]} ${m[2]}`, `${m[1]} ${m[3]}`] : [bus.name];
}

// ------------------------------------------------------------------------------------ FX
function pageFx(body) {
  const fxBuses = buses().filter(b => b.kind === 'fx');
  const tab = subTabs('fx', fxBuses.map(b => [b.id, b.name.toUpperCase()]));
  const n = tab.split('/')[1];
  const base = `fx/${n}`;
  const typeCard = card('EFFECT TYPE');
  const tr = el('div', 'row');
  S.info.fxTypes.forEach((t, i) => {
    const b = key(t.toUpperCase(), '', () => setParam(`${base}/type`, i));
    watch(`${base}/type`, v => b.classList.toggle('on', v === i));
    b.classList.add('sel');
    tr.append(b);
  });
  typeCard.append(tr);
  body.append(typeCard);
  const params = card('PARAMETERS');
  const pr = el('div', 'row');
  params.append(pr);
  const layouts = {
    0: [['rev/decay', 'DECAY'], ['rev/predelay', 'PRE-DLY'], ['rev/damp', 'DAMP'], ['rev/size', 'SIZE']],
    1: [['dly/time', 'TIME'], ['dly/feedback', 'FEEDBK'], ['dly/hicut', 'HI CUT']],
    2: [['cho/rate', 'RATE'], ['cho/depth', 'DEPTH'], ['cho/delay', 'DELAY']],
    3: [['flg/rate', 'RATE'], ['flg/depth', 'DEPTH'], ['flg/delay', 'DELAY'], ['flg/feedback', 'FEEDBK']],
  };
  watch(`${base}/type`, t => {
    pr.innerHTML = '';
    for (const [s, l] of layouts[t] || []) pr.append(knob(`${base}/${s}`, l));
    if (t === 1) pr.append(tkey(`${base}/dly/pingpong`, 'PING-PONG'));
  });
  body.append(params);
  const levels = card('SEND / RETURN');
  const lr = el('div', 'row');
  lr.append(knob(`fx/${n}/fader`, 'SEND MSTR'), tkey(`fx/${n}/mute`, 'MUTE', 'mute'),
            knob(`fxret/${n}/fader`, 'RETURN'), tkey(`fxret/${n}/mute`, 'MUTE', 'mute'), knob(`fxret/${n}/pan`, 'BAL'));
  levels.append(lr);
  levels.append(el('div', 'kl', 'Tip: select an FX key on the right to put the send levels on the faders.'));
  body.append(levels);
}

// -------------------------------------------------------------------------------- scenes
function pageScenes(body) {
  subTabs('scenes', []);
  const store = card('STORE');
  const r = el('div', 'row');
  const name = el('input', 'txt grow');
  name.placeholder = 'Scene name (letters, numbers, space, - _)';
  name.value = S.scene || '';
  r.append(name, key('STORE', 'sel on', () => { const n = name.value.trim(); if (n) send(`scene save ${n}`); }));
  store.append(r);
  body.append(store);
  const list = card('RECALL');
  if (!S.scenes.length) list.append(el('div', 'kl', 'No scenes stored yet.'));
  for (const s of S.scenes) {
    const row = el('div', 'row');
    row.append(el('div', 'grow nm', (s === S.scene ? '● ' : '') + s), key('RECALL', '', () => {
      if (confirm(`Recall "${s}"? Current settings will be replaced.`)) send(`scene load ${s}`);
    }));
    list.append(row);
  }
  body.append(list);
}

// --------------------------------------------------------------------------------- setup
function pageSetup(body) {
  const tab = subTabs('setup', [['outputs', 'OUTPUT PATCH'], ['names', 'NAMES'], ['info', 'SYSTEM']]);
  if (tab === 'outputs') {
    const c = card('PHYSICAL OUTPUTS');
    const list = el('div', 'list');
    for (let o = 1; o <= S.info.physOutputs; o++) {
      const path = `out/${o}/src`;
      const s = el('select', 'sel');
      S.info.outputSources.forEach((n, i) => { const opt = el('option', '', n); opt.value = i; s.append(opt); });
      s.addEventListener('change', () => setParam(path, +s.value));
      watch(path, v => { s.value = v; });
      list.append(el('div', 'nm', `Output ${o}`), s, el('span'));
    }
    c.append(list);
    body.append(c);
  } else if (tab === 'names') {
    const c = card('NAMES (double-tap a strip name to rename it too)');
    const list = el('div', 'list');
    const keys = [...inputs().map(i => [i.id, i.name]), ...buses().map(b => [b.id, b.name]),
      ...Array.from({ length: S.info.dcas }, (_, i) => [`dca/${i + 1}`, `DCA ${i + 1}`]),
      ...Array.from({ length: S.info.muteGroups }, (_, i) => [`mg/${i + 1}`, `MG ${i + 1}`])];
    for (const [k, def] of keys) {
      const input = el('input', 'txt');
      input.placeholder = def;
      watchLabel(k, t => { if (document.activeElement !== input) input.value = t; });
      input.addEventListener('change', () => send(`label ${k} ${input.value}`));
      list.append(el('div', 'nm', def), input, el('span'));
    }
    c.append(list);
    body.append(c);
  } else {
    const c = card('SYSTEM');
    const i = S.info;
    c.append(el('div', '', `Engine ${i.name} ${i.version} · ${i.backend} · ${i.sampleRate} Hz`),
      el('div', '', `${i.inputs.length} input channels · ${i.buses.length} masters · ${i.physInputs} in / ${i.physOutputs} out`),
      el('div', 'kl', `Control surface: ${location.host}. Any number of tablets can connect at once.`));
    body.append(c);
  }
}

// -------------------------------------------------------------------------------- meters
function pageMeters(body) {
  subTabs('meters', []);
  const mk = (title, items) => {
    const c = card(title);
    const g = el('div', 'mtr-grid');
    for (const [id, get] of items) {
      const cell = el('div', 'mtr-cell');
      const bar = el('div', 'bar');
      const fill = el('i');
      bar.append(fill);
      const nm = el('span');
      watchLabel(id, () => { nm.textContent = displayName(id); });
      cell.append(bar, nm);
      g.append(cell);
      let level = 0;
      onMeters(() => {
        const db = get();
        level = Math.max(Math.min(1, Math.max(0, (db + 60) / 60)), level - 0.03);
        fill.style.height = (level * 100) + '%';
      });
    }
    c.append(g);
    body.append(c);
  };
  mk('INPUTS', inputs().map(i => [i.id, () => inputMeter(i.id).in]));
  mk('MASTERS', buses().map(b => [b.id, () => { const m = busMeter(b.id); return Math.max(m.l, m.r); }]));
}

// =============================================================================== connection
function handleMessage(text) {
  if (text.startsWith('m ')) { S.meters = text.slice(2).split(',').map(Number); return; }
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
    document.getElementById('sceneName').textContent = S.scene ? `Scene: ${S.scene}` : 'No scene';
    if (S.page === 'scenes') renderScreen();
    return;
  }
  let first = !S.ready;
  for (const line of text.split('\n')) {
    if (line.startsWith('p ')) {
      const sp = line.indexOf(' ', 2);
      setLocal(line.slice(2, sp), parseFloat(line.slice(sp + 1)));
    } else if (line.startsWith('l ')) {
      const sp = line.indexOf(' ', 2);
      const k = sp < 0 ? line.slice(2) : line.slice(2, sp);
      S.labels[k] = sp < 0 ? '' : line.slice(sp + 1);
      (labelWatchers.get(k) || []).forEach(f => f(S.labels[k]));
      if (k === S.sel) document.getElementById('selName').textContent = displayName(k);
    } else if (line.startsWith('e ')) {
      console.warn('qnu16:', line.slice(2));
    }
  }
  if (first && S.info && Object.keys(S.params).length) {
    S.ready = true;
    buildAll();
  }
}

function buildAll() {
  const i = S.info;
  document.getElementById('engineInfo').textContent = `${i.backend} · ${i.sampleRate / 1000} kHz`;
  renderScreenTabs();
  renderMixKeys();
  select(S.sel);
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
  if (S.meters && S.ready) for (const list of meterViews.values()) for (const f of list) f(S.meters);
  requestAnimationFrame(animate);
}

document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  if (e.key === 'a' || e.key === 'A') { S.layer = 'A'; renderBank(); }
  if (e.key === 'b' || e.key === 'B') { S.layer = 'B'; renderBank(); }
  if (e.key === 'c' || e.key === 'C') { S.layer = 'C'; renderBank(); }
});

connect();
requestAnimationFrame(animate);
