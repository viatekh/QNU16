// QNU16 web control surface. Talks to qnu16d over a WebSocket (see docs/protocol.md).
'use strict';

const NCH = 16, NAUX = 4, NDCA = 4, NBANDS = 4;
const OUTS = ['main_L', 'main_R', 'aux_1', 'aux_2', 'aux_3', 'aux_4', 'pfl_L', 'pfl_R'];

// ---------- state ----------
const state = {};        // param path -> value
const labels = {};       // label key -> text
const watchers = {};     // param path -> [fn(value)]
const labelWatchers = {};
let ws = null;
let layer = 'main';      // 'main' or aux index '1'..'4' (sends on faders)
let selected = null;     // selected channel number (1-based) for the detail panel
let detailTab = 'eq';
let sampleRate = 48000;
let meters = null;
let currentScene = '';

// Watchers registered while watchTag is set belong to a rebuildable view and can be pruned.
let watchTag = null;
function watch(path, fn) {
  fn = fn.bind(null);  // fresh identity so the same handler can carry different tags
  fn.tag = watchTag;
  (watchers[path] ||= []).push(fn);
  if (path in state) fn(state[path]);
}
function watchLabel(key, fn) {
  fn.tag = watchTag;
  (labelWatchers[key] ||= []).push(fn);
  fn(labels[key] || '');
}
function pruneWatchers(tag) {
  for (const k of Object.keys(watchers)) watchers[k] = watchers[k].filter(fn => fn.tag !== tag);
  for (const k of Object.keys(labelWatchers)) labelWatchers[k] = labelWatchers[k].filter(fn => fn.tag !== tag);
}
function withTag(tag, build) {
  pruneWatchers(tag);
  watchTag = tag;
  try { build(); } finally { watchTag = null; }
}
function setLocal(path, v) {
  state[path] = v;
  (watchers[path] || []).forEach(fn => fn(v));
}
function send(text) { if (ws && ws.readyState === 1) ws.send(text); }

// Throttle outgoing param changes per path to ~60 Hz, always sending the final value.
const pending = new Map();
let flushScheduled = false;
function setParam(path, v) {
  setLocal(path, v);
  pending.set(path, v);
  if (!flushScheduled) {
    flushScheduled = true;
    requestAnimationFrame(() => {
      flushScheduled = false;
      const lines = [];
      for (const [p, val] of pending) lines.push(`set ${p} ${+val.toFixed(4)}`);
      pending.clear();
      if (lines.length) send(lines.join('\n'));
    });
  }
}

// ---------- parameter metadata (mirrors engine/src/mixer.cpp) ----------
function meta(path) {
  const leaf = path.split('/').slice(-2).join('/');
  const last = path.split('/').pop();
  const T = { min: 0, max: 1, toggle: true };
  if (/\/(mute|solo|main|polarity|on|pre)$/.test(path) || /\/dca\/\d$/.test(path)) return T;
  switch (last) {
    case 'fader': case 'send': return { min: -90, max: 10, unit: 'dB', fader: true };
    case 'trim': return { min: -24, max: 24, unit: 'dB', step: 0.5 };
    case 'pan': return { min: -1, max: 1, unit: '' };
    case 'freq': return leaf.startsWith('hpf') ? { min: 20, max: 400, unit: 'Hz', log: true }
                                              : { min: 20, max: 20000, unit: 'Hz', log: true };
    case 'gain': return { min: -15, max: 15, unit: 'dB', step: 0.5 };
    case 'q': return { min: 0.3, max: 10, unit: '', log: true };
    case 'thresh': return path.includes('/gate/') ? { min: -80, max: 0, unit: 'dB' } : { min: -60, max: 0, unit: 'dB' };
    case 'range': return { min: 0, max: 80, unit: 'dB' };
    case 'attack': return path.includes('/gate/') ? { min: 0.05, max: 50, unit: 'ms', log: true }
                                                  : { min: 0.1, max: 100, unit: 'ms', log: true };
    case 'hold': return { min: 0, max: 500, unit: 'ms' };
    case 'release': return path.includes('/gate/') ? { min: 5, max: 2000, unit: 'ms', log: true }
                                                   : { min: 10, max: 2000, unit: 'ms', log: true };
    case 'ratio': return { min: 1, max: 20, unit: ':1', log: true };
    case 'knee': return { min: 0, max: 12, unit: 'dB' };
    case 'makeup': return { min: 0, max: 24, unit: 'dB' };
  }
  return { min: 0, max: 1 };
}

function fmt(v, m) {
  if (m.fader) return v <= -89.9 ? '-∞' : (v > 0 ? '+' : '') + v.toFixed(1);
  if (m.unit === 'Hz') return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + 'k' : Math.round(v) + '';
  if (m.unit === 'ms') return v < 10 ? v.toFixed(1) : Math.round(v) + '';
  if (m.unit === ':1') return v.toFixed(1) + ':1';
  if (m.unit === '' && m.min === -1) return Math.abs(v) < 0.02 ? 'C' : (v < 0 ? 'L' : 'R') + Math.round(Math.abs(v) * 100);
  if (m.unit === '') return v.toFixed(2);
  return (v > 0 && m.min < 0 ? '+' : '') + v.toFixed(1);
}

// Slider position (0..1) <-> value, with log taper where useful.
function toPos(v, m) {
  if (m.log) return Math.log(v / m.min) / Math.log(m.max / m.min);
  return (v - m.min) / (m.max - m.min);
}
function fromPos(p, m) {
  p = Math.min(1, Math.max(0, p));
  let v = m.log ? m.min * Math.pow(m.max / m.min, p) : m.min + p * (m.max - m.min);
  if (m.step) v = Math.round(v / m.step) * m.step;
  return v;
}

// Console-style fader law: more travel around unity, -inf at the bottom.
const TAPER = [[0, -90], [0.04, -60], [0.15, -40], [0.32, -25], [0.5, -12], [0.72, 0], [1, 10]];
function faderPos(db) {
  if (db <= -89.9) return 0;
  for (let i = 1; i < TAPER.length; i++) {
    const [p1, d1] = TAPER[i - 1], [p2, d2] = TAPER[i];
    if (db <= d2) return p1 + (db - d1) / (d2 - d1) * (p2 - p1);
  }
  return 1;
}
function faderDb(pos) {
  pos = Math.min(1, Math.max(0, pos));
  if (pos <= 0.002) return -90;
  for (let i = 1; i < TAPER.length; i++) {
    const [p1, d1] = TAPER[i - 1], [p2, d2] = TAPER[i];
    if (pos <= p2) return d1 + (pos - p1) / (p2 - p1) * (d2 - d1);
  }
  return 10;
}

// ---------- widgets ----------
function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}

function toggleButton(path, text, cls = 'toggle') {
  const b = el('button', 'btn ' + cls, text);
  b.addEventListener('click', () => setParam(path, state[path] >= 0.5 ? 0 : 1));
  watch(path, v => b.classList.toggle('active', v >= 0.5));
  return b;
}

// Vertical fader. pathFn() returns the path currently bound (it changes with the layer).
function makeFader(pathFn, readout) {
  const f = el('div', 'fader');
  const track = el('div', 'track');
  const unity = el('div', 'unity');
  const cap = el('div', 'cap');
  f.append(track, unity, cap);
  const geom = () => { const r = f.getBoundingClientRect(); return { top: r.top + 10, h: r.height - 20 }; };
  const render = v => {
    const g = f.clientHeight - 20;
    cap.style.top = (10 + (1 - faderPos(v)) * g) + 'px';
    unity.style.top = (10 + (1 - faderPos(0)) * g) + 'px';
    if (readout) readout.textContent = fmt(v, { fader: true });
  };
  const set = v => { setParam(pathFn(), v); render(v); };
  let dragging = false, startY = 0, startPos = 0;
  f.addEventListener('pointerdown', e => {
    dragging = true; f.setPointerCapture(e.pointerId);
    startY = e.clientY; startPos = faderPos(state[pathFn()] ?? -90);
    // Tap on the track (not the cap) jumps there; dragging the cap is relative.
    if (e.target !== cap) {
      const g = geom();
      startPos = 1 - (e.clientY - g.top) / g.h;
      set(faderDb(startPos));
    }
  });
  f.addEventListener('pointermove', e => {
    if (!dragging) return;
    const g = geom();
    const fine = e.shiftKey ? 0.25 : 1;
    set(faderDb(startPos - (e.clientY - startY) / g.h * fine));
  });
  const end = () => { dragging = false; };
  f.addEventListener('pointerup', end);
  f.addEventListener('pointercancel', end);
  f.addEventListener('dblclick', () => set(0));
  f.addEventListener('wheel', e => {
    e.preventDefault();
    const cur = state[pathFn()] ?? -90;
    set(Math.min(10, Math.max(-90, (cur <= -89.9 ? -60 : cur) - Math.sign(e.deltaY) * 0.5)));
  }, { passive: false });
  f.render = () => render(state[pathFn()] ?? -90);
  new ResizeObserver(() => f.render()).observe(f);
  return f;
}

function makeMeter(cls = '') {
  const m = el('div', 'meter ' + cls);
  const bar = el('i');
  m.append(bar);
  m.level = 0;
  m.set = frac => {
    m.level = Math.max(frac, m.level - 0.012);  // fast attack, smooth fall
    bar.style.height = (m.level * 100).toFixed(1) + '%';
  };
  new ResizeObserver(() => m.style.setProperty('--mh', m.clientHeight + 'px')).observe(m);
  return m;
}
// dBFS -> meter fraction over a -60..0 dB scale.
const meterFrac = db => Math.min(1, Math.max(0, (db + 60) / 60));

function sliderControl(path, label) {
  const m = meta(path);
  const row = el('div', 'ctl');
  const lab = el('label', '', label);
  const input = el('input');
  input.type = 'range'; input.min = 0; input.max = 1000; input.step = 1;
  const out = el('output');
  input.addEventListener('input', () => setParam(path, fromPos(input.value / 1000, m)));
  input.addEventListener('dblclick', () => setParam(path, m.min < 0 && m.max > 0 ? 0 : m.min));
  watch(path, v => { input.value = Math.round(toPos(v, m) * 1000); out.textContent = fmt(v, m) + (m.unit && !m.fader && m.unit !== ':1' ? ' ' + m.unit : ''); });
  row.append(lab, input, out);
  return row;
}

// ---------- channel strips ----------
const strips = [];
const faders = [];        // channel strip faders
let masterFaders = [];

function layerPath(ch) { return layer === 'main' ? `ch/${ch}/fader` : `ch/${ch}/aux/${layer}/send`; }

function buildStrips() {
  const root = document.getElementById('strips');
  for (let c = 1; c <= NCH; c++) {
    const s = el('div', 'strip');
    const name = el('div', 'name');
    watchLabel(`ch/${c}`, t => { name.textContent = t || `CH ${c}`; });
    name.addEventListener('click', () => rename(`ch/${c}`, `CH ${c}`));
    const gate = el('div', 'gate-led');
    const pan = el('input', 'pan'); pan.type = 'range'; pan.min = -100; pan.max = 100;
    pan.addEventListener('input', () => setParam(`ch/${c}/pan`, pan.value / 100));
    pan.addEventListener('dblclick', () => setParam(`ch/${c}/pan`, 0));
    watch(`ch/${c}/pan`, v => { pan.value = Math.round(v * 100); pan.title = fmt(v, meta('x/pan')); });
    const pre = el('button', 'btn pre', 'PRE');
    pre.addEventListener('click', () => { if (layer !== 'main') { const p = `ch/${c}/aux/${layer}/pre`; setParam(p, state[p] >= 0.5 ? 0 : 1); } });
    const sel = el('button', 'btn sel', 'SEL');
    sel.addEventListener('click', () => select(selected === c ? null : c));
    const zone = el('div', 'faderzone');
    const meter = makeMeter(), gr = makeMeter('gr');
    const db = el('div', 'db');
    const fader = makeFader(() => layerPath(c), db);
    zone.append(meter, gr, fader);
    const row = el('div', 'row');
    row.append(toggleButton(`ch/${c}/mute`, 'MUTE', 'mute'), toggleButton(`ch/${c}/solo`, 'PFL', 'pfl'));
    s.append(name, gate, pan, pre, sel, zone, db, row, el('div', 'num', String(c)));
    root.append(s);
    strips.push({ s, meter, gr, gate, pan, pre, sel, fader });
    faders.push({ fader, path: () => layerPath(c) });
    watch(`ch/${c}/solo`, updateSoloButton);
  }
}

function buildMasters() {
  const root = document.getElementById('masters');
  root.innerHTML = '';
  const mk = (key, title, faderPath, mutePath, meterOuts, extraCls = '') => {
    const s = el('div', 'strip master ' + extraCls);
    const name = el('div', 'name');
    watchLabel(key, t => { name.textContent = t || title; });
    name.addEventListener('click', () => rename(key, title));
    const zone = el('div', 'faderzone');
    const ms = meterOuts.map(() => makeMeter());
    const db = el('div', 'db');
    const fader = makeFader(() => faderPath, db);
    zone.append(...ms, fader);
    s.append(name, zone, db, toggleButton(mutePath, 'MUTE', 'mute'), el('div', 'num', title));
    root.append(s);
    masterFaders.push({ fader, path: () => faderPath });
    return { s, ms, meterOuts };
  };
  masterMeters = [];
  masterFaders = [];
  withTag('masters', () => {
    for (let d = 1; d <= NDCA; d++) mk(`dca/${d}`, `DCA ${d}`, `dca/${d}/fader`, `dca/${d}/mute`, [], 'dca');
    if (layer !== 'main') masterMeters.push(mk(`aux/${layer}`, `AUX ${layer}`, `aux/${layer}/fader`, `aux/${layer}/mute`, [1 + +layer], 'auxmode'));
    masterMeters.push(mk('main', 'MAIN', 'main/fader', 'main/mute', [0, 1]));
  });
}
let masterMeters = [];

function setLayer(l) {
  layer = l;
  document.querySelectorAll('#mixSel button').forEach(b => b.classList.toggle('active', b.dataset.layer === l));
  strips.forEach((st, i) => {
    st.s.classList.toggle('auxmode', l !== 'main');
    st.pre.style.visibility = l === 'main' ? 'hidden' : 'visible';
    st.pre.classList.toggle('active', l !== 'main' && state[`ch/${i + 1}/aux/${l}/pre`] >= 0.5);
    st.fader.render();
  });
  buildMasters();
}

function buildLayerSelector() {
  const nav = document.getElementById('mixSel');
  const add = (l, text, cls) => {
    const b = el('button', cls, text);
    b.dataset.layer = l;
    b.addEventListener('click', () => setLayer(l));
    nav.append(b);
  };
  add('main', 'MAIN', '');
  for (let a = 1; a <= NAUX; a++) add(String(a), `AUX ${a}`, 'aux');
}

function updateSoloButton() {
  const any = Array.from({ length: NCH }, (_, i) => state[`ch/${i + 1}/solo`] >= 0.5).some(Boolean);
  document.getElementById('clearSolo').hidden = !any;
}

function rename(key, fallback) {
  const t = prompt(`Name for ${fallback}`, labels[key] || '');
  if (t !== null) send(`label ${key} ${t}`);
}

// ---------- detail panel ----------
const TABS = [['input', 'INPUT'], ['gate', 'GATE'], ['eq', 'EQ'], ['comp', 'COMP'], ['sends', 'SENDS']];
let eqCanvas = null, grMeter = null, grHold = 0;

function select(c) {
  selected = c;
  strips.forEach((st, i) => { st.s.classList.toggle('selected', i + 1 === c); st.sel.classList.toggle('active', i + 1 === c); });
  const panel = document.getElementById('detail');
  panel.hidden = c == null;
  if (c != null) renderDetail();
  else withTag('detail', () => {});
}

function renderDetail() {
  const c = selected;
  document.getElementById('detailTitle').textContent = `CH ${c}` + (labels[`ch/${c}`] ? ` · ${labels[`ch/${c}`]}` : '');
  const tabs = document.getElementById('detailTabs');
  tabs.innerHTML = '';
  for (const [id, text] of TABS) {
    const b = el('button', 'btn' + (id === detailTab ? ' active' : ''), text);
    b.addEventListener('click', () => { detailTab = id; renderDetail(); });
    tabs.append(b);
  }
  withTag('detail', () => buildDetailBody(c));
}

function buildDetailBody(c) {
  const body = document.getElementById('detailBody');
  body.innerHTML = '';
  eqCanvas = null; grMeter = null;
  const b = `ch/${c}/`;
  const group = (title, togglePath) => {
    const g = el('div', 'group');
    const h = el('h3', '', title);
    if (togglePath) h.append(toggleButton(togglePath, 'ON'));
    g.append(h);
    body.append(g);
    return g;
  };
  if (detailTab === 'input') {
    const g = group('PREAMP');
    g.append(sliderControl(b + 'trim', 'Trim'));
    g.append(toggleButton(b + 'polarity', 'Ø POLARITY'));
    const h = group('HIGH-PASS', b + 'hpf/on');
    h.append(sliderControl(b + 'hpf/freq', 'Freq'));
    const m = group('ROUTING');
    m.append(toggleButton(b + 'main', 'MAIN L/R'));
    const chips = el('div', 'chips');
    for (let d = 1; d <= NDCA; d++) chips.append(toggleButton(`${b}dca/${d}`, `DCA ${d}`));
    m.append(chips);
  } else if (detailTab === 'gate') {
    const g = group('GATE', b + 'gate/on');
    for (const [p, l] of [['thresh', 'Threshold'], ['range', 'Range'], ['attack', 'Attack'], ['hold', 'Hold'], ['release', 'Release']])
      g.append(sliderControl(b + 'gate/' + p, l));
  } else if (detailTab === 'eq') {
    eqCanvas = el('canvas', 'eqcanvas');
    body.append(eqCanvas);
    const names = ['LOW SHELF', 'LOW MID', 'HIGH MID', 'HIGH SHELF'];
    for (let k = 1; k <= NBANDS; k++) {
      const g = group(names[k - 1], k === 1 ? b + 'eq/on' : null);
      g.append(sliderControl(`${b}eq/${k}/freq`, 'Freq'), sliderControl(`${b}eq/${k}/gain`, 'Gain'), sliderControl(`${b}eq/${k}/q`, 'Q'));
      for (const p of ['freq', 'gain', 'q']) watch(`${b}eq/${k}/${p}`, drawEq);
    }
    for (const p of ['eq/on', 'hpf/on', 'hpf/freq']) watch(b + p, drawEq);
    new ResizeObserver(drawEq).observe(eqCanvas);
  } else if (detailTab === 'comp') {
    const g = group('COMPRESSOR', b + 'comp/on');
    for (const [p, l] of [['thresh', 'Threshold'], ['ratio', 'Ratio'], ['attack', 'Attack'], ['release', 'Release'], ['knee', 'Knee'], ['makeup', 'Gain']])
      g.append(sliderControl(b + 'comp/' + p, l));
    const gr = group('GAIN REDUCTION');
    grMeter = el('output', 'db', '0.0 dB');
    gr.append(grMeter);
  } else if (detailTab === 'sends') {
    for (let a = 1; a <= NAUX; a++) {
      const g = group(`AUX ${a}`);
      g.append(sliderControl(`${b}aux/${a}/send`, 'Level'), toggleButton(`${b}aux/${a}/pre`, 'PRE-FADER'));
    }
  }
}

// RBJ biquad magnitude, same formulas as engine/src/dsp/biquad.h.
function biquadMag(type, f0, gainDb, q, f, fs) {
  const A = Math.pow(10, gainDb / 40), w0 = 2 * Math.PI * Math.min(f0, 0.45 * fs) / fs;
  const cs = Math.cos(w0), sn = Math.sin(w0), al = sn / (2 * q), s2 = 2 * Math.sqrt(A) * al;
  let b0, b1, b2, a0, a1, a2;
  if (type === 'peak') { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
  else if (type === 'lowshelf') { b0 = A * ((A + 1) - (A - 1) * cs + s2); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - s2); a0 = (A + 1) + (A - 1) * cs + s2; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - s2; }
  else { b0 = A * ((A + 1) + (A - 1) * cs + s2); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - s2); a0 = (A + 1) - (A - 1) * cs + s2; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - s2; }
  const w = 2 * Math.PI * f / fs, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2w = Math.sin(2 * w);
  const nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2w);
  const dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2w);
  return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

function drawEq() {
  if (!eqCanvas || selected == null) return;
  const dpr = window.devicePixelRatio || 1;
  const W = eqCanvas.clientWidth, H = eqCanvas.clientHeight;
  if (!W || !H) return;
  eqCanvas.width = W * dpr; eqCanvas.height = H * dpr;
  const g = eqCanvas.getContext('2d');
  g.scale(dpr, dpr);
  g.clearRect(0, 0, W, H);
  const xOf = f => Math.log(f / 20) / Math.log(1000) * W;
  const yOf = db => H / 2 - db / 18 * (H / 2 - 8);
  g.strokeStyle = '#262a31'; g.lineWidth = 1; g.fillStyle = '#5c626d'; g.font = '10px system-ui';
  for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
    g.beginPath(); g.moveTo(xOf(f), 0); g.lineTo(xOf(f), H); g.stroke();
    g.fillText(f >= 1000 ? f / 1000 + 'k' : f, xOf(f) + 3, H - 4);
  }
  for (const db of [-12, -6, 0, 6, 12]) {
    g.strokeStyle = db === 0 ? '#3a404a' : '#262a31';
    g.beginPath(); g.moveTo(0, yOf(db)); g.lineTo(W, yOf(db)); g.stroke();
  }
  const b = `ch/${selected}/eq/`;
  const on = state[`ch/${selected}/eq/on`] >= 0.5;
  const types = ['lowshelf', 'peak', 'peak', 'highshelf'];
  const hpfOn = state[`ch/${selected}/hpf/on`] >= 0.5;
  g.beginPath();
  for (let x = 0; x <= W; x += 2) {
    const f = 20 * Math.pow(1000, x / W);
    let db = 0;
    if (on) for (let k = 0; k < NBANDS; k++)
      db += biquadMag(types[k], state[`${b}${k + 1}/freq`], state[`${b}${k + 1}/gain`], state[`${b}${k + 1}/q`], f, sampleRate);
    if (hpfOn) { const r = f / state[`ch/${selected}/hpf/freq`]; db += 10 * Math.log10(Math.pow(r, 4) / (1 + Math.pow(r, 4))); }
    const y = yOf(Math.max(-18, Math.min(18, db)));
    x === 0 ? g.moveTo(x, y) : g.lineTo(x, y);
  }
  g.strokeStyle = on ? '#3fb6ff' : '#5c626d'; g.lineWidth = 2; g.stroke();
}

// ---------- meters ----------
function onMeters(vals) { meters = vals; }
function animate() {
  if (meters) {
    for (let c = 0; c < NCH; c++) {
      const st = strips[c], o = c * 4;
      // In aux layers, show the input level (what feeds the send); in main, post-fader.
      st.meter.set(meterFrac(layer === 'main' ? meters[o + 1] : meters[o]));
      st.gr.set(Math.min(1, meters[o + 2] / 20));
      const gateOn = state[`ch/${c + 1}/gate/on`] >= 0.5;
      st.gate.className = 'gate-led' + (gateOn ? (meters[o + 3] ? ' open' : ' closed') : '');
    }
    const base = NCH * 4;
    for (const m of masterMeters) m.ms.forEach((mm, i) => mm.set(meterFrac(meters[base + m.meterOuts[i]])));
    if (grMeter && selected) {
      // Hold the readout so short reductions (drum hits) stay readable.
      grHold = Math.max(meters[(selected - 1) * 4 + 2], grHold * 0.97);
      grMeter.textContent = (grHold < 0.05 ? '0.0' : '-' + grHold.toFixed(1)) + ' dB';
    }
  }
  requestAnimationFrame(animate);
}

// ---------- scenes ----------
function onSceneList(lines) {
  currentScene = lines[0].slice(2);
  const sel = document.getElementById('sceneList');
  sel.innerHTML = '';
  const names = lines.slice(1).filter(Boolean);
  if (!names.length) sel.append(el('option', '', '(no scenes)'));
  for (const n of names) { const o = el('option', '', n); o.value = n; o.selected = n === currentScene; sel.append(o); }
}

// ---------- connection ----------
function handleMessage(text) {
  if (text.startsWith('m ')) { onMeters(text.slice(2).split(',').map(Number)); return; }
  if (text.startsWith('i ')) { const info = JSON.parse(text.slice(2)); sampleRate = info.sampleRate; return; }
  if (text.startsWith('s ')) { onSceneList(text.split('\n')); return; }
  for (const line of text.split('\n')) {
    if (line.startsWith('p ')) {
      const sp = line.indexOf(' ', 2);
      setLocal(line.slice(2, sp), parseFloat(line.slice(sp + 1)));
    } else if (line.startsWith('l ')) {
      const sp = line.indexOf(' ', 2);
      const key = sp < 0 ? line.slice(2) : line.slice(2, sp);
      labels[key] = sp < 0 ? '' : line.slice(sp + 1);
      (labelWatchers[key] || []).forEach(fn => fn(labels[key]));
    } else if (line.startsWith('e ')) {
      console.warn('qnu16:', line.slice(2));
    }
  }
  faders.forEach(f => f.fader.render());
  masterFaders.forEach(f => f.fader.render());
  if (layer !== 'main') strips.forEach((st, i) => st.pre.classList.toggle('active', state[`ch/${i + 1}/aux/${layer}/pre`] >= 0.5));
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  const conn = document.getElementById('conn'), txt = document.getElementById('connText');
  ws.onopen = () => { conn.classList.add('on'); txt.textContent = location.host; };
  ws.onmessage = e => handleMessage(e.data);
  ws.onclose = () => {
    conn.classList.remove('on'); txt.textContent = 'reconnecting…';
    // Labels from a restarted engine replace ours wholesale.
    for (const k of Object.keys(labels)) delete labels[k];
    setTimeout(connect, 1000);
  };
}

// ---------- boot ----------
document.getElementById('clearSolo').addEventListener('click', () => {
  for (let c = 1; c <= NCH; c++) if (state[`ch/${c}/solo`] >= 0.5) setParam(`ch/${c}/solo`, 0);
});
document.getElementById('sceneLoad').addEventListener('click', () => {
  const n = document.getElementById('sceneList').value;
  if (n && n !== '(no scenes)' && confirm(`Recall scene "${n}"? Current settings will be replaced.`)) send(`scene load ${n}`);
});
document.getElementById('sceneSave').addEventListener('click', () => {
  const n = prompt('Store scene as (letters, numbers, space, - _):', currentScene || 'Scene 1');
  if (n) send(`scene save ${n.trim()}`);
});
document.getElementById('detailClose').addEventListener('click', () => select(null));
document.addEventListener('keydown', e => { if (e.key === 'Escape') select(null); });

buildLayerSelector();
buildStrips();
setLayer('main');
connect();
requestAnimationFrame(animate);
