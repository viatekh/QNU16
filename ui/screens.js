// QNU16 control surface: the 800x480 touch screen and its pages, as on the Qu:
//   Processing (TouchChannel + parameter panels, Fn: Library / Source), Routing (Fn: Mute/DCA),
//   Home (Home, User, Meters, RTA / Spectrogram, Qu-Drive), FX (front and back panel, Fn:
//   Library), Scenes (Scenes, Global Filter, Safes), Setup (Audio, Control, USB Data, I/O Patch,
//   Utility). The status bar at the bottom shows the Fn function, Curr/Next scenes and messages.
'use strict';

// =================================================================================== frame
function refreshScreen(only) {
  if (!S.ready || (only && S.screen !== only)) return;
  for (const b of document.querySelectorAll('[data-screen]')) b.classList.toggle('lit', b.dataset.screen === S.screen);
  const scr = document.getElementById('scr');
  scr.innerHTML = '';
  scr.className = 'scr ' + S.screen;
  let fn = '';
  withTag('screen', () => {
    switch (S.screen) {
      case 'proc': fn = screenProcessing(scr); break;
      case 'route': fn = screenRouting(scr); break;
      case 'home': fn = screenHome(scr); break;
      case 'fx': fn = screenFx(scr); break;
      case 'scenes': fn = screenScenes(scr); break;
      case 'setup': fn = screenSetup(scr); break;
    }
  });
  const label = document.getElementById('fnlabel');
  label.textContent = S.fnOpen ? 'Close' : fn || '';
  label.classList.toggle('on', !!(fn || S.fnOpen));
  document.getElementById('fnkey').classList.toggle('lit', S.fnOpen);
  statusScene();
  statusIcons();
}
function statusScene() {
  const name = n => { const s = S.scenes.find(x => x.n === n); return n ? `${n}) ${s ? s.name : ''}` : ''; };
  document.getElementById('curr').textContent = 'Curr:' + name(S.scene);
  document.getElementById('next').textContent = 'Next:' + name(S.next);
}
function statusIcons() {
  const icons = document.getElementById('icons');
  if (!icons) return;
  icons.innerHTML = '';
  const d = S.drive;
  if (d && d.dropouts) icons.append(el('span', 'errs', `Errors: ${d.dropouts}`));
  if (d && d.state === 'rec') icons.append(el('span', 'ic rec', '●'));
  else if (d && d.state === 'play') icons.append(el('span', 'ic play', '▶'));
  else icons.append(el('span', 'ic usb', '⭍'));
  if (S.params['talk/on'] >= 0.5) icons.append(el('span', 'ic talk', 'T'));
}

// ================================================================================ helpers
function tabs(key, list, opts = {}) {
  const bar = el('div', 'tabs' + (opts.sub ? ' sub' : ''));
  let cur = S.tab[key];
  if (!list.some(t => t[0] === cur && !t[2])) cur = list.find(t => !t[2])[0];
  S.tab[key] = cur;
  for (const [id, text, disabled, dot] of list) {
    const t = el('button', 'tab' + (id === cur ? ' on' : '') + (disabled ? ' dis' : ''), text);
    if (dot) t.append(el('i', 'dot ' + dot));
    if (!disabled) t.addEventListener('click', () => { S.tab[key] = id; S.fnOpen = false; S.focus = null; refreshScreen(); });
    bar.append(t);
  }
  return [bar, cur];
}
function group(title, cls = '') {
  const g = el('div', 'grp ' + cls);
  if (title) g.append(el('div', 'gt', title));
  return g;
}
const div = (cls, ...kids) => { const d = el('div', cls); d.append(...kids.filter(Boolean)); return d; };
function label(text, cls = '') { return el('div', 'lab ' + cls, text); }
// Name box: touch to edit with the keypad (pencil when blank).
function nameBox(id, cls = '') {
  const b = el('button', 'namebox ' + cls);
  watchLabel(id, v => { b.textContent = v || '✎'; b.classList.toggle('blank', !v); });
  b.addEventListener('click', () => rename(id));
  return b;
}

// ================================================================================ Processing
const BAND_COLORS = ['#41c95a', '#2ec4c0', '#4f7dff', '#e04646'];
function screenProcessing(scr) {
  const id = S.sel;
  if (S.fnOpen) {
    if (S.block === 'pre' && isChannel(id)) return sourcePage(scr);
    return libraryPage(scr, id);
  }
  if (isDca(id)) { scr.append(div('empty', label(`${quName(id)}: a DCA has no processing. Use Routing / Fn for the DCA masters.`))); return ''; }
  if (/^fx\//.test(id)) { scr.append(div('empty', label(`${quName(id)} Send: the FX are set up on the FX screen.`), sbtn('Open FX screen', () => { S.tab.fx = 'fx' + num(id); setScreen('fx'); }))); return ''; }
  const fxret = /^fxret\//.test(id);
  const blocks = isChannel(id) ? ['pre', 'gate', 'peq', 'comp'] : fxret ? ['pre', 'peq'] : ['pre', 'peq', 'geq', 'comp'];
  if (!blocks.includes(S.block)) S.block = 'peq';
  // TouchChannel: touch a block to open its panel.
  const tc = el('div', 'touchchannel');
  for (const b of blocks) {
    const cell = el('div', 'tcb ' + b + (S.block === b ? ' on' : ''));
    cell.addEventListener('click', () => {
      const blk = { pre: 'preamp', gate: 'gate', peq: 'peq', geq: 'geq', comp: 'comp' }[b];
      if (b !== 'pre' && editBlock(id, blk)) return;
      if (b === 'pre' && isChannel(id) && editBlock(id, 'preamp')) return;
      S.block = b; S.focus = null; refreshScreen();
    });
    if (b === 'pre') cell.append(...chBlock(id));
    if (b === 'gate') cell.append(...gateBlock(id));
    if (b === 'peq') cell.append(...peqBlock(id));
    if (b === 'geq') cell.append(...geqBlock(id));
    if (b === 'comp') cell.append(...compBlock(id));
    tc.append(cell);
  }
  scr.append(tc);
  const lower = el('div', 'lower ' + S.block);
  scr.append(lower);
  switch (S.block) {
    case 'pre': if (isChannel(id)) panelPreamp(lower, id); else panelMaster(lower, id); break;
    case 'gate': panelGate(lower, id); break;
    case 'peq': panelPeq(lower, id); break;
    case 'geq': panelGeq(lower, id); break;
    case 'comp': panelComp(lower, id); break;
  }
  return S.block === 'pre' && isChannel(id) ? 'Source' : 'Library';
}
function inOut(path) {
  const t = el('span', 'io');
  if (has(path)) watch(path, v => { t.textContent = v >= 0.5 ? 'IN' : 'OUT'; t.classList.toggle('in', v >= 0.5); });
  return t;
}
// TouchChannel CH block: name, gain, source, 48V, polarity, Insert, Delay.
function chBlock(id) {
  const head = el('div', 'tch');
  head.append(el('div', 'tcid', headerName(id)), nameBox(id, 'small'));
  const out = [head];
  if (isChannel(id)) {
    const g = el('div', 'tcgain');
    const dot = el('i', 'gk');
    const val = el('span');
    g.append(dot, val);
    watch(`${id}/gain`, v => { val.textContent = Math.round(v) + 'dB'; });
    watch(`${id}/src`, v => { dot.classList.toggle('usb', v >= 0.5); });
    const src = el('div', 'tcsrc');
    watch(`${id}/src`, v => { src.textContent = v >= 0.5 ? 'Qu-Drv' : 'Local'; });
    const flags = el('div', 'tcflags');
    const pol = el('span', '', 'ø'), ins = el('span', '', 'Insert'), dly = el('span', '', 'Delay'), amm = el('span', '', 'AMM');
    flags.append(el('span', 'na', '48V'), pol, ins, dly);
    watch(`${id}/polarity`, v => pol.classList.toggle('lit', v >= 0.5));
    watch(`${id}/insert/on`, v => ins.classList.toggle('lit', v >= 0.5));
    watch(`${id}/delay/on`, v => dly.classList.toggle('lit', v >= 0.5));
    if (/^ch\//.test(id)) { flags.append(amm); watch(`amm/${num(id)}/on`, v => amm.classList.toggle('lit', v >= 0.5 && on('amm/on'))); }
    out.push(g, src, flags);
  } else if (isMaster(id)) {
    const flags = el('div', 'tcflags col');
    const ins = el('span', '', 'Insert'), dly = el('span', '', 'Delay');
    flags.append(ins, dly);
    watch(`${id}/insert/on`, v => ins.classList.toggle('lit', v >= 0.5));
    watch(`${id}/delay/on`, v => dly.classList.toggle('lit', v >= 0.5));
    out.push(flags);
  }
  return out;
}
function gateBlock(id) {
  const t = el('div', 'tct');
  t.append(el('span', '', 'GATE'), inOut(`${id}/gate/on`));
  return [t, gateMeter(id, true)];
}
// Gate meter with the threshold pointer and gain reduction (darker below -40 dB: noise).
function gateMeter(id, mini) {
  const m = el('div', 'gmeter' + (mini ? ' mini' : ''));
  const fill = el('i', 'fill'), gr = el('i', 'gr'), thr = el('i', 'thr');
  m.append(fill, gr, thr);
  const pos = db => clamp((db + 72) / 90, 0, 1) * 100;  // Qu dB scale -72 .. +18
  watch(`${id}/gate/thresh`, v => { thr.style.bottom = pos(v) + '%'; });
  onFrame(() => {
    const mtr = inMeter(id);
    fill.style.height = pos(mtr.in + 18) + '%';
    fill.classList.toggle('noise', mtr.in + 18 < -40);
    const closed = on(`${id}/gate/on`) && !mtr.gate;
    gr.style.height = closed ? clamp(get(`${id}/gate/depth`) / 60, 0, 1) * 100 + '%' : '0';
  });
  return m;
}
function peqBlock(id) {
  const t = el('div', 'tct');
  const hpf = el('span', 'hpftag', 'HPF');
  if (has(`${id}/hpf/on`)) watch(`${id}/hpf/on`, v => hpf.classList.toggle('in', v >= 0.5)); else hpf.style.display = 'none';
  t.append(el('span', '', 'PEQ'), div('tags', inOut(`${id}/eq/on`), hpf));
  return [t, eqGraph(id, { mini: true })];
}
function geqBlock(id) {
  const t = el('div', 'tct');
  t.append(el('span', '', 'GEQ'), inOut(`${id}/geq/on`));
  const bars = el('div', 'geqmini');
  for (let k = 1; k <= 28; k++) {
    const b = el('i');
    watch(`${id}/geq/${k}`, v => { b.style.height = Math.abs(v) / 12 * 50 + '%'; b.style.bottom = v >= 0 ? '50%' : 50 - Math.abs(v) / 12 * 50 + '%'; });
    bars.append(b);
  }
  return [t, bars];
}
function compBlock(id) {
  const t = el('div', 'tct');
  t.append(el('span', '', 'COMP'), inOut(`${id}/comp/on`));
  return [t, compCurve(id)];
}

// ---- graphs ----
function canvas(cls) {
  const c = el('canvas', cls);
  new ResizeObserver(() => c.draw && c.draw()).observe(c);
  return c;
}
function prep(c) {
  const r = c.getBoundingClientRect();
  if (!r.width) return null;
  const W = c.clientWidth, H = c.clientHeight, dpr = (window.devicePixelRatio || 1) * Math.max(1, scale());
  if (c.width !== Math.round(W * dpr)) { c.width = Math.round(W * dpr); c.height = Math.round(H * dpr); }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  return { g, W, H };
}
// RBJ biquad magnitude, as engine/src/dsp/biquad.h.
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
const bandType = (id, b) => (get(`${id}/eq/${b}/width`) > 1.5 ? (b === 1 ? 'lowshelf' : 'highshelf') : 'peak');

// PEQ graph: the response, band handles in the band colours, the HPF in violet, the optional
// colour fill (Setup / Control / Preferences). Drag a handle: frequency and gain.
function eqGraph(id, opts = {}) {
  const c = canvas('eqg' + (opts.mini ? ' mini' : ''));
  const p = s => `${id}/${s}`;
  const fs = () => S.info.sampleRate;
  const xOf = (f, W) => Math.log(f / 20) / Math.log(1000) * W;
  const fOf = (x, W) => 20 * Math.pow(1000, x / W);
  const yOf = (db, H) => H / 2 - db / 15 * (H / 2 - 6);
  const dbOf = (y, H) => (H / 2 - y) / (H / 2 - 6) * 15;
  const bandDb = (b, f) => {
    const t = bandType(id, b);
    return biquadDb(t, get(p(`eq/${b}/freq`)), get(p(`eq/${b}/gain`)), t === 'peak' ? widthToQ(get(p(`eq/${b}/width`))) : 0.7071, f, fs());
  };
  c.draw = () => {
    const r = prep(c);
    if (!r) return;
    const { g, W, H } = r;
    g.lineWidth = 1;
    g.font = '9px system-ui';
    for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
      g.strokeStyle = '#2a3e57';
      g.beginPath(); g.moveTo(xOf(f, W), 0); g.lineTo(xOf(f, W), H); g.stroke();
      if (!opts.mini) { g.fillStyle = '#9fb3c8'; g.fillText(f >= 1000 ? f / 1000 + 'k' : f, xOf(f, W) + 2, H - 3); }
    }
    for (const db of [-12, -6, 0, 6, 12]) {
      g.strokeStyle = db === 0 ? '#4b6888' : '#2a3e57';
      g.beginPath(); g.moveTo(0, yOf(db, H)); g.lineTo(W, yOf(db, H)); g.stroke();
      if (!opts.mini) { g.fillStyle = '#9fb3c8'; g.fillText(db, 2, yOf(db, H) - 2); }
    }
    const eqOn = has(p('eq/on')) && on(p('eq/on'));
    const hpfOn = has(p('hpf/on')) && on(p('hpf/on'));
    // Colour fill per band.
    if (S.prefs.fill && eqOn) {
      for (let b = 1; b <= 4; b++) {
        g.strokeStyle = BAND_COLORS[b - 1] + '99';
        for (let x = 0; x <= W; x += opts.mini ? 3 : 4) {
          const db = bandDb(b, fOf(x, W));
          if (Math.abs(db) < 0.3) continue;
          g.beginPath(); g.moveTo(x, yOf(0, H)); g.lineTo(x, yOf(clamp(db, -15, 15), H)); g.stroke();
        }
      }
    }
    if (has(p('hpf/on'))) {
      g.beginPath();
      for (let x = 0; x <= W; x += 2) {
        const db = biquadDb('hp', get(p('hpf/freq')), 0, 0.7071, fOf(x, W), fs());
        const y = yOf(clamp(db, -15, 15), H);
        x === 0 ? g.moveTo(x, y) : g.lineTo(x, y);
      }
      g.strokeStyle = hpfOn ? '#c05cff' : '#5b4a6e'; g.lineWidth = 1.5; g.stroke();
    }
    g.beginPath();
    for (let x = 0; x <= W; x += 2) {
      const f = fOf(x, W);
      let db = 0;
      if (eqOn) for (let b = 1; b <= 4; b++) db += bandDb(b, f);
      if (hpfOn) db += biquadDb('hp', get(p('hpf/freq')), 0, 0.7071, f, fs());
      const y = yOf(clamp(db, -15, 15), H);
      x === 0 ? g.moveTo(x, y) : g.lineTo(x, y);
    }
    g.strokeStyle = eqOn ? '#ffe14d' : '#7a7a60'; g.lineWidth = 2; g.stroke();
    for (let b = 1; b <= 4; b++) {
      const x = xOf(get(p(`eq/${b}/freq`)), W), y = yOf(get(p(`eq/${b}/gain`)), H);
      g.strokeStyle = BAND_COLORS[b - 1]; g.lineWidth = 2;
      g.strokeRect(x - 4, y - 4, 8, 8);
    }
  };
  const deps = [...EQ_SUFFIXES, 'hpf/on', 'hpf/freq'].map(p);
  for (const d of deps) if (has(d)) watch(d, () => c.draw());
  if (opts.mini) return c;
  let band = 0;
  c.addEventListener('pointerdown', e => {
    const r = c.getBoundingClientRect();
    let best = 1e9;
    for (let b = 1; b <= 4; b++) {
      const dx = xOf(get(p(`eq/${b}/freq`)), r.width) - (e.clientX - r.left);
      const dy = yOf(get(p(`eq/${b}/gain`)), r.height) - (e.clientY - r.top);
      if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; band = b; }
    }
    if (best > 900 * scale() * scale()) { band = 0; return; }
    c.setPointerCapture(e.pointerId);
  });
  c.addEventListener('pointermove', e => {
    if (!band || !c.hasPointerCapture(e.pointerId)) return;
    const r = c.getBoundingClientRect();
    setParam(p(`eq/${band}/freq`), fOf((e.clientX - r.left) / scale(), r.width / scale()));
    setParam(p(`eq/${band}/gain`), Math.round(dbOf((e.clientY - r.top) / scale(), r.height / scale()) * 10) / 10);
  });
  return c;
}
// Compressor transfer curve (Qu dB scale), live input dot.
function compCurve(id) {
  const c = canvas('ccurve');
  const p = s => `${id}/${s}`;
  c.draw = () => {
    const r = prep(c);
    if (!r) return;
    const { g, W, H } = r;
    const lo = -60, hi = 18, span = hi - lo;
    const xOf = db => (db - lo) / span * W, yOf = db => H - (db - lo) / span * H;
    g.strokeStyle = '#2f3a46'; g.lineWidth = 1;
    for (let db = -40; db <= 0; db += 20) {
      g.beginPath(); g.moveTo(xOf(db), 0); g.lineTo(xOf(db), H); g.stroke();
      g.beginPath(); g.moveTo(0, yOf(db)); g.lineTo(W, yOf(db)); g.stroke();
    }
    const T = get(p('comp/thresh')), R = get(p('comp/ratio')), type = get(p('comp/type'));
    const knee = type === 2 ? 10 : type === 3 ? 0 : on(p('comp/hard')) ? 0 : 6;
    const slope = R >= 49.9 ? -1 : 1 / R - 1;
    const outOf = db => {
      const over = db - T;
      let gr = 0;
      if (knee > 0 && 2 * Math.abs(over) <= knee) gr = slope * Math.pow(over + knee / 2, 2) / (2 * knee);
      else if (over > 0) gr = slope * over;
      return db + gr + get(p('comp/gain'));
    };
    g.beginPath();
    for (let db = lo; db <= hi; db += 0.5) { const y = yOf(clamp(outOf(db), lo, hi)); db === lo ? g.moveTo(xOf(db), y) : g.lineTo(xOf(db), y); }
    g.strokeStyle = on(p('comp/on')) ? '#ffe14d' : '#777'; g.lineWidth = 2; g.stroke();
    const lvl = isInput(id) ? inMeter(id).in + 18 : stripLevel(id) + 18;
    if (lvl > lo) { g.fillStyle = '#ff5050'; g.beginPath(); g.arc(xOf(lvl), yOf(clamp(outOf(lvl), lo, hi)), 3, 0, 7); g.fill(); }
  };
  for (const s of ['comp/on', 'comp/thresh', 'comp/ratio', 'comp/hard', 'comp/gain', 'comp/type']) if (has(p(s))) watch(p(s), () => c.draw());
  onFrame(() => c.draw());
  return c;
}

// ---- lower panels ----
function panelPreamp(host, id) {
  const usb = on(`${id}/src`);
  const left = div('col pre1',
    sbtn('48V (Hold)', () => message('48V: set on the ADA8200 / interface'), 'na'),
    sbtn('Pad', () => message('Pad: not available on this interface'), 'na'),
    pbox(`${id}/gain`, usb ? 'Trim' : 'Gain', { cls: usb ? 'usb' : 'local' }));
  const meter = mbar(() => inMeter(id).in, 'tall');
  const mid = div('col pre2', tbtn(`${id}/polarity`, 'ø', 'ø', { cls: 'pol' }));
  if (/^ch\//.test(id)) {
    const n = num(id), pair = Math.ceil(n / 2);
    const link = sbtn('', () => linkPage(pair), 'link');
    watch(`link/${pair}/on`, v => { link.innerHTML = `${v >= 0.5 ? 'Linked' : 'Not Linked'}<br>CH ${2 * pair - 1}-${2 * pair}`; link.classList.toggle('on', v >= 0.5); });
    mid.append(link);
  }
  const right = div('col pre3', tbtn(`${id}/delay/on`, 'Delay', 'Delay'), pbox(`${id}/delay/time`, 'Delay'));
  // Insert: FX or Ducker, selected with the knob; switched with Insert.
  const ins = group('', 'insert');
  const sel = el('div', 'inssel');
  const fxL = el('span', 'il', 'FX\nInsert'), dkL = el('span', 'il dk', 'Ducker\nInsert');
  const kn = el('div', 'insknob');
  sel.append(fxL, kn, dkL);
  sel.addEventListener('click', () => setParam(`${id}/insert/type`, on(`${id}/insert/type`) ? 0 : 1));
  const insBtn = tbtn(`${id}/insert/on`, 'Insert', 'Insert');
  const info = el('div', 'insinfo');
  const duckSetup = sbtn('Ducker Setup', () => duckerPage(id), 'small');
  watch(`${id}/insert/type`, v => {
    kn.style.transform = `rotate(${v >= 0.5 ? 135 : -45}deg)`;
    fxL.classList.toggle('on', v < 0.5); dkL.classList.toggle('on', v >= 0.5);
    info.innerHTML = '';
    if (v >= 0.5) info.append(duckSetup);
    else {
      const fx = insertedFx(fxTargetIndex(id));
      info.append(el('div', 'small', fx ? `FX${fx} Inserted` : 'No FX patched'));
      if (fx) info.append(sbtn(`FX${fx}`, () => { S.tab.fx = 'fx' + fx; setScreen('fx'); }, 'small'));
    }
  });
  ins.append(sel, div('col', insBtn, info));
  host.append(left, meter, mid, right, ins);
}
function fxTargetIndex(id) {
  const i = inputs().findIndex(x => x.id === id);
  if (i >= 0 && i < FX_TARGET_INPUTS) return i + 1;
  const b = buses().findIndex(x => x.id === id);
  return b >= 0 ? 1 + FX_TARGET_INPUTS + b : 0;
}
function insertedFx(target) {
  for (let f = 1; f <= 4; f++) if (get(`fx/${f}/patch`) === 1 && get(`fx/${f}/src`) === target) return f;
  return 0;
}
// Mix / LR processing: Delay and FX Insert.
function panelMaster(host, id) {
  if (/^fxret\//.test(id)) { host.append(div('empty', label('FX returns have PEQ only: touch PEQ above. FX parameters are on the FX screen.'))); return; }
  const meter = mbar(() => stripLevel(id), 'tall');
  host.append(meter, div('col', tbtn(`${id}/delay/on`, 'Delay', 'Delay'), pbox(`${id}/delay/time`, 'Delay')));
  const fx = insertedFx(fxTargetIndex(id));
  const ins = group('', 'insert');
  ins.append(div('col', label('FX\nInsert', 'il on'), tbtn(`${id}/insert/on`, 'Insert', 'Insert'), el('div', 'small', fx ? `FX${fx} Inserted` : 'No FX patched')));
  host.append(ins);
}
function panelGate(host, id) {
  host.append(gateMeter(id, false),
    div('col', div('row', pbox(`${id}/gate/hold`, 'Hold'), div('col inbtn', label('GATE'), tbtn(`${id}/gate/on`, 'In', 'Out'))),
      div('row', pbox(`${id}/gate/thresh`, 'Thres'), pbox(`${id}/gate/depth`, 'Depth'), pbox(`${id}/gate/attack`, 'Attack'), pbox(`${id}/gate/release`, 'Release'))));
}
function widthIcon(id, b) {
  const i = el('span', 'wicon');
  watch(`${id}/eq/${b}/width`, v => { i.textContent = v > 1.5 ? (b === 1 ? '⌐' : '¬') : '◇'; i.classList.toggle('shelf', v > 1.5); });
  return i;
}
function panelPeq(host, id) {
  if (has(`${id}/hpf/on`)) {
    const h = group('HPF', 'hpfbox');
    h.append(pbox(`${id}/hpf/freq`, ''), tbtn(`${id}/hpf/on`, 'In', 'Out'));
    host.append(h);
  }
  for (let b = 1; b <= 4; b++) {
    const c = el('div', 'band b' + b);
    c.style.borderColor = BAND_COLORS[b - 1];
    const w = pbox(`${id}/eq/${b}/width`, '', { cls: 'w' });
    w.prepend(widthIcon(id, b));
    c.append(w, pbox(`${id}/eq/${b}/freq`, ''), pbox(`${id}/eq/${b}/gain`, ''));
    host.append(c);
  }
  host.append(div('col inbtn', label('PEQ'), tbtn(`${id}/eq/on`, 'In', 'Out')));
  // Graph on top of the panel's left in the full view.
}
function panelGeq(host, id) {
  const sliders = el('div', 'geqs');
  for (let k = 1; k <= 28; k++) {
    const path = `${id}/geq/${k}`;
    const s = el('div', 'gsl');
    const track = el('div', 'gtr'), thumb = el('i', 'gth'), rta = el('i', 'grta');
    track.append(rta, thumb);
    s.append(track, el('div', 'gfl', GEQ_FREQS[k - 1]));
    watch(path, v => { thumb.style.bottom = `calc(${(v + 12) / 24 * 100}% - 4px)`; thumb.classList.toggle('off0', Math.abs(v) > 0.05); });
    s.addEventListener('pointerdown', () => { if (S.hold === 'reset') { setParam(path, 0); return; } focusBox(path, s); });
    dragY(s, () => toPos(path, get(path)), pos => setParam(path, Math.round(fromPos(path, pos) * 2) / 2), 120);
    s.addEventListener('wheel', e => { e.preventDefault(); focusBox(path, s); nudge(path, -Math.sign(e.deltaY)); }, { passive: false });
    onFrame(() => {
      const v = S.rta ? S.rta[k + 1] : -90;
      rta.style.height = clamp((v + 70) / 70, 0, 1) * 100 + '%';
      rta.classList.toggle('pk', rtaPeakIndex() === k + 1);
    });
    sliders.append(s);
  }
  host.append(sliders, div('col inbtn', label('GEQ'), tbtn(`${id}/geq/on`, 'In', 'Out')));
}
function panelComp(host, id) {
  const type = omenu(`${id}/comp/type`);
  const att = pbox(`${id}/comp/attack`, 'Attack'), rel = pbox(`${id}/comp/release`, 'Release');
  watch(`${id}/comp/type`, v => { att.classList.toggle('auto', v >= 2); rel.classList.toggle('auto', v >= 2); });
  const gr = grbar(() => (isInput(id) ? inMeter(id).gr : busMeter(id).gr));
  host.append(div('col', div('row', tbtn(`${id}/comp/hard`, 'Hard Knee', 'Hard Knee'), gr, type, div('col inbtn', label('COMP'), tbtn(`${id}/comp/on`, 'In', 'Out'))),
    div('row', pbox(`${id}/comp/thresh`, 'Thres'), pbox(`${id}/comp/ratio`, 'Ratio'), pbox(`${id}/comp/gain`, 'Gain'), att, rel)));
}

// Stereo linking (CH n <-> n+1) with the link options and Apply / Cancel.
function linkPage(pair) {
  const ov = overlay('linkpg');
  const a = 2 * pair - 1, b = a + 1;
  const local = {};
  for (const o of ['on', 'preamp', 'sc', 'fader', 'pan']) local[o] = on(`link/${pair}/${o}`);
  const title = el('div', 'ovt');
  const draw = () => { title.textContent = `CH${a} <-> CH${b} ${local.on ? 'Linked' : 'Not Linked'}`; };
  draw();
  const opt = (o, text) => {
    const btn = sbtn('', () => { local[o] = !local[o]; paint(); }, 'tog');
    const paint = () => { btn.classList.toggle('on', local[o]); btn.textContent = local[o] ? 'linked' : 'not linked'; };
    paint();
    return div('col', label(text), btn);
  };
  const mainBtn = sbtn('', () => { local.on = !local.on; paintMain(); draw(); }, 'tog big');
  const paintMain = () => { mainBtn.classList.toggle('on', local.on); mainBtn.textContent = local.on ? 'linked' : 'not linked'; };
  paintMain();
  const ch = group('Channel', 'lnk');
  ch.append(label('Insert · HPF · Gate · PEQ · Comp · Delay · Assignments · Pre/Post · Sends'), mainBtn);
  const opts = div('row', group('Preamp'), group('Dynamics'), group('Main'));
  opts.children[0].append(opt('preamp', 'Gain · Source'));
  opts.children[1].append(opt('sc', 'Side Chains'));
  opts.children[2].append(opt('fader', 'Fader · Mute'), opt('pan', 'Pan'));
  const apply = sbtn('Apply', () => {
    // Options first, so linking copies with them.
    for (const o of ['preamp', 'sc', 'fader', 'pan']) setParam(`link/${pair}/${o}`, local[o] ? 1 : 0);
    setParam(`link/${pair}/on`, local.on ? 1 : 0);
    ov.close();
    refreshScreen();
  }, 'apply');
  ov.box.append(title, div('row', ch, opts), div('row end', apply, sbtn('Cancel', () => ov.close(), 'cancel')));
}
// Ducker setup: Single or Gang, trigger, threshold, depth, attack, hold, release.
function duckerPage(id) {
  const ov = overlay('duckpg');
  withTag('overlay', () => {
    const gang = on(`${id}/duck/gang`);
    const base = gang ? 'duck' : `${id}/duck`;
    const single = sbtn('Single', () => { setParam(`${id}/duck/gang`, 0); ov.close(); duckerPage(id); }, 'tog' + (gang ? '' : ' on yel'));
    const gangB = sbtn('Gang', () => { setParam(`${id}/duck/gang`, 1); ov.close(); duckerPage(id); }, 'tog' + (gang ? ' on yel' : ''));
    const trig = group('Trigger Input');
    trig.append(omenu(`${base}/trigger`), pbox(`${base}/thresh`, 'Thres'));
    ov.box.append(el('div', 'ovt', `Ducker Setup ${headerName(id)}`), div('row', single, gangB),
      div('row', trig, pbox(`${base}/depth`, 'Depth'), div('col', pbox(`${base}/attack`, 'Attack'), pbox(`${base}/hold`, 'Hold'), pbox(`${base}/release`, 'Release'))),
      div('row end', sbtn('Close', () => ov.close())));
  });
}

// Preamp Source page (Fn on the preamp): Local or USB per channel; Qu-Drive is the USB source.
function sourcePage(scr) {
  const ch = inputs().filter(i => isChannel(i.id));
  const picked = new Set([S.sel]);
  const grid = el('div', 'srcgrid');
  const btns = ch.map(i => {
    const b = el('button', 'srcb');
    b.append(el('div', 'sn', quName(i.id)), el('div', 'sv'));
    watch(`${i.id}/src`, v => { b.lastChild.textContent = v >= 0.5 ? 'Qu-Drv' : 'Local'; b.classList.toggle('usb', v >= 0.5); });
    b.addEventListener('click', () => { picked.has(i.id) ? picked.delete(i.id) : picked.add(i.id); paint(); });
    grid.append(b);
    return [i.id, b];
  });
  const paint = () => btns.forEach(([id, b]) => b.classList.toggle('pick', picked.has(id)));
  paint();
  const choose = v => { for (const id of picked) setParam(`${id}/src`, v); };
  const src = group('Select Source', 'srcsel');
  src.append(sbtn('Local', () => choose(0), 'loc'), sbtn('dSNAKE', null, 'na'), sbtn('USB', () => choose(1), 'usb'));
  const all = div('col', label('Select'), sbtn('All', () => { ch.forEach(i => picked.add(i.id)); paint(); }), sbtn('None', () => { picked.clear(); paint(); }));
  scr.append(div('page src', grid, div('row', div('col', label('Global USB Source:'), el('div', 'gusb', 'Qu-Drive')), src, all)));
  return '';
}

// ---- Libraries (Fn on the Processing and FX screens) ----
const L = (name, values) => ({ name, values });
const FACTORY = {
  peq: [
    L(':Bell', { 'eq/1/width': 0.5, 'eq/2/width': 0.5, 'eq/3/width': 0.5, 'eq/4/width': 0.5 }),
    L(':Shelf', { 'eq/1/width': 2, 'eq/4/width': 2 }),
    L('Banjo', { 'hpf/on': 1, 'hpf/freq': 120, 'eq/2/freq': 400, 'eq/2/gain': -3, 'eq/3/freq': 3000, 'eq/3/gain': 2 }),
    L('Bass:A', { 'hpf/on': 1, 'hpf/freq': 40, 'eq/1/freq': 90, 'eq/1/gain': 2, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/3/freq': 1500, 'eq/3/gain': 2 }),
    L('Bass:E1', { 'hpf/on': 1, 'hpf/freq': 35, 'eq/1/freq': 70, 'eq/1/gain': 4, 'eq/2/freq': 300, 'eq/2/gain': -4, 'eq/4/freq': 5000, 'eq/4/gain': -3 }),
    L('Bass:E2', { 'hpf/on': 1, 'hpf/freq': 40, 'eq/1/freq': 100, 'eq/1/gain': 3, 'eq/2/freq': 400, 'eq/2/gain': -3, 'eq/3/freq': 2500, 'eq/3/gain': 4 }),
    L('Drm:KK1', { 'hpf/on': 1, 'hpf/freq': 30, 'eq/1/freq': 63, 'eq/1/gain': 5, 'eq/2/freq': 350, 'eq/2/gain': -8, 'eq/2/width': 0.75, 'eq/3/freq': 3500, 'eq/3/gain': 5 }),
    L('Drm:KK2', { 'hpf/on': 1, 'hpf/freq': 40, 'eq/1/freq': 80, 'eq/1/gain': 3, 'eq/2/freq': 450, 'eq/2/gain': -4, 'eq/3/freq': 2000, 'eq/3/gain': 2 }),
    L('Drm:SN1', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/1/freq': 200, 'eq/1/gain': 2, 'eq/2/freq': 600, 'eq/2/gain': -3, 'eq/3/freq': 5000, 'eq/3/gain': 4 }),
    L('Drm:SN2', { 'hpf/on': 1, 'hpf/freq': 80, 'eq/1/freq': 150, 'eq/1/gain': 4, 'eq/2/freq': 800, 'eq/2/gain': -4, 'eq/4/freq': 8000, 'eq/4/gain': 2 }),
    L('Gtr:A1', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/4/freq': 8000, 'eq/4/gain': 2 }),
    L('Gtr:A2', { 'hpf/on': 1, 'hpf/freq': 120, 'eq/2/freq': 300, 'eq/2/gain': -4, 'eq/3/freq': 3000, 'eq/3/gain': 3, 'eq/4/freq': 10000, 'eq/4/gain': 3, 'eq/4/width': 2 }),
    L('Gtr:E1', { 'hpf/on': 1, 'hpf/freq': 90, 'eq/2/freq': 400, 'eq/2/gain': -2, 'eq/3/freq': 2500, 'eq/3/gain': -2 }),
    L('Gtr:E2', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 500, 'eq/2/gain': -3, 'eq/3/freq': 3000, 'eq/3/gain': 3 }),
    L('Horn', { 'hpf/on': 1, 'hpf/freq': 120, 'eq/2/freq': 500, 'eq/2/gain': -2, 'eq/3/freq': 3500, 'eq/3/gain': -2 }),
    L('Keys:1', { 'hpf/on': 1, 'hpf/freq': 60, 'eq/1/freq': 100, 'eq/1/gain': 2, 'eq/4/freq': 8000, 'eq/4/gain': 2 }),
    L('Keys:2', { 'hpf/on': 1, 'hpf/freq': 80, 'eq/2/freq': 300, 'eq/2/gain': -3, 'eq/3/freq': 2500, 'eq/3/gain': 2 }),
    L('Stomp', { 'hpf/on': 1, 'hpf/freq': 40, 'eq/1/freq': 80, 'eq/1/gain': 4, 'eq/3/freq': 2000, 'eq/3/gain': -6 }),
    L('String:1', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 350, 'eq/2/gain': -2, 'eq/4/freq': 10000, 'eq/4/gain': 2, 'eq/4/width': 2 }),
    L('String:2', { 'hpf/on': 1, 'hpf/freq': 120, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/3/freq': 4000, 'eq/3/gain': -2 }),
    L('Vox:1', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 300, 'eq/2/gain': -2, 'eq/3/freq': 3000, 'eq/3/gain': 2 }),
    L('Vox:2M', { 'hpf/on': 1, 'hpf/freq': 90, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/3/freq': 2500, 'eq/3/gain': 2, 'eq/4/freq': 10000, 'eq/4/gain': 2, 'eq/4/width': 2 }),
    L('Vox:3F', { 'hpf/on': 1, 'hpf/freq': 130, 'eq/2/freq': 400, 'eq/2/gain': -2, 'eq/3/freq': 3500, 'eq/3/gain': 2, 'eq/4/freq': 12000, 'eq/4/gain': 2, 'eq/4/width': 2 }),
    L('V:Choir', { 'hpf/on': 1, 'hpf/freq': 150, 'eq/2/freq': 300, 'eq/2/gain': -3, 'eq/3/freq': 3000, 'eq/3/gain': 1 }),
    L('V:Speech', { 'hpf/on': 1, 'hpf/freq': 180, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/3/freq': 2500, 'eq/3/gain': 3, 'eq/4/freq': 8000, 'eq/4/gain': -2 }),
  ],
  gate: [
    L(':Slow', { 'gate/thresh': -40, 'gate/depth': 20, 'gate/attack': 5, 'gate/hold': 200, 'gate/release': 500 }),
    L(':Fast', { 'gate/thresh': -30, 'gate/depth': 30, 'gate/attack': 0.05, 'gate/hold': 50, 'gate/release': 80 }),
    L(':Noise', { 'gate/thresh': -50, 'gate/depth': 10, 'gate/attack': 1, 'gate/hold': 300, 'gate/release': 400 }),
    L('Drum', { 'gate/thresh': -20, 'gate/depth': 40, 'gate/attack': 0.05, 'gate/hold': 80, 'gate/release': 120 }),
  ],
  comp: [
    L(':Gentle', { 'comp/thresh': -10, 'comp/ratio': 2.2, 'comp/type': 1, 'comp/attack': 20, 'comp/release': 250 }),
    L(':Heavy', { 'comp/thresh': -15, 'comp/ratio': 3.2, 'comp/type': 1, 'comp/attack': 10, 'comp/release': 200, 'comp/gain': 4 }),
    L(':Limit', { 'comp/thresh': 6, 'comp/ratio': 50, 'comp/type': 0, 'comp/hard': 1, 'comp/attack': 0.3, 'comp/release': 100 }),
    L('Bass:1', { 'comp/thresh': -12, 'comp/ratio': 3, 'comp/type': 2, 'comp/gain': 3 }),
    L('Bass:2', { 'comp/thresh': -15, 'comp/ratio': 4, 'comp/type': 3, 'comp/gain': 4 }),
    L('Vox:1', { 'comp/thresh': -8, 'comp/ratio': 2.5, 'comp/type': 1, 'comp/attack': 15, 'comp/release': 250, 'comp/gain': 2 }),
    L('Vox:2', { 'comp/thresh': -12, 'comp/ratio': 3.5, 'comp/type': 1, 'comp/attack': 10, 'comp/release': 200, 'comp/gain': 3 }),
    L('Vox:3', { 'comp/thresh': -18, 'comp/ratio': 5, 'comp/type': 0, 'comp/attack': 5, 'comp/release': 150, 'comp/gain': 5 }),
  ],
  geq: [
    L('Wdg:Vox', { 'geq/on': 1, 'geq/7': -3, 'geq/10': -2, 'geq/18': -3, 'geq/20': -4, 'geq/22': -3, 'geq/25': -2 }),
  ],
  channel: [
    L(':Mic', { 'hpf/on': 1, 'hpf/freq': 80 }),
    L(':Line', { 'hpf/on': 0 }),
    L(':DI 48V', { 'hpf/on': 1, 'hpf/freq': 40 }),
    L('Bass:A', { 'hpf/on': 1, 'hpf/freq': 40, 'eq/2/freq': 250, 'eq/2/gain': -3, 'comp/on': 1, 'comp/thresh': -12, 'comp/ratio': 3, 'comp/type': 2 }),
    L('Bass:E', { 'hpf/on': 1, 'hpf/freq': 35, 'eq/1/freq': 80, 'eq/1/gain': 3, 'eq/2/freq': 300, 'eq/2/gain': -4, 'comp/on': 1, 'comp/thresh': -15, 'comp/ratio': 4, 'comp/type': 3 }),
    L('Drm:Hat', { 'hpf/on': 1, 'hpf/freq': 300, 'eq/2/freq': 600, 'eq/2/gain': -3, 'eq/4/freq': 10000, 'eq/4/gain': 2, 'eq/4/width': 2 }),
    L('Drm:KK', { 'hpf/on': 1, 'hpf/freq': 30, 'gate/on': 1, 'gate/thresh': -20, 'gate/depth': 40, 'gate/attack': 0.05, 'gate/hold': 80, 'gate/release': 120, 'eq/1/freq': 63, 'eq/1/gain': 5, 'eq/2/freq': 350, 'eq/2/gain': -8, 'eq/3/freq': 3500, 'eq/3/gain': 5, 'comp/on': 1, 'comp/thresh': -10, 'comp/ratio': 4, 'comp/type': 3 }),
    L('Drm:OH', { 'hpf/on': 1, 'hpf/freq': 150, 'eq/2/freq': 400, 'eq/2/gain': -3, 'eq/4/freq': 10000, 'eq/4/gain': 2, 'eq/4/width': 2 }),
    L('Drm:SN', { 'hpf/on': 1, 'hpf/freq': 100, 'gate/on': 1, 'gate/thresh': -25, 'gate/depth': 20, 'gate/attack': 0.05, 'gate/hold': 100, 'gate/release': 150, 'eq/1/freq': 200, 'eq/1/gain': 2, 'eq/3/freq': 5000, 'eq/3/gain': 4, 'comp/on': 1, 'comp/thresh': -12, 'comp/ratio': 3, 'comp/type': 3 }),
    L('Drm:Tom', { 'hpf/on': 1, 'hpf/freq': 60, 'gate/on': 1, 'gate/thresh': -25, 'gate/depth': 30, 'gate/hold': 150, 'gate/release': 250, 'eq/2/freq': 500, 'eq/2/gain': -5, 'eq/3/freq': 4000, 'eq/3/gain': 3 }),
    L('Gtr:A', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/4/freq': 8000, 'eq/4/gain': 2, 'comp/on': 1, 'comp/thresh': -10, 'comp/ratio': 2.5, 'comp/type': 1 }),
    L('Gtr:E', { 'hpf/on': 1, 'hpf/freq': 90, 'eq/2/freq': 400, 'eq/2/gain': -2, 'eq/3/freq': 2500, 'eq/3/gain': 2 }),
    L('Horn', { 'hpf/on': 1, 'hpf/freq': 120, 'eq/2/freq': 500, 'eq/2/gain': -2, 'comp/on': 1, 'comp/thresh': -8, 'comp/ratio': 2.5, 'comp/type': 1 }),
    L('Keys', { 'hpf/on': 1, 'hpf/freq': 60, 'eq/4/freq': 8000, 'eq/4/gain': 2 }),
    L('String', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 350, 'eq/2/gain': -2 }),
    L('Vox', { 'hpf/on': 1, 'hpf/freq': 100, 'eq/2/freq': 300, 'eq/2/gain': -2, 'eq/3/freq': 3000, 'eq/3/gain': 2, 'comp/on': 1, 'comp/thresh': -12, 'comp/ratio': 3, 'comp/type': 1, 'comp/gain': 3 }),
    L('V:Choir', { 'hpf/on': 1, 'hpf/freq': 150, 'eq/2/freq': 300, 'eq/2/gain': -3, 'comp/on': 1, 'comp/thresh': -10, 'comp/ratio': 2, 'comp/type': 2 }),
    L('V:Speech', { 'hpf/on': 1, 'hpf/freq': 180, 'eq/2/freq': 250, 'eq/2/gain': -3, 'eq/3/freq': 2500, 'eq/3/gain': 3, 'comp/on': 1, 'comp/thresh': -14, 'comp/ratio': 3, 'comp/type': 2, 'comp/gain': 4 }),
  ],
};
// A library recalls its block with every other parameter at its default (so ":Bell" is flat).
const LIB_BLOCKS = { peq: ['hpf', 'peq'], gate: ['gate'], comp: ['comp'], geq: ['geq'], channel: ['hpf', 'gate', 'peq', 'comp', 'delay'] };
function applyLibrary(id, type, values, recallPreamp) {
  const paths = blockPaths(id, LIB_BLOCKS[type].concat(type === 'channel' && recallPreamp ? ['preamp'] : []));
  for (const p of paths) {
    const s = p.slice(id.length + 1);
    if (s.endsWith('/on') && !(s in values) && type !== 'channel') continue;  // keep In keys
    setParam(p, s in values ? values[s] : def(p));
  }
}
function libraryValues(id, type, withPreamp) {
  return blockPaths(id, LIB_BLOCKS[type].concat(withPreamp ? ['preamp'] : []))
    .map(p => `${p.slice(id.length + 1)}=${+get(p).toFixed(4)}`).join(',');
}
const parseValues = s => Object.fromEntries(s.split(',').filter(Boolean).map(kv => { const [k, v] = kv.split('='); return [k, +v]; }));
function libraryPage(scr, id) {
  const types = isChannel(id) ? { gate: 'Gate', peq: 'PEQ', comp: 'Comp' }[S.block] || 'peq' : isMaster(id) ? { geq: 'geq', comp: 'comp' }[S.block] || 'peq' : 'peq';
  const type = typeof types === 'string' && types.length < 5 && types === types.toLowerCase() ? types : types.toLowerCase();
  const st = S.tab.lib || (S.tab.lib = { src: 'factory', pick: null, pre: false });
  const side = el('div', 'col libside');
  const name = el('div', 'namebox lib');
  const listFor = (t, src) => (src === 'factory' ? FACTORY[t].map((l, i) => ({ key: `f:${t}:${i}`, name: l.name, t })) : S.libs.filter(l => l.type === t).map(l => ({ key: `u:${l.n}`, name: l.name, t, n: l.n })));
  const pickedItem = () => { const [kind, a, b] = (st.pick || '').split(':'); return kind === 'f' ? { kind, t: a, i: +b, name: FACTORY[a][+b].name } : kind === 'u' ? { kind, n: +a, t: (S.libs.find(l => l.n === +a) || {}).type, name: (S.libs.find(l => l.n === +a) || {}).name } : null; };
  const recall = () => {
    const it = pickedItem();
    if (!it) return;
    if (it.kind === 'f') { applyLibrary(id, it.t, FACTORY[it.t][it.i].values, st.pre); message(`Recalled ${it.name}`); }
    else libGet(it.n, text => { const vals = parseValues(text.split('|')[1] || ''); applyLibrary(id, it.t, vals, st.pre); message(`Recalled ${it.name}`); });
  };
  const userBtn = sbtn('User', () => { st.src = 'user'; refreshScreen(); }, 'tog' + (st.src === 'user' ? ' on' : ''));
  const facBtn = sbtn('Factory', () => { st.src = 'factory'; refreshScreen(); }, 'tog' + (st.src === 'factory' ? ' on' : ''));
  const it = pickedItem();
  name.textContent = it ? it.name : '';
  name.addEventListener('click', () => { if (it && it.kind === 'u') keypad('Library Name:', it.name, 14, t => t && send(`lib name ${it.n} ${t}`)); });
  side.append(label('Name:'), name, userBtn, facBtn, sbtn('Recall', recall, 'recall'),
    sbtn('Overwrite', () => { if (it && it.kind === 'u') send(`lib store ${it.n} ${it.t} ${it.name}|${libraryValues(id, it.t, it.t === 'channel')}`); }, it && it.kind === 'u' ? '' : 'na'),
    sbtn('Delete', () => { if (it && it.kind === 'u') { send(`lib delete ${it.n}`); st.pick = null; } }, it && it.kind === 'u' ? 'del' : 'na'));
  const listBox = (t, title, extra) => {
    const g = group(title, 'liblist');
    if (extra) g.append(extra);
    const list = el('div', 'plist');
    for (const item of listFor(t, st.src)) {
      const r = el('button', 'pli' + (st.pick === item.key ? ' cur' : ''));
      r.append(el('i', 'licon ' + (st.src === 'factory' ? 'f' : 'u')), document.createTextNode(item.name));
      r.addEventListener('click', () => {
        if (S.hold === 'copy' || S.hold === 'paste') return;
        st.pick = item.key; refreshScreen();
      });
      r.addEventListener('dblclick', recall);
      list.append(r);
    }
    g.append(list, sbtn('Store New', () => keypad('Store New Library:', '', 14, n => n && send(`lib store 0 ${t} ${n}|${libraryValues(id, t, t === 'channel')}`)), 'store'));
    return g;
  };
  const blockType = isChannel(id) ? ({ gate: 'gate', comp: 'comp' }[S.block] || 'peq') : isMaster(id) ? ({ geq: 'geq', comp: 'comp' }[S.block] || 'peq') : 'peq';
  const lists = div('row', listBox(blockType, { peq: 'PEQ Library', gate: 'Gate Library', comp: 'Comp Library', geq: 'GEQ Library' }[blockType]));
  if (isChannel(id)) {
    const rp = sbtn('', () => { st.pre = !st.pre; rp.classList.toggle('on', st.pre); rp.textContent = st.pre ? 'On' : 'Off'; }, 'tog small' + (st.pre ? ' on' : ''));
    rp.textContent = st.pre ? 'On' : 'Off';
    lists.append(listBox('channel', 'Channel Library', div('row', label('Recall Preamp'), rp)));
  }
  scr.append(div('page library', side, lists));
  void type;
  return '';
}

// =================================================================================== Routing
function screenRouting(scr) {
  if (S.fnOpen) return muteDcaPage(scr);
  const id = S.sel;
  if (isDca(id)) { scr.append(div('empty', label('DCA masters: press Fn for the Mute/DCA view.'))); return 'Mute/DCA'; }
  const head = el('button', 'tab name' + (S.tab.route === 'main' ? ' on' : ''));
  head.append(el('div', 'tcid', headerName(id)), el('div', 'nm', userName(id)));
  head.addEventListener('click', () => { S.tab.route = 'main'; refreshScreen(); });
  if (isInput(id)) {
    const fxret = /^fxret\//.test(id);
    const list = [['main', '', true], ['mix', 'Mix Sends'], ['fxs', 'FX Sends', fxret], ['grp', 'Grp Sends', true], ['mutedca', 'Mute/DCA\nGroup Assign']];
    if (S.tab.route !== 'main' || fxret) { const [bar] = tabs('route', list.slice(1)); bar.prepend(head); scr.append(bar); }
    else { const [bar] = tabs('route_', list.slice(1)); bar.querySelectorAll('.tab').forEach(t => t.classList.remove('on')); bar.prepend(head); scr.append(bar); }
    const page = el('div', 'page route');
    scr.append(page);
    switch (fxret && S.tab.route === 'main' ? 'mix' : S.tab.route) {
      case 'main': routeMain(page, id); break;
      case 'mix': routeMixSends(page, id); break;
      case 'fxs': routeFxSends(page, id); break;
      case 'mutedca': routeMuteDca(page, id); break;
    }
  } else {
    const [bar, tab] = tabs('routeM', [['sends', busById(id).kind === 'lr' ? 'LR' : 'Mix Sends'], ['mutedca', 'Mute/DCA\nGroup Assign']]);
    bar.prepend(head);
    scr.append(bar);
    const page = el('div', 'page route');
    scr.append(page);
    if (tab === 'sends') routeMaster(page, id); else routeMuteDca(page, id);
  }
  return 'Mute/DCA';
}
// Main tab: the global direct out settings and this channel's trim.
function routeMain(page, id) {
  const g = group('Global Direct out settings', 'dout');
  g.append(label('Direct out source:'), omenu('dout/src'), label('Follow fader:'), tbtn('dout/fader'), label('Follow mute:'), tbtn('dout/mute'));
  const t = group('Direct out:');
  t.append(pbox(`${id}/dout/trim`, 'Trim'));
  page.append(g, t);
}
function panBar(path) {
  const b = pbox(path, '', { cls: 'pan', format: v => '' });
  const bar = el('div', 'panbar');
  const mark = el('i');
  bar.append(el('span', '', 'L'), mark, el('span', 'r', 'R'));
  b.append(bar);
  watch(path, v => { mark.style.left = (v + 1) / 2 * 100 + '%'; });
  return b;
}
function routeMixSends(page, id) {
  const cols = buses().filter(b => b.kind === 'mix').map(b => b.id).concat(['lr']);
  for (const mix of cols) {
    const c = el('div', 'sendcol');
    c.append(label(quName(mix).replace('LR', 'LR')));
    if (mix === 'lr') {
      c.append(el('div', 'sbsp'), tbtn(`${id}/lr`, 'On', 'Off'), panBar(`${id}/pan`), pbox(`${id}/fader`, 'Level'));
    } else {
      const b = `${id}/mix/${num(mix)}`;
      c.append(tbtn(`${b}/pre`, 'Pre', 'Post', { cls: 'pre' }), tbtn(`${b}/on`, 'On', 'Off'));
      c.append(busById(mix).stereo ? panBar(`${b}/pan`) : el('div', 'sbsp'), pbox(`${b}/level`, 'Level'));
    }
    page.append(c);
  }
}
function routeFxSends(page, id) {
  for (let f = 1; f <= 4; f++) {
    const c = el('div', 'sendcol' + (f > 2 ? ' na' : ''));
    c.append(label(`FX ${f}`));
    if (f <= 2) c.append(tbtn(`${id}/fx/${f}/pre`, 'Pre', 'Post', { cls: 'pre' }), tbtn(`${id}/fx/${f}/on`, 'On', 'Off'), el('div', 'sbsp'), pbox(`${id}/fx/${f}/level`, 'Level'));
    else c.append(el('div', 'small', 'Fed from a Mix: FX / Back panel'));
    page.append(c);
  }
}
function onSoftKey(m) { for (let k = 1; k <= 4; k++) if (get(`soft/${k}/func`) === 2 && get(`soft/${k}/arg`) === m - 1) return true; return false; }
function routeMuteDca(page, id) {
  if (isInput(id)) {
    const d = group('DCA Groups', 'dcas');
    for (let g = 1; g <= 4; g++) {
      const c = el('div', 'col');
      const n = el('div', 'small');
      watchLabel(`dca/${g}`, v => { n.textContent = v || String(g); });
      c.append(n, tbtn(`${id}/dca/${g}`, 'In', 'Out'));
      d.append(c);
    }
    page.append(d);
  }
  const m = group('Mute Groups', 'mgs');
  m.append(el('div', 'small right', 'On SoftKey'));
  for (let g = 1; g <= 4; g++) {
    const n = el('div', 'small');
    watchLabel(`mg/${g}`, v => { n.textContent = v || String(g); });
    m.append(div('row', div('col', n, has(`${id}/mg/${g}`) ? tbtn(`${id}/mg/${g}`, 'In', 'Out') : label('-')), el('span', 'tick', onSoftKey(g) ? '✓' : '')));
  }
  page.append(m);
}
// Mix master routing: assign all, pre-fade all, channel source; output balance.
function routeMaster(page, id) {
  const bus = busById(id);
  const isFx = bus.kind === 'fx';
  const all = (kind, v) => {
    for (const i of inputs()) {
      const p = id === 'lr' ? `${i.id}/lr` : isFx ? `${i.id}/fx/${num(id)}/${kind}` : `${i.id}/mix/${num(id)}/${kind}`;
      if (has(p) && (kind === 'on' || id !== 'lr')) setParam(p, v);
    }
  };
  const g = group(id === 'lr' ? 'LR settings' : 'Mix Send settings', 'mixset');
  const allOn = sbtn('All On', () => all('on', 1)), allOff = sbtn('All Off', () => all('on', 0));
  g.append(label('Assign:'), div('row', allOn, allOff));
  if (id !== 'lr') {
    g.append(label('Pre-Fade:'), div('row', sbtn('All Pre', () => all('pre', 1)), sbtn('All Post', () => all('pre', 0))));
    g.append(label('Channel Source:'), omenu(`${id}/presrc`));
  }
  page.append(g);
  if (bus.stereo) { const b = group('Output Balance:'); b.append(panBar(`${id}/balance`)); page.append(b); }
}
// Mute/DCA view (Fn on Routing): Masters, DCA Group Assign, Mute Group Assign.
function muteDcaPage(scr) {
  const mgDot = Array.from({ length: 4 }, (_, i) => i + 1).some(g => Object.keys(S.params).some(p => p.endsWith(`/mg/${g}`) && on(p)));
  const [bar, tab] = tabs('mutedca', [['masters', 'Masters'], ['dca', 'DCA Group Assign'], ['mg', 'Mute Group Assign', false, mgDot ? 'yel' : '']]);
  scr.append(bar);
  const page = el('div', 'page mutedca');
  scr.append(page);
  if (tab === 'masters') {
    const d = group('DCA Groups', 'dcam');
    for (let g = 1; g <= 4; g++) {
      d.append(div('col dcacol', label(`DCA Grp${g}`), nameBox(`dca/${g}`, 'small'), tbtn(`dca/${g}/mute`, 'Mute', 'Mute', { cls: 'mute' }), pbox(`dca/${g}/fader`, 'Level')));
    }
    const m = group('Mute Groups', 'mgm');
    m.append(el('div', 'small right', 'On SoftKey'));
    for (let g = 1; g <= 4; g++) m.append(div('row', nameBox(`mg/${g}`, 'small'), tbtn(`mg/${g}/mute`, `Mute Grp ${g}`, `Mute Grp ${g}`, { cls: 'mute' }), el('span', 'tick', onSoftKey(g) ? '✓' : '')));
    page.append(d, m);
    return 'Close';
  }
  const kind = tab;
  const [gbar, gtab] = tabs('mutedca_' + kind, [1, 2, 3, 4].map(g => [String(g), kind === 'dca' ? `DCA Grp ${g}` : `Mute Grp ${g}`]), { sub: true });
  page.append(gbar);
  const g = +gtab;
  const pg = S.tab.mdpage || 'inputs';
  const ids = pg === 'inputs' ? inputs().filter(i => i.kind !== 'fxret').map(i => i.id)
    : [...inputs().filter(i => i.kind === 'fxret').map(i => i.id), ...buses().filter(b => b.kind === 'fx').map(b => b.id), ...buses().filter(b => b.kind === 'mix').map(b => b.id), 'lr'];
  const grid = el('div', 'agrid');
  const paths = [];
  for (const id of ids) {
    const p = kind === 'dca' ? `${id}/dca/${g}` : `${id}/mg/${g}`;
    const c = el('div', 'col');
    c.append(label(quName(id) + (/^fx\//.test(id) ? ' Send' : '')));
    if (has(p)) { c.append(tbtn(p, 'In', 'Out')); paths.push(p); } else c.append(label('-'));
    grid.append(c);
  }
  page.append(grid, div('row end',
    sbtn(pg === 'inputs' ? 'Mix ▶' : '◀ Inputs', () => { S.tab.mdpage = pg === 'inputs' ? 'mix' : 'inputs'; refreshScreen(); }),
    div('col', label('All'), sbtn('In / Out', () => { const allIn = paths.every(p => on(p)); paths.forEach(p => setParam(p, allIn ? 0 : 1)); }))));
  return 'Close';
}

// ====================================================================================== Home
function screenHome(scr) {
  const [bar, tab] = tabs('home', [['home', 'Home'], ['user', 'User'], ['meters', 'Meters'], ['rta', 'RTA'], ['drive', 'Qu-Drive']]);
  scr.append(bar);
  const page = el('div', 'page home ' + tab);
  scr.append(page);
  switch (tab) {
    case 'home': {
      const logo = div('col', el('div', 'logo', 'QNU-16'), label(`Firmware: ${S.info.version} · ${S.info.backend} · ${S.info.sampleRate / 1000} kHz`), label(`Current User: ${S.user[0].toUpperCase() + S.user.slice(1)}`));
      const mg = div('col mgcol', ...[1, 2, 3, 4].map(g => tbtn(`mg/${g}/mute`, `Mute Grp ${g}`, `Mute Grp ${g}`, { cls: 'mute' })));
      const sys = div('col', sbtn('Shut Down', () => message('Shut down the host computer to power off safely'), 'shut'), sbtn('Lock Surface', lockSurface, 'lock'));
      page.append(logo, mg, sys);
      return '';
    }
    case 'user': {
      const users = { admin: 'Admin: full access', standard: 'Standard: operation only (no Setup)', basic: 'Basic: faders and mutes, scene recall' };
      const left = group('Select User');
      for (const u of Object.keys(users)) left.append(sbtn(u[0].toUpperCase() + u.slice(1), () => { S.user = u; savePrefs(); renderBank(); refreshScreen(); }, 'tog' + (S.user === u ? ' on yel' : '')));
      page.append(left, group(users[S.user], 'userinfo'));
      return S.user === 'admin' ? 'Permissions' : '';
    }
    case 'meters': metersPage(page); return '';
    case 'rta': return S.fnOpen ? spectroPage(page) : rtaPage(page);
    case 'drive': return drivePage(page);
  }
  return '';
}
function lockSurface() {
  S.locked = true;
  const lock = el('div', 'lockov');
  lock.append(el('div', 'lockmsg', 'Surface Locked'), sbtn('Unlock', () => { S.locked = false; lock.remove(); }, 'apply'));
  document.getElementById('console').append(lock);
}
function metersPage(page) {
  const strip = (id, getDb, cls = '') => div('mcell ' + cls, mbar(getDb), label(quName(id).replace(' Ret', '').replace('Mix', '')));
  const ins = el('div', 'mrow');
  for (const i of inputs()) {
    if (i.kind === 'fxret') continue;
    ins.append(strip(i.id, () => inMeter(i.id).in, i.stereo ? 'st' : ''));
  }
  const outs = el('div', 'mrow');
  for (const i of inputs().filter(x => x.kind === 'fxret')) outs.append(strip(i.id, () => inMeter(i.id).in, 'fxr'));
  for (const b of buses()) {
    if (b.id === 'lr') continue;
    outs.append(strip(b.id, () => { const m = busMeter(b.id); return Math.max(m.l, m.r); }, b.kind));
  }
  outs.append(strip('lr', () => { const m = busMeter('lr'); return Math.max(m.l, m.r); }, 'lr'));
  page.append(ins, outs);
}
const RTA_LABELS = ['20', '25', '31', '40', '50', '63', '80', '100', '125', '160', '200', '250', '315', '400', '500', '630', '800', '1k', '1k2', '1k6', '2k', '2k5', '3k1', '4k', '5k', '6k3', '8k', '10k', '12k', '16k', '20k'];
function rtaPage(page) {
  page.append(label('RTA follows PAFL', 'title'));
  const c = canvas('rta');
  c.draw = () => {
    const r = prep(c);
    if (!r || !S.rta) return;
    const { g, W, H } = r;
    const n = S.rta.length, bw = W / n, peak = rtaPeakIndex();
    g.font = '9px system-ui';
    for (let k = 0; k < n; k++) {
      const v = clamp((S.rta[k] + 80) / 80, 0, 1);
      g.fillStyle = '#2d3742';
      g.fillRect(k * bw + 2, 0, bw - 4, H - 16);
      g.fillStyle = k === peak ? '#ff3b3b' : '#f3d23c';
      g.fillRect(k * bw + 2, (H - 16) * (1 - v), bw - 4, (H - 16) * v);
      g.fillStyle = '#9fb3c8';
      g.fillText(RTA_LABELS[k], k * bw + 2, H - 4);
    }
  };
  onFrame(() => c.draw());
  page.append(c);
  return 'Spectro';
}
// Spectrogram: 8 seconds of RTA frames, colour from dark blue (low) to red (peak).
const spectro = { frames: [], sens: 1 };
function spectroPush() {
  if (!S.rta) return;
  spectro.frames.push(S.rta.slice());
  if (spectro.frames.length > 160) spectro.frames.shift();
}
function spectroPage(page) {
  page.append(label('Spectrogram follows PAFL', 'title'));
  const c = canvas('spectro');
  const color = v => {
    const x = clamp(v, 0, 1);
    const stops = [[0, [10, 10, 80]], [0.35, [0, 120, 255]], [0.55, [0, 220, 120]], [0.75, [255, 230, 0]], [1, [255, 30, 30]]];
    for (let i = 1; i < stops.length; i++) if (x <= stops[i][0]) {
      const [p0, c0] = stops[i - 1], [p1, c1] = stops[i], t = (x - p0) / (p1 - p0);
      return `rgb(${c0.map((a, j) => Math.round(a + (c1[j] - a) * t)).join(',')})`;
    }
    return 'rgb(255,30,30)';
  };
  c.draw = () => {
    const r = prep(c);
    if (!r) return;
    const { g, W, H } = r;
    const fr = spectro.frames, rows = 160, bands = 31;
    const range = [90, 70, 50][spectro.sens];
    for (let t = 0; t < fr.length; t++)
      for (let k = 0; k < bands; k++) {
        g.fillStyle = color((fr[t][k] + range) / range);
        g.fillRect(k * W / bands, H - (fr.length - t) * H / rows, W / bands + 0.5, H / rows + 0.5);
      }
  };
  onFrame(() => c.draw());
  const sens = div('row', label('Sensitivity:'), ...['Low', 'Med', 'High'].map((n, i) => sbtn(n, () => { spectro.sens = i; refreshScreen(); }, 'tog' + (spectro.sens === i ? ' on' : ''))));
  page.append(c, sens);
  return 'RTA';
}
const clock = s => { s = Math.max(0, Math.floor(s)); return `${String(Math.floor(s / 3600)).padStart(2, '0')}:${String(Math.floor(s / 60) % 60).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`; };
function drivePage(page) {
  const [bar, tab] = tabs('drive', [['stereo', 'Stereo'], ['multi', 'Multitrack']], { sub: true });
  page.append(bar);
  const d = () => S.drive || { state: 'stop', mode: 'stereo', seconds: 0, length: 0, file: '' };
  const pick = (prefix, current, done) => pickList(S.driveFiles.filter(f => f.startsWith(prefix)).map(f => ({ text: f, value: f })), current, done, null, 'Recordings');
  S.tab.driveFile = S.tab.driveFile || {};
  const transport = (mode, prefix) => {
    const rec = sbtn('●', () => send(d().state === 'rec' ? 'rec stop' : `rec ${mode}`), 'tr rec');
    const stop = sbtn('■', () => send('rec stop\nplay stop'), 'tr stop');
    const play = sbtn('▶❚❚', () => { const f = S.tab.driveFile[mode]; if (d().state === 'play') send('play stop'); else if (f) send(`play ${f}`); else message('Touch to select a track'); }, 'tr play');
    const prevB = sbtn('|◀', () => { const f = S.tab.driveFile[mode]; if (f) send(`play ${f}`); }, 'tr');
    const nextB = sbtn('▶|', () => {
      const files = S.driveFiles.filter(f => f.startsWith(prefix));
      const i = files.indexOf(S.tab.driveFile[mode]);
      if (i + 1 < files.length) { S.tab.driveFile[mode] = files[i + 1]; refreshScreen(); }
    }, 'tr');
    onFrame(() => {
      const s = d();
      rec.classList.toggle('on', s.state === 'rec' && s.mode === mode);
      play.classList.toggle('on', s.state === 'play' && s.mode === mode);
    });
    return { rec, stop, play, prevB, nextB };
  };
  const times = () => {
    const t = el('div', 'times');
    const e = el('span'), r = el('span');
    t.append(e, r);
    const prog = el('div', 'prog'), fill = el('i');
    prog.append(fill);
    onFrame(() => {
      const s = d();
      e.textContent = 'Elapsed: ' + clock(s.seconds);
      r.textContent = 'Remaining: ' + clock(s.state === 'play' ? s.length - s.seconds : 0);
      fill.style.width = s.state === 'play' && s.length ? clamp(s.seconds / s.length, 0, 1) * 100 + '%' : '0';
    });
    return [t, prog];
  };
  if (tab === 'stereo') {
    const tr = transport('stereo', 'QU-ST');
    const recName = el('div', 'namebox file');
    onFrame(() => { const s = d(); recName.textContent = s.state === 'rec' && s.mode === 'stereo' ? s.file : ''; });
    const recG = group('Stereo Record', 'drv');
    recG.append(recName, mbar(() => { const m = busMeter('lr'); return Math.max(m.l, m.r); }), div('row', tr.stop, tr.play.cloneNode(true), tr.rec));
    recG.querySelector('.tr.play').addEventListener('click', () => send('rec stereo'));
    const playG = group('Stereo Playback', 'drv');
    const file = el('button', 'namebox file', S.tab.driveFile.stereo || 'Touch to select Track');
    file.addEventListener('click', () => pick('QU-ST', S.tab.driveFile.stereo, f => { S.tab.driveFile.stereo = f; refreshScreen(); }));
    const stop2 = sbtn('■', () => send('play stop'), 'tr stop');
    playG.append(file, mbar(() => inMeter('st/3').in), div('row', tr.prevB, stop2, tr.play, tr.nextB));
    page.append(recG, playG, ...times());
    if (!on('st/3/src')) page.append(label('Playback returns on ST3: set its source to USB (Processing / Preamp, Fn)', 'small warn'));
  } else {
    const tr = transport('multi', 'QU-MT');
    const meters = el('div', 'mrow tracks');
    for (let t = 1; t <= 18; t++) {
      const sp = `usb/${t}/src`;
      const src = () => S.info.usbSources[get(sp)] || 'Off';
      meters.append(div('mcell', mbar(() => {
        const s = src();
        if (s.startsWith('CH ')) return inMeter(`ch/${s.slice(3)}`).in;
        if (s.startsWith('ST')) return inMeter(`st/${s[2]}`).in;
        if (s.startsWith('LR')) { const m = busMeter('lr'); return s.endsWith('R') ? m.r : m.l; }
        return -90;
      }), label(src().replace('CH ', ''))));
    }
    const file = el('button', 'namebox file', S.tab.driveFile.multi || 'Touch to select Track');
    file.addEventListener('click', () => pick('QU-MT', S.tab.driveFile.multi, f => { S.tab.driveFile.multi = f; refreshScreen(); }));
    page.append(meters, div('row', file, tr.prevB, tr.stop, tr.play, tr.rec, tr.nextB), ...times());
  }
  return '';
}

// ======================================================================================= FX
const FX_PRESETS = {
  0: [['Arena 1', { 'rev/model': 1, 'rev/decay': 4.5, 'rev/predelay': 40, 'rev/size': 100, 'rev/damp': 60 }],
    ['Arena 2', { 'rev/model': 1, 'rev/decay': 3.5, 'rev/predelay': 30, 'rev/size': 95, 'rev/damp': 50 }],
    ['Chamber Large', { 'rev/model': 0, 'rev/decay': 2.6, 'rev/predelay': 20, 'rev/size': 90 }],
    ['Chamber Mid', { 'rev/model': 0, 'rev/decay': 1.8, 'rev/predelay': 15, 'rev/size': 75 }],
    ['Chamber Small', { 'rev/model': 0, 'rev/decay': 1.1, 'rev/predelay': 10, 'rev/size': 60 }],
    ['EMT Plate', { 'rev/model': 3, 'rev/decay': 2.0, 'rev/predelay': 20, 'rev/damp': 20, 'rev/hfcut': 12000 }],
    ['Hall Concert', { 'rev/model': 1, 'rev/decay': 2.8, 'rev/predelay': 25 }],
    ['Room Studio', { 'rev/model': 2, 'rev/decay': 0.8, 'rev/predelay': 5 }]],
  1: [['Delay 2tap', { 'dly/time': 1110, 'dly/rtime': 1020, 'dly/feedback': 30 }], ['Slapback', { 'dly/time': 110, 'dly/rtime': 110, 'dly/feedback': 0 }],
    ['Quarter 120', { 'dly/time': 500, 'dly/rtime': 500, 'dly/feedback': 35 }], ['Ping Pong', { 'dly/time': 375, 'dly/rtime': 375, 'dly/pingpong': 1, 'dly/feedback': 45 }]],
  2: [['ADT Classic', { 'adt/sep': 25, 'adt/thick': 50 }], ['ADT Slap', { 'adt/sep': 90, 'adt/thick': 30 }]],
  3: [['Chorus Classic', { 'cho/rate': 0.8, 'cho/depth': 3, 'cho/delay': 12 }], ['Chorus Wide', { 'cho/rate': 0.5, 'cho/depth': 5, 'cho/delay': 18 }]],
  4: [['SymphonicVox', { 'sym/rate': 0.4, 'sym/depth': 3 }], ['SymphonicStrings', { 'sym/rate': 0.6, 'sym/depth': 6 }]],
  5: [['Ambient', { 'flg/rate': 0.15, 'flg/depth': 2, 'flg/feedback': 40 }], ['Vintage', { 'flg/rate': 0.3, 'flg/depth': 3, 'flg/feedback': 60 }], ['Wild', { 'flg/rate': 1.2, 'flg/depth': 4, 'flg/feedback': 85 }]],
  6: [['12 Stage', { 'pha/stages': 3, 'pha/rate': 0.4, 'pha/depth': 70, 'pha/feedback': 40 }], ['Slow Swirl', { 'pha/stages': 1, 'pha/rate': 0.1, 'pha/depth': 80, 'pha/feedback': 20 }]],
  7: [['Gated Snare', { 'rev/decay': 3, 'gvb/hold': 250, 'gvb/release': 60, 'gvb/thresh': -20 }], ['Panned', { 'rev/decay': 2.5, 'gvb/hold': 180, 'gvb/release': 40 }], ['Powerbox', { 'rev/decay': 5, 'gvb/hold': 350, 'gvb/release': 80 }]],
};
const FX_GROUPS = [['Reverbs', [0]], ['Delays', [1]], ['Modulators', [2, 3, 4, 5, 6]], ['Gated Verb', [7]], ['Empty Rack', []]];
const FX_PARAMS = {
  0: [['rev/model', 'Model'], ['rev/predelay', 'Predelay'], ['rev/decay', 'Decay Time'], ['rev/damp', 'HF Decay'], ['rev/size', 'Size'], ['rev/lfcut', 'LF Cut'], ['rev/hfcut', 'HF Cut']],
  1: [['dly/time', 'Left Tap'], ['dly/rtime', 'Right Tap'], ['dly/feedback', 'Feedback'], ['dly/hicut', 'Hi Cut'], ['dly/pingpong', 'Ping Pong']],
  2: [['adt/sep', 'Delay Separation'], ['adt/thick', 'Thickness']],
  3: [['cho/rate', 'Rate'], ['cho/depth', 'Depth'], ['cho/delay', 'Delay']],
  4: [['sym/rate', 'Frequency'], ['sym/depth', 'Depth']],
  5: [['flg/rate', 'Rate'], ['flg/depth', 'Depth'], ['flg/delay', 'Delay'], ['flg/feedback', 'Regenerate']],
  6: [['pha/rate', 'Rate'], ['pha/depth', 'Depth'], ['pha/feedback', 'Feedback'], ['pha/stages', 'Stages']],
  7: [['rev/predelay', 'Predelay'], ['gvb/thresh', 'Threshold'], ['gvb/hold', 'Hold'], ['gvb/release', 'Release'], ['rev/decay', 'Decay'], ['rev/lfcut', 'Lo Cut'], ['rev/hfcut', 'Hi Cut']],
};
function screenFx(scr) {
  const [bar, tab] = tabs('fx', [['fx1', 'FX1'], ['fx2', 'FX2'], ['fx3', 'FX3'], ['fx4', 'FX4']]);
  const f = +tab.slice(2);
  const back = !!S.tab.fxBack;
  const side = el('button', 'tab panel on', back ? 'Back panel' : 'Front panel');
  side.addEventListener('click', () => { S.tab.fxBack = !back; refreshScreen(); });
  bar.append(side);
  scr.append(bar);
  const page = el('div', 'page fx');
  scr.append(page);
  if (S.fnOpen) { fxLibrary(page, f); return 'Library'; }
  const b = `fx/${f}`;
  if (back) {
    // Back panel: patch type and input, Apply / Cancel.
    const local = { patch: get(`${b}/patch`), src: get(`${b}/src`) };
    const allowed = v => {
      if (v === 0) return true;
      if (local.patch === 2) return v > FX_TARGET_INPUTS;                   // a bus
      if (local.patch === 3) return v <= FX_TARGET_INPUTS;                  // a channel
      if (local.patch === 1) return v <= FX_TARGET_INPUTS || !/^fx\//.test(buses()[v - 1 - FX_TARGET_INPUTS].id);
      return false;
    };
    const patchBtn = el('button', 'om'), srcBtn = el('button', 'om');
    const paint = () => {
      patchBtn.innerHTML = ''; patchBtn.append(el('span', 'omt', PATCH_TYPES[local.patch]), el('i', 'tri'));
      srcBtn.innerHTML = ''; srcBtn.append(el('span', 'omt', fxTargetName(local.src)), el('i', 'tri'));
    };
    paint();
    patchBtn.addEventListener('click', () => pickList(PATCH_TYPES.map((t, i) => ({ text: t, value: i })), local.patch, v => { local.patch = v; if (!allowed(local.src)) local.src = 0; paint(); }));
    srcBtn.addEventListener('click', () => pickList(S.info.fxTargets.map((_, i) => ({ text: fxTargetName(i), value: i })).filter(x => allowed(x.value)), local.src, v => { local.src = v; paint(); }));
    const rack = div('rack back', el('div', 'rackname', S.info.fxTypes[get(`${b}/type`)]));
    page.append(rack, div('row', patchBtn, div('col', label('Input'), srcBtn)),
      div('row end', sbtn('Apply', () => { setParam(`${b}/patch`, local.patch); setParam(`${b}/src`, local.src); message(`FX${f} ${PATCH_TYPES[local.patch]}`); refreshScreen(); }, 'apply'),
        sbtn('Cancel', () => refreshScreen(), 'cancel')));
    if (local.patch === 1) page.append(label('Switch the insert in with the channel or mix Insert button (Processing / Preamp).', 'small'));
    if (f > 2 && local.patch === 2) page.append(label('Qu-16 FX3 and FX4 have no send bus: feed them from a Mix.', 'small'));
    return 'Library';
  }
  // Front panel: the rack unit with its parameters.
  const type = get(`${b}/type`);
  const rack = el('div', `rack t${type}`);
  const head = div('rackhead', el('div', 'rackname', S.info.fxTypes[type]), el('div', 'rackpatch', `${PATCH_TYPES[get(`${b}/patch`)]}${get(`${b}/src`) ? ' · ' + fxTargetName(get(`${b}/src`)) : ''}`));
  rack.append(head);
  const params = el('div', 'rackparams');
  for (const [s, l] of FX_PARAMS[type]) {
    const p = `${b}/${s}`;
    if (!has(p)) continue;
    params.append(S.meta[p].kind === 't' ? div('col', label(l), tbtn(p)) : S.meta[p].kind === 'h' ? div('col', label(l), omenu(p)) : pbox(p, l));
  }
  rack.append(params);
  page.append(rack);
  if (get(`${b}/patch`) === 1) page.append(div('row', pbox(`${b}/mix`, 'Dry / Wet', { format: v => `D ${100 - Math.round(v)} / W ${Math.round(v)}` })));
  else page.append(div('row', label(`Return: FX${f} Ret strip (upper layer)`, 'small'), sbtn('Return PEQ', () => { select(`fxret/${f}`); S.block = 'peq'; setScreen('proc'); }, 'small')));
  return 'Library';
}
function fxLibrary(page, f) {
  const b = `fx/${f}`;
  const st = S.tab.fxlib || (S.tab.fxlib = { group: 0, src: 'factory', pick: null });
  const side = el('div', 'col libside');
  const types = FX_GROUPS[st.group][1];
  const items = st.src === 'factory'
    ? types.flatMap(t => (FX_PRESETS[t] || []).map(([name, values], i) => ({ key: `f:${t}:${i}`, name, t, values })))
    : S.libs.filter(l => l.type === 'fx').map(l => ({ key: `u:${l.n}`, name: l.name, n: l.n }));
  const picked = items.find(i => i.key === st.pick);
  const recall = () => {
    if (FX_GROUPS[st.group][0] === 'Empty Rack') { setParam(`${b}/patch`, 0); message(`FX${f} Empty Rack`); return; }
    if (!picked) return;
    const apply = (t, values) => {
      setParam(`${b}/type`, t);
      for (const [s] of FX_PARAMS[t]) if (has(`${b}/${s}`)) setParam(`${b}/${s}`, def(`${b}/${s}`));
      for (const [s, v] of Object.entries(values)) if (s !== 'type' && has(`${b}/${s}`)) setParam(`${b}/${s}`, v);
      message(`FX${f} ${picked.name}`);
    };
    if (picked.t != null) apply(picked.t, picked.values);
    else libGet(picked.n, text => { const vals = parseValues(text.split('|')[1] || ''); apply(vals.type || 0, vals); });
  };
  const nameB = el('div', 'namebox lib', picked ? picked.name : '');
  nameB.addEventListener('click', () => { if (picked && picked.n) keypad('Library Name:', picked.name, 14, t => t && send(`lib name ${picked.n} ${t}`)); });
  side.append(label('Name:'), nameB,
    sbtn('User', () => { st.src = 'user'; st.pick = null; refreshScreen(); }, 'tog' + (st.src === 'user' ? ' on' : '')),
    sbtn('Factory', () => { st.src = 'factory'; st.pick = null; refreshScreen(); }, 'tog' + (st.src === 'factory' ? ' on' : '')),
    sbtn('Recall', recall, 'recall'),
    sbtn('Overwrite', () => { if (picked && picked.n) send(`lib store ${picked.n} fx ${picked.name}|${fxValues(b)}`); }, picked && picked.n ? '' : 'na'),
    sbtn('Delete', () => { if (picked && picked.n) { send(`lib delete ${picked.n}`); st.pick = null; } }, picked && picked.n ? 'del' : 'na'));
  const tg = group('FX Type', 'liblist');
  const tl = el('div', 'plist');
  FX_GROUPS.forEach(([n], i) => { const r = el('button', 'pli' + (i === st.group ? ' cur' : ''), n); r.addEventListener('click', () => { st.group = i; st.pick = null; refreshScreen(); }); tl.append(r); });
  tg.append(tl, sbtn('Store New', () => keypad('Store New FX Library:', '', 14, n => n && send(`lib store 0 fx ${n}|${fxValues(b)}`)), 'store'));
  const lg = group(st.src === 'factory' ? 'Factory Library' : 'User Library', 'liblist');
  const ll = el('div', 'plist');
  for (const it of items) { const r = el('button', 'pli' + (st.pick === it.key ? ' cur' : '')); r.append(el('i', 'licon ' + (it.n ? 'u' : 'f')), document.createTextNode(it.name)); r.addEventListener('click', () => { st.pick = it.key; refreshScreen(); }); r.addEventListener('dblclick', recall); ll.append(r); }
  lg.append(ll);
  page.append(side, tg, lg);
}
function fxValues(b) {
  const type = get(`${b}/type`);
  return [`type=${type}`, ...FX_PARAMS[type].map(([s]) => `${s}=${+get(`${b}/${s}`).toFixed(4)}`)].join(',');
}

// =================================================================================== Scenes
const FILTER_LAYOUT = [
  ['In/FX', [['inpatch', 'Input Patch'], ['preamp', 'Preamps'], ['fxpatch', 'FX Patch'], ['chproc', 'CH/ST'], [null, 'Audio Grp'], ['fxparam', 'FX Param'], ['innames', 'Names'], ['infaders', 'Faders/Pan'], ['inmutes', 'Mutes']]],
  ['Out', [['outpatch', 'Output Patch'], [null, ''], [null, ''], ['mixproc', 'Mix'], ['lrproc', 'LR'], [null, 'MTX'], ['mixnames', 'Names'], ['mixfaders', 'Faders/Bal'], ['mixmutes', 'Mutes']]],
  ['', [[null, 'Group Mode'], ['amm', 'AMM'], ['custom', 'Custom Layer'], ['soft', 'SoftKeys'], ['talk', 'Talkback']]],
  ['Routing', [[null, 'Grp Sends'], ['mixsends', 'Mix Sends'], ['fxsends', 'FX Sends'], [null, 'MTX Sends'], ['mutedca', 'Mute/DCA'], ['masters', 'Mute/DCA Grp Masters']]],
];
// Allow/Block grid. isBlocked(cat), set(cat, block).
function filterGrid(title, isBlocked, set) {
  const g = el('div', 'fgrid');
  g.append(el('div', 'ovt', title));
  for (const [gt, items] of FILTER_LAYOUT) {
    const box = group(gt, 'fg');
    for (const [cat, text] of items) {
      const c = el('div', 'col fi');
      c.append(label(text));
      if (!cat) { c.append(sbtn('Allow', null, 'na')); box.append(c); continue; }
      const b = sbtn('', () => { set(cat, !isBlocked(cat)); paint(); });
      const paint = () => { const bl = isBlocked(cat); b.textContent = bl ? 'Block' : 'Allow'; b.classList.toggle('block', bl); };
      paint();
      c.append(b);
      box.append(c);
    }
    g.append(box);
  }
  const allBtn = sbtn('All', () => { const any = S.info.sceneCategories.some(isBlocked); for (const c of S.info.sceneCategories) set(c, !any); refreshScreen(); });
  g.append(div('col fi', label('All'), allBtn));
  return g;
}
function screenScenes(scr) {
  const anyFilter = S.info.sceneCategories.some(c => on(`filter/${c}`));
  const anySafe = Object.keys(S.params).some(p => p.startsWith('safe/') && on(p));
  const [bar, tab] = tabs('scenes', [['scenes', 'Scenes'], ['filter', 'Global Filter', false, anyFilter ? 'red' : ''], ['safes', 'Safes', false, anySafe ? 'blue' : '']]);
  scr.append(bar);
  const page = el('div', 'page scenes');
  scr.append(page);
  if (tab === 'filter') {
    page.append(filterGrid('Recall Filter affects all Scenes', c => on(`filter/${c}`), (c, v) => setParam(`filter/${c}`, v ? 1 : 0)));
    return '';
  }
  if (tab === 'safes') {
    const [sb, st] = tabs('safes', [['inputs', 'Inputs'], ['mix', 'Mix']], { sub: true });
    page.append(sb);
    const ids = st === 'inputs' ? inputs().map(i => i.id) : buses().map(b => b.id).sort((a, b) => (a === 'lr') - (b === 'lr'));
    const grid = el('div', 'agrid');
    for (const id of ids) grid.append(div('col', label(quName(id) + (/^fx\//.test(id) ? ' Send' : '')), tbtn(`safe/${id}`, 'Safe', 'Safe', { cls: 'safe' })));
    page.append(grid, div('row end', div('col', label('All'), sbtn('Safe', () => { const all = ids.every(id => on(`safe/${id}`)); ids.forEach(id => setParam(`safe/${id}`, all ? 0 : 1)); }))));
    return '';
  }
  // Scene list with Filter buttons; Name, Recall, Store, Clear, Reset Mix Settings.
  const list = el('div', 'slist');
  const stored = n => S.scenes.find(s => s.n === n);
  for (let n = 1; n <= S.info.scenes; n++) {
    const s = stored(n);
    const row = el('div', 'srow' + (n === S.next ? ' next' : '') + (n === S.scene ? ' curr' : ''));
    const name = el('button', 'sname');
    name.append(el('span', '', `${n}) ${s ? s.name : ''}`), el('span', 'tick', s ? '✓' : ''));
    name.addEventListener('click', () => {
      if (S.hold === 'copy') { if (s) { S.clip = { type: 'scene', n }; message(`Copied Scene ${n}`); holdUsed(); } return; }
      if (S.hold === 'paste') { if (S.clip && S.clip.type === 'scene') { send(`scene copy ${S.clip.n} ${n}`); message(`Pasted Scene ${n}`); holdUsed(); } return; }
      S.next = n; statusScene(); refreshScreen();
    });
    const fb = sbtn('Filter', () => {
      if (!s) return;
      if (S.hold === 'reset') { for (const c of s.filter) send(`scene filter ${n} ${c} 0`); holdUsed(); return; }
      if (S.hold === 'copy') { S.clip = { type: 'filter', cats: s.filter.slice() }; message(`Copied Scene ${n} Filter`); holdUsed(); return; }
      if (S.hold === 'paste' && S.clip && S.clip.type === 'filter') {
        for (const c of S.info.sceneCategories) send(`scene filter ${n} ${c} ${S.clip.cats.includes(c) ? 1 : 0}`);
        holdUsed();
        return;
      }
      const ov = overlay('filterpg');
      ov.box.append(filterGrid(`Scene Recall Filter: Scene ${n}`, c => (stored(n) || { filter: [] }).filter.includes(c), (c, v) => {
        send(`scene filter ${n} ${c} ${v ? 1 : 0}`);
        const sc = stored(n); if (sc) { if (v && !sc.filter.includes(c)) sc.filter.push(c); if (!v) sc.filter = sc.filter.filter(x => x !== c); }
      }), div('row end', sbtn('Close', () => ov.close())));
    }, 'filt' + (s ? '' : ' na'));
    if (s && s.filter.length) fb.append(el('i', 'dot red'));
    row.append(name, fb);
    list.append(row);
  }
  const side = el('div', 'col sside');
  const cur = stored(S.next);
  const nm = el('button', 'namebox', cur ? cur.name : '');
  nm.addEventListener('click', () => keypad('Scene Name:', cur ? cur.name : '', S.info.sceneNameChars || 14, t => t && send(`scene name ${S.next} ${t}`)));
  const recall = sbtn('Recall', () => { if (cur) send(`scene recall ${S.next}`); }, 'recall' + (cur ? '' : ' na'));
  const store = sbtn('Store', () => send(`scene store ${S.next}`), 'store');
  const clear = sbtn('Clear', () => { if (cur) send(`scene clear ${S.next}`); }, 'clear' + (cur ? '' : ' na'));
  const reset = sbtn('Reset Mix\nSettings', null, 'reset');
  holdPress(reset, () => { send('scene reset'); message('Reset Mix Settings'); });
  side.append(label('Name:'), nm, recall, store, div('row', clear, div('col', label('(hold)'), reset)));
  page.append(list, side);
  requestAnimationFrame(() => { const r = list.querySelector('.next'); if (r) r.scrollIntoView({ block: 'center' }); });
  S.focus = { path: null, el: null, turn: d => { const n = clamp(S.next + d, 1, S.info.scenes); if (n !== S.next) { S.next = n; statusScene(); refreshScreen(); } } };
  return '';
}

// ===================================================================================== Setup
function screenSetup(scr) {
  const [bar, tab] = tabs('setup', [['audio', 'Audio'], ['control', 'Control'], ['usbdata', 'USB Data'], ['io', 'I/O Patch'], ['utility', 'Utility']]);
  scr.append(bar);
  const subs = {
    audio: [['pafl', 'PAFL'], ['talk', 'Talkback'], ['siggen', 'SigGen'], ['amm', 'AMM']],
    control: [['custom', 'Custom Layer'], ['soft', 'SoftKeys'], ['network', 'Network'], ['midi', 'MIDI'], ['prefs', 'Preferences']],
    usbdata: [['scenes', 'Scenes'], ['libs', 'Libraries'], ['shows', 'Shows']],
    io: [['outputs', 'Output Patch'], ['usb', 'USB Audio']],
    utility: [['engine', 'Engine'], ['drive', 'Qu-Drive']],
  }[tab];
  const [sbar, sub] = tabs('setup_' + tab, subs, { sub: true });
  scr.append(sbar);
  const page = el('div', 'page setup ' + sub);
  scr.append(page);
  switch (sub) {
    case 'pafl': {
      page.append(div('col', div('row', label('Additive'), tbtn('pafl/additive')), group('', 'afl')),
        div('col', div('row', label('Sel follow PAFL'), tbtn('pafl/selfollow')), div('row', label('PAFL follow Mix'), tbtn('pafl/mixfollow'))),
        div('col', div('row', label('LR to PAFL'), tbtn('pafl/lr'))));
      page.querySelector('.afl').append(div('row', label('Output AFL'), tbtn('pafl/outafl')), div('row', label('Input AFL'), tbtn('pafl/inafl')));
      const p = group('PAFL');
      p.append(div('row', pbox('pafl/delay', 'Delay'), pbox('pafl/trim', 'PFL Trim')));
      page.append(p);
      return '';
    }
    case 'talk': {
      if (S.fnOpen) {
        page.append(group('Talkback options', 'opts'));
        page.querySelector('.opts').append(label('Momentary Latch'), tbtn('talk/latch'), label('Hold the Talk button for momentary Talkback or tap the Talk button to latch Talkback.', 'small'));
        return 'Close';
      }
      const h = group('HPF');
      h.append(tbtn('talk/hpf', 'In', 'Out'), pbox('talk/hpffreq', 'Freq'));
      const g = div('col', label('Source'), omenu('talk/src'), mbar(() => { const s = get('talk/src'); return s > 0 && s <= 16 ? inMeter(`ch/${s}`).in : -90; }), pbox('talk/gain', 'Gain'));
      page.append(h, g, assignGrid('talk'));
      return 'Options';
    }
    case 'siggen': {
      page.append(div('col', omenu('gen/type'), tbtn('gen/mute', 'Mute', 'Mute', { cls: 'mute' }), div('row', pbox('gen/freq', 'Freq'), pbox('gen/level', 'Level'))), assignGrid('gen'));
      if (on('talk/on')) page.append(label('The generator is off while Talk is active.', 'small warn'));
      return '';
    }
    case 'amm': return ammPage(page);
    case 'custom': return customPage(page);
    case 'soft': return softPage(page);
    case 'network': page.append(group('Network', 'info'), label(`This desk: ${location.host} · ${S.info.backend} backend`, 'small')); page.querySelector('.info').append(label(`Unit Name: ${S.info.name}`), label('DHCP and addresses are set in the host OS.')); return '';
    case 'midi': page.append(label('MIDI and MMC are not available on QNU16.', 'small')); return '';
    case 'prefs': {
      const fill = sbtn('', () => { S.prefs.fill = !S.prefs.fill; savePrefs(); refreshScreen(); }, 'tog' + (S.prefs.fill ? ' on' : ''));
      fill.textContent = S.prefs.fill ? 'On' : 'Off';
      const follow = sbtn('', () => { S.prefs.follow = !S.prefs.follow; savePrefs(); refreshScreen(); }, 'tog' + (S.prefs.follow ? ' on' : ''));
      follow.textContent = S.prefs.follow ? 'On' : 'Off';
      page.append(div('col', label('PEQ Graph Fill'), fill, label('Processing Screens Follow Surface'), follow));
      return '';
    }
    case 'scenes': case 'libs': case 'shows':
      page.append(label('Scenes, libraries and recordings are files on the engine host (--scenes, --libraries, --recordings). Copy those folders to back up or move a show.', 'small'));
      page.append(div('col', label(`${S.scenes.length} scenes · ${S.libs.length} user libraries · ${S.driveFiles.length} recordings`)));
      return '';
    case 'outputs': return patchPage(page, 'out', S.info.physOutputs, 'Output');
    case 'usb': return patchPage(page, 'usb', 18, 'Track');
    case 'engine':
      page.append(group('Engine', 'info'));
      page.querySelector('.info').append(label(`${S.info.name} ${S.info.version}`), label(`Backend: ${S.info.backend}`), label(`Sample rate: ${S.info.sampleRate} Hz`), label(`I/O: ${S.info.physInputs} in, ${S.info.physOutputs} out`));
      return '';
    case 'drive':
      page.append(group('Qu-Drive', 'info'));
      page.querySelector('.info').append(label('Recordings: QU-STnnn.WAV (stereo) and QU-MTnnn/TRK01-18.WAV (multitrack), 24-bit.'), label(`${S.driveFiles.length} recordings on the drive.`));
      return '';
  }
  return '';
}
// Talkback and signal generator assigns: Mix 1-4, 5-6, 7-8, 9-10, LR.
function assignGrid(base) {
  const g = el('div', 'agrid small');
  for (const b of buses().filter(x => x.kind === 'mix')) g.append(div('col', label(quName(b.id)), tbtn(`${base}/mix/${num(b.id)}`)));
  g.append(div('col', label('LR'), tbtn(`${base}/lr`)));
  return g;
}
function ammPage(page) {
  if (S.fnOpen) {
    const g = group('AMM Setup', 'opts');
    g.append(label('Sidechain Filter:'), div('row', pbox('amm/hpf', 'HPF'), pbox('amm/lpf', 'LPF')), label('Follow Fader:'), tbtn('amm/follow'));
    page.append(g);
    return 'Close';
  }
  const half = S.tab.ammHalf || 0;
  const block = el('button', 'ammblock');
  for (let c = 1; c <= 16; c++) {
    const i = el('i');
    onFrame(() => { const v = S.amm ? S.amm[c - 1] : -90; i.style.height = on(`amm/${c}/on`) ? clamp((v + 25) / 30, 0, 1) * 100 + '%' : '0'; });
    block.append(i);
  }
  block.addEventListener('click', e => { const r = block.getBoundingClientRect(); S.tab.ammHalf = e.clientX - r.left > r.width / 2 ? 1 : 0; refreshScreen(); });
  page.append(div('row', div('ammlogo', el('b', '', 'AMM'), el('span', '', 'Auto Mic Mixer')), block, div('col', label('AMM'), tbtn('amm/on', 'In', 'Out'))));
  const row = el('div', 'ammch');
  for (let c = 1 + half * 8; c <= 8 + half * 8; c++) {
    const cell = el('div', 'ammc');
    const prio = pbox(`amm/${c}/prio`, 'Prio');
    const g = el('div', 'mbar amm'), fill = el('i', 'fill');
    g.append(fill);
    onFrame(() => { const v = S.amm ? S.amm[c - 1] : -90; fill.style.height = on(`amm/${c}/on`) && on('amm/on') ? clamp((v + 25) / 30, 0, 1) * 100 + '%' : '0'; });
    const name = el('div', 'small');
    watchLabel(`ch/${c}`, v => { name.textContent = v || ''; });
    cell.append(div('row', el('span', 'cn', String(c)), tbtn(`amm/${c}/on`, 'ON', 'OFF')), div('row', prio, div('col', g, label('Gain'))), name);
    row.append(cell);
  }
  page.append(row);
  return 'Setup';
}
function customPage(page) {
  const grid = el('div', 'agrid custom');
  for (let i = 1; i <= S.info.customStrips; i++) {
    const p = `custom/${i}/strip`;
    const b = el('button', 'custb');
    const n = el('div', 'cn', String(i)), v = el('div', 'cv');
    b.append(n, v);
    watch(p, x => { v.textContent = customStripNames()[x]; b.classList.toggle('set', x > 0); });
    b.addEventListener('click', () => {
      if (S.focus && S.focus.path === p) { pickList(customStripNames().map((t, j) => ({ text: t, value: j })), get(p), x => setParam(p, x)); return; }
      focusBox(p, b);
    });
    if (S.focus && S.focus.path === p) { S.focus.el = b; b.classList.add('focus'); }
    grid.append(b);
  }
  page.append(grid, div('row', tbtn('custom/only', 'Custom layer only', 'Custom layer only')));
  return '+1';
}
// Fn +1 on the custom layer and patch pages: the next button gets the next source.
function plusOne() {
  const f = S.focus;
  if (!f || !f.path) return;
  const m = f.path.match(/^(custom|out|usb)\/(\d+)\/(strip|src)$/);
  if (!m) return;
  const next = `${m[1]}/${+m[2] + 1}/${m[3]}`;
  if (!has(next)) return;
  setParam(next, Math.min(S.meta[next].max, get(f.path) + 1));
  S.focus = { path: next, el: null };
  refreshScreen();
}
function softPage(page) {
  const k = +(S.tab.softKey || 1);
  const left = el('div', 'col softsel');
  for (let i = 1; i <= S.info.softKeys; i++) left.append(sbtn(`Soft ${i}`, () => { S.tab.softKey = i; S.tab.softLocal = null; refreshScreen(); }, 'tog' + (i === k ? ' on red' : '')));
  const local = S.tab.softLocal || (S.tab.softLocal = { func: get(`soft/${k}/func`), arg: get(`soft/${k}/arg`) });
  const funcBtn = el('button', 'om');
  funcBtn.append(el('span', 'omt', SOFT_FUNCS[local.func]), el('i', 'tri'));
  funcBtn.addEventListener('click', () => pickList(SOFT_FUNCS.map((t, i) => ({ text: t, value: i })), local.func, v => { local.func = v; local.arg = 0; refreshScreen(); }));
  const argItems = {
    1: customStripNames().map((t, i) => ({ text: t, value: i })).slice(1),
    2: [1, 2, 3, 4].map(g => ({ text: `Mute Grp ${g}`, value: g - 1 })),
    3: [1, 2, 3, 4].map(g => ({ text: `DCA ${g}`, value: g - 1 })),
    4: [1, 2, 3, 4].map(g => ({ text: `FX ${g}`, value: g - 1 })),
    5: [1, 2, 3, 4].map(g => ({ text: `FX ${g}`, value: g - 1 })),
    6: [1, 2, 3, 4].map(g => ({ text: `FX ${g}`, value: g - 1 })),
    7: Array.from({ length: 100 }, (_, i) => ({ text: `Scene ${i + 1}`, value: i })),
  }[local.func];
  const right = div('col', label('Function'), funcBtn);
  if (argItems) {
    const ab = el('button', 'om');
    ab.append(el('span', 'omt', (argItems.find(x => x.value === local.arg) || { text: '-' }).text), el('i', 'tri'));
    ab.addEventListener('click', () => pickList(argItems, local.arg, v => { local.arg = v; refreshScreen(); }));
    right.append(ab);
  }
  right.append(div('row end', sbtn('Apply', () => { setParam(`soft/${k}/func`, local.func); setParam(`soft/${k}/arg`, local.arg); S.tab.softLocal = null; message(`Soft ${k}: ${SOFT_FUNCS[local.func]}`); refreshScreen(); }, 'apply'),
    sbtn('Cancel', () => { S.tab.softLocal = null; refreshScreen(); }, 'cancel')));
  page.append(left, right);
  return '';
}
// Output Patch and USB Audio: a button per output/track; touch, then the rotary or the list.
function patchPage(page, base, count, word) {
  const grid = el('div', 'agrid patch');
  for (let i = 1; i <= count; i++) {
    const p = `${base}/${i}/src`;
    const b = el('button', 'custb');
    const v = el('div', 'cv');
    b.append(el('div', 'cn', String(i)), v);
    watch(p, x => { v.textContent = choiceLabels(p)[x]; b.classList.toggle('set', x > 0); });
    b.addEventListener('click', () => {
      if (S.focus && S.focus.path === p) { pickList(choiceLabels(p).map((t, j) => ({ text: t, value: j })), get(p), x => setParam(p, x)); return; }
      focusBox(p, b);
    });
    if (S.focus && S.focus.path === p) { S.focus.el = b; b.classList.add('focus'); }
    grid.append(b);
  }
  page.append(grid);
  if (base === 'usb') page.append(div('row', label('Qu-Drive Multitrack = 1-18 · Qu-Drive Stereo records LR'), label('CH/ST source:'), omenu('usb/chsrc')));
  else page.append(label(`${word}s are the interface's physical outputs (1-2 LR, 3-12 Mix 1-10, 13-14 Alt out = PAFL).`, 'small'));
  return '+1';
}

// The Fn key on pages where it is "+1".
const fnBase = () => {
  const fn = document.getElementById('fnlabel');
  return fn && fn.textContent === '+1';
};
document.addEventListener('click', e => {
  const t = e.target.closest && e.target.closest('#fnkey');
  if (t && S.fnOpen && fnBase()) { S.fnOpen = false; plusOne(); }
}, true);
