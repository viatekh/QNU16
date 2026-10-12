// QNU16 control surface: the physical panel, laid out as the Qu-16 top panel.
//   Top: SuperStrip | touch screen with its screen keys, Fn, Copy/Paste/Reset and rotary | meters,
//        Qu-Drive, engineer's monitor
//   Bottom: Pre Fade/Assign and layer keys | 16 fader strips | master strip | SoftKeys and Mix keys
'use strict';

// =========================================================================== processing blocks
const EQ_SUFFIXES = ['eq/on', ...[1, 2, 3, 4].flatMap(b => [`eq/${b}/freq`, `eq/${b}/gain`, `eq/${b}/width`])];
const BLOCKS = {
  preamp: ['gain', 'polarity'],
  hpf: ['hpf/on', 'hpf/freq'],
  gate: ['gate/on', 'gate/thresh', 'gate/depth', 'gate/attack', 'gate/hold', 'gate/release'],
  peq: EQ_SUFFIXES,
  geq: ['geq/on', ...Array.from({ length: 28 }, (_, i) => `geq/${i + 1}`)],
  comp: ['comp/on', 'comp/thresh', 'comp/ratio', 'comp/attack', 'comp/release', 'comp/hard', 'comp/gain', 'comp/type'],
  delay: ['delay/on', 'delay/time'],
};
const BLOCK_NAMES = { preamp: 'Preamp', hpf: 'HPF', gate: 'Gate', peq: 'PEQ', geq: 'GEQ', comp: 'Comp', delay: 'Delay', proc: 'Proc' };
// Sel + Copy/Paste/Reset: inputs HPF, Gate, PEQ, Comp, Delay; masters PEQ, GEQ, Comp, Delay.
function procBlocks(id) { return isChannel(id) ? ['hpf', 'gate', 'peq', 'comp', 'delay'] : isMaster(id) ? ['peq', 'geq', 'comp', 'delay'] : ['peq']; }
function blockPaths(id, blocks) {
  return blocks.flatMap(b => BLOCKS[b]).map(s => `${id}/${s}`).filter(has);
}
function copyBlock(id, block) {
  const blocks = block === 'proc' ? procBlocks(id) : [block];
  S.clip = { type: 'proc', block, kind: kindOf(id), values: blockPaths(id, blocks).map(p => [p.slice(id.length + 1), get(p)]) };
  message(`Copied ${quName(id)} ${BLOCK_NAMES[block]}`);
}
function pasteBlock(id, block) {
  const c = S.clip;
  if (!c || c.type !== 'proc' || c.block !== block) { message('Nothing to paste'); return; }
  for (const [s, v] of c.values) if (has(`${id}/${s}`)) setParam(`${id}/${s}`, v);
  message(`Pasted ${quName(id)} ${BLOCK_NAMES[block]}`);
}
function resetBlock(id, block) {
  const blocks = block === 'proc' ? procBlocks(id) : [block];
  for (const p of blockPaths(id, blocks)) setParam(p, def(p));
  message(`Reset ${quName(id)} ${BLOCK_NAMES[block]}`);
}
// Copy/Paste/Reset held: returns true if it handled the press of a block key.
function editBlock(id, block) {
  if (S.hold === 'copy') { copyBlock(id, block); holdUsed(); return true; }
  if (S.hold === 'paste') { pasteBlock(id, block); holdUsed(); return true; }
  if (S.hold === 'reset') { resetBlock(id, block); holdUsed(); return true; }
  return false;
}

// Mix sends (Mix key + Copy/Paste/Reset): send levels, pans and assigns, not pre/post.
function sendPaths(mix) {
  const out = [];
  for (const i of inputs()) {
    const base = mix === 'lr' ? null : busById(mix).kind === 'fx' ? `${i.id}/fx/${num(mix)}` : `${i.id}/mix/${num(mix)}`;
    const list = base ? [`${base}/level`, `${base}/on`, `${base}/pan`] : [`${i.id}/fader`, `${i.id}/lr`, `${i.id}/pan`];
    for (const p of list) if (has(p)) out.push(p);
  }
  return out;
}
function editMix(mix) {
  const tail = p => p.split('/').slice(-1)[0];
  if (S.hold === 'copy') {
    S.clip = { type: 'sends', values: sendPaths(mix).map(p => [p.split('/').slice(0, 2).join('/'), tail(p), get(p)]) };
    message(`Copied ${quName(mix)} Sends`);
  } else if (S.hold === 'paste') {
    if (!S.clip || S.clip.type !== 'sends') { message('Nothing to paste'); return true; }
    const targets = sendPaths(mix);
    for (const [src, t, v] of S.clip.values) {
      const p = targets.find(x => x.startsWith(src + '/') && tail(x) === t || (t === 'fader' && x === `${src}/fader`));
      if (p) setParam(p, v);
    }
    message(`Pasted ${quName(mix)} Sends`);
  } else if (S.hold === 'reset') {
    for (const p of sendPaths(mix)) setParam(p, def(p));
    message(`Reset ${quName(mix)} Sends`);
  } else {
    return false;
  }
  holdUsed();
  return true;
}

// ===================================================================== hold keys (latching)
// Pre Fade, Assign, Copy, Paste, Reset and Fn act while held; a short press latches them
// (so they work with a mouse), and another press releases.
let holdState = { key: null, t: 0, used: false, latched: false };
function holdUsed() { holdState.used = true; }
function holdKey(name, b) {
  b.addEventListener('pointerdown', () => {
    if (S.hold === name && holdState.latched) { setHold(null); return; }
    holdState = { key: name, t: performance.now(), used: false, latched: false };
    setHold(name);
  });
  b.addEventListener('pointerup', () => {
    if (S.hold !== name || holdState.latched) return;
    // Held and used: release. Short press: latch.
    if (holdState.used || performance.now() - holdState.t > 600) setHold(null);
    else holdState.latched = true;
  });
}
function setHold(name) {
  S.hold = name;
  document.body.dataset.hold = name || '';
  for (const b of document.querySelectorAll('[data-holdkey]')) b.classList.toggle('lit', b.dataset.holdkey === name);
}
window.addEventListener('keydown', e => { if (e.key === 'Escape' && S.hold) setHold(null); });

// =========================================================================== layers and mixes
const UPPER = () => ['st/1', 'st/2', 'st/3', 'fxret/1', 'fxret/2', 'fxret/3', 'fxret/4', 'fx/1', 'fx/2',
  'mix/1', 'mix/2', 'mix/3', 'mix/4', 'mix/5', 'mix/6', 'mix/7'];
function layerIds(layer) {
  if (layer === 'lower') return Array.from({ length: 16 }, (_, i) => `ch/${i + 1}`);
  if (layer === 'upper') return UPPER();
  return Array.from({ length: 16 }, (_, i) => customStripId(get(`custom/${i + 1}/strip`) || 0));
}
const GEQ_FREQS = ['31', '40', '50', '63', '80', '100', '125', '160', '200', '250', '315', '400', '500', '630', '800', '1k',
  '1k25', '1k6', '2k', '2k5', '3k15', '4k', '5k', '6k3', '8k', '10k', '12k5', '16k'];
const flipBand = k => (S.flip === 1 ? k + 1 : k + 13);  // fader k (0-based) -> GEQ band
const canFlip = () => S.mix === 'lr' || /^mix\//.test(S.mix);

// The parameter a strip's fader moves.
function stripFader(id) {
  if (!id) return null;
  if (isDca(id)) return `dca/${num(id)}/fader`;
  if (S.mix === 'lr' || !isInput(id)) return `${id}/fader`;
  const p = busById(S.mix).kind === 'fx' ? `${id}/fx/${num(S.mix)}/level` : `${id}/mix/${num(S.mix)}/level`;
  return has(p) ? p : null;
}
// The Assign / Pre Fade parameter for a strip in the selected mix.
function assignPath(id) {
  if (!isInput(id)) return null;
  if (S.mix === 'lr') return `${id}/lr`;
  const p = busById(S.mix).kind === 'fx' ? `${id}/fx/${num(S.mix)}/on` : `${id}/mix/${num(S.mix)}/on`;
  return has(p) ? p : null;
}
function prePath(id) {
  if (!isInput(id) || S.mix === 'lr') return null;
  const p = busById(S.mix).kind === 'fx' ? `${id}/fx/${num(S.mix)}/pre` : `${id}/mix/${num(S.mix)}/pre`;
  return has(p) ? p : null;
}
// Muted by its own key and/or by a mute group or a muted DCA: 0 none, 1 own, 2 group, 3 both.
function muteState(id) {
  if (isDca(id)) return on(`dca/${num(id)}/mute`) ? 1 : 0;
  if (!has(`${id}/mute`)) return 0;
  let group = false;
  for (let m = 1; m <= 4; m++) if (on(`${id}/mg/${m}`) && on(`mg/${m}/mute`)) group = true;
  for (let d = 1; d <= 4; d++) if (on(`${id}/dca/${d}`) && on(`dca/${d}/mute`)) group = true;
  return (on(`${id}/mute`) ? 1 : 0) + (group ? 2 : 0);
}

function setMix(id) {
  if (id !== 'lr' && S.mix === id) id = 'lr';  // pressing the active Mix key returns to LR
  // PAFL follow Mix: the master strip's PAFL moves with the Mix keys.
  if (on('pafl/mixfollow') && S.mix !== id && on(`${S.mix}/pafl`)) setParam(`${id}/pafl`, 1);
  S.mix = id;
  if (!canFlip()) S.flip = 0;
  if (S.sel === 'lr' || busById(S.sel)) S.sel = id;
  renderBank();
  renderMaster();
  renderSuperStrip();
  refreshScreen();
}
function select(id) {
  if (!id) return;
  S.sel = id;
  S.focus = null;
  if (isDca(id)) S.sel = id;
  updateSelKeys();
  renderSuperStrip();
  if (S.screen === 'proc' || S.screen === 'route' || (S.screen === 'fx' && /^fx/.test(id))) refreshScreen();
}

// ===================================================================================== build
function buildSurface() {
  const c = document.getElementById('console');
  c.innerHTML = '';
  for (const id of ['superstrip', 'screenpanel', 'rightpanel', 'mixassign', 'layerkeys', 'bank', 'masterstrip', 'softkeys', 'mixselect'])
    c.append(Object.assign(el('section', id), { id }));
  renderScreenPanel();
  renderRightPanel();
  renderMixAssign();
  renderBank();
  renderMaster();
  renderSoftKeys();
  renderSuperStrip();
  refreshScreen();
  fit();
}

// ================================================================================ SuperStrip
function renderSuperStrip() {
  const host = document.getElementById('superstrip');
  host.innerHTML = '';
  withTag('superstrip', () => {
    const id = S.sel;
    const p = s => (has(`${id}/${s}`) ? `${id}/${s}` : null);
    const section = (cls, title) => { const s = el('div', 'ss ' + cls); s.append(el('div', 'sst', title)); host.append(s); return s; };
    const inKey = (path, block, text = 'In') => {
      const b = pkey(text, 'white small', () => { if (!path) return; if (editBlock(id, block)) return; toggle(path); panelTouched(path); });
      if (path) watch(path, v => b.classList.toggle('lit', v >= 0.5)); else b.classList.add('na');
      return b;
    };
    const knobs = [];
    const knob = (pathFn, label, cls) => { const k = rotary(pathFn, label, { cls }); knobs.push(k); return k; };

    // Preamp: USB select key and Gain (Trim when the source is USB).
    const pre = section('pre', 'Preamp');
    const usb = pkey('', 'white small usb', () => { const s = p('src'); if (!s) return; if (editBlock(id, 'preamp')) return; toggle(s); });
    usb.querySelector('.kt').innerHTML = '<svg viewBox="0 0 24 12"><path d="M2 6h16M14 2l4 4-4 4M6 6l3-4h4M9 6l3 4h3" fill="none" stroke="currentColor" stroke-width="1.4"/></svg>';
    if (p('src')) watch(p('src'), v => usb.classList.toggle('lit', v >= 0.5)); else usb.classList.add('na');
    const gainPk = el('i', 'pkled');
    pre.append(usb, knob(() => p('gain'), 'Gain'), gainPk);

    // HPF.
    const hpf = section('hpf', 'HPF');
    hpf.append(knob(() => p('hpf/freq'), ''), inKey(p('hpf/on'), 'hpf'));

    // Parametric EQ: Width / Freq / Gain rows for LF, LM, HM, HF; In key between LM and HM.
    const eq = section('peq', 'Parametric EQ');
    const grid = el('div', 'eqgrid');
    for (const row of ['width', 'freq', 'gain'])
      for (let b = 1; b <= 4; b++) grid.append(knob(() => p(`eq/${b}/${row}`), row === 'width' ? 'Width' : row === 'freq' ? 'Freq' : 'Gain'));
    const bands = el('div', 'eqbands');
    bands.append(el('span', '', 'LF'), el('span', '', 'LM'), inKey(p('eq/on'), 'peq'), el('span', '', 'HM'), el('span', '', 'HF'));
    const eqPk = el('i', 'pkled');
    eq.append(eqPk, grid, bands);

    // Gate: threshold, GR LED, In.
    const gate = section('gate', 'Gate');
    const gateGr = el('i', 'grled');
    gate.append(knob(() => p('gate/thresh'), 'Thres'), el('div', 'ledl', 'GR'), gateGr, inKey(p('gate/on'), 'gate'));
    // Comp: threshold, GR LED, In.
    const comp = section('comp', 'Comp');
    const compGr = el('i', 'grled');
    comp.append(knob(() => p('comp/thresh'), 'Thres'), el('div', 'ledl', 'GR'), compGr, inKey(p('comp/on'), 'comp'));

    // GEQ: Fader Flip.
    const geq = section('geq', 'GEQ');
    const flip = pkey('Fader Flip', 'white small', () => {
      if (!canFlip()) { message('Select LR or a Mix for GEQ Fader Flip'); return; }
      S.flip = (S.flip + 1) % 3;
      renderBank();
      flip.classList.toggle('lit', S.flip > 0);
    });
    flip.classList.toggle('lit', S.flip > 0);
    geq.append(el('i', 'pkled'), flip);

    // Pan: depends on the selected mix (LR main pan, stereo mix send pan, FX and mono mixes off).
    const pan = section('pan', 'Pan');
    const panPath = () => {
      if (!isInput(id)) return p('balance');
      if (S.mix === 'lr') return p('pan');
      const b = busById(S.mix);
      return b && b.kind === 'mix' && b.stereo ? p(`mix/${num(S.mix)}/pan`) : null;
    };
    const arc = el('div', 'panleds');
    const panLeds = Array.from({ length: 11 }, () => { const i = el('i'); arc.append(i); return i; });
    const panKnob = knob(panPath, '');
    pan.append(arc, panKnob, el('div', 'lr', 'L'), el('div', 'rr', 'R'));

    // Physical controls follow the selection.
    const deps = ['gain', 'src', 'hpf/freq', 'gate/thresh', 'comp/thresh', 'pan', 'balance', ...EQ_SUFFIXES]
      .map(s => `${id}/${s}`).concat(S.mix !== 'lr' && isInput(id) ? [`${id}/mix/${num(S.mix)}/pan`] : []);
    const renderAll = () => knobs.forEach(k => k.render());
    for (const d of deps) if (has(d)) watch(d, renderAll);
    renderAll();
    onFrame(() => {
      const pp = panPath();
      const v = pp ? get(pp) : null;
      panLeds.forEach((l, i) => l.classList.toggle('on', v != null && Math.abs((i - 5) / 5 - v) < 0.11));
      const m = isInput(id) ? inMeter(id) : null;
      gainPk.classList.toggle('on', !!m && m.in >= -3);
      eqPk.classList.toggle('on', stripLevel(id) >= -3);
      gateGr.classList.toggle('on', !!m && p('gate/on') && on(p('gate/on')) && !m.gate);
      const gr = m ? m.gr : busById(id) ? busMeter(id).gr : 0;
      compGr.classList.toggle('on', gr >= 1);
    });
  });
}

// ============================================================================= screen panel
const SCREEN_KEYS = [['proc', 'Processing', 'sel'], ['route', 'Routing', 'sel'], ['home', 'Home'], ['fx', 'FX'], ['scenes', 'Scenes'], ['setup', 'Setup']];
function renderScreenPanel() {
  const host = document.getElementById('screenpanel');
  host.innerHTML = '';
  const title = el('div', 'tstitle', 'Touch Screen');
  const display = el('div', 'display');
  display.id = 'display';
  const scr = el('div', 'scr');
  scr.id = 'scr';
  const bar = el('div', 'statusbar');
  bar.innerHTML = '<div class="fnlabel" id="fnlabel"></div><div class="sinfo"><span id="curr"></span><span id="next"></span><span id="sysmsg"></span></div><div class="icons" id="icons"></div>';
  display.append(scr, bar);
  const keys = el('div', 'skeys');
  const selGroup = el('div', 'selscreens');
  for (const [id, text, grp] of SCREEN_KEYS) {
    const b = pkey(text, grp ? 'green' : 'white', () => setScreen(id));
    b.dataset.screen = id;
    (grp ? selGroup : keys).append(b);
  }
  keys.prepend(selGroup);
  // Fn, Copy, Paste, Reset and the screen rotary, below the screen.
  const edit = el('div', 'editkeys');
  const fn = pkey('Fn', 'fn', () => { S.fnOpen = !S.fnOpen; refreshScreen(); });
  fn.id = 'fnkey';
  const copy = pkey('Copy', 'white'), paste = pkey('Paste', 'red'), reset = pkey('Reset', 'red');
  for (const [b, n] of [[copy, 'copy'], [paste, 'paste'], [reset, 'reset']]) { b.dataset.holdkey = n; holdKey(n, b); }
  const rot = el('div', 'bigrot');
  rot.title = 'Screen rotary: turns the parameter highlighted orange';
  const dial = el('div', 'dial');
  rot.append(dial);
  let angle = 0, acc = 0;
  dragY(rot, () => 0, (pos, e) => {
    const steps = Math.trunc((pos - acc) * 40);
    if (steps) { acc += steps / 40; for (let i = 0; i < Math.abs(steps); i++) rotaryTurn(Math.sign(steps), e.shiftKey); angle += steps * 6; dial.style.transform = `rotate(${angle}deg)`; }
  }, 200);
  rot.addEventListener('pointerdown', () => { acc = 0; });
  rot.addEventListener('wheel', e => { e.preventDefault(); const d = -Math.sign(e.deltaY); rotaryTurn(d, e.shiftKey); angle += d * 6; dial.style.transform = `rotate(${angle}deg)`; }, { passive: false });
  edit.append(fn, copy, paste, reset, rot);
  host.append(title, display, keys, edit);
}
function setScreen(id) {
  if (S.locked) return;
  if (!userAllows(id)) { message('Not available for this user'); return; }
  if (id === 'home' && S.screen === 'home') id = S.lastSel;  // Home again: back to the Sel screen
  if (id === 'setup' && S.screen === 'setup' && S.hold === 'setup') return;
  S.screen = id;
  if (id === 'proc' || id === 'route') S.lastSel = id;
  S.fnOpen = false;
  S.focus = null;
  refreshScreen();
}

// ============================================================================== right panel
function renderRightPanel() {
  const host = document.getElementById('rightpanel');
  host.innerHTML = '';
  withTag('right', () => {
    // Main meters: LR, or PAFL while a PAFL is selected.
    const meters = el('div', 'mainmeters');
    const marks = [['Pk', -3], ['+12', -6], ['+9', -9], ['+6', -12], ['+3', -15], ['0', -18], ['-3', -21], ['-6', -24], ['-12', -30], ['-18', -36], ['-24', -42], ['-30', -48]];
    const col = side => {
      const c = el('div', 'mcol');
      const ls = marks.map(([, db], i) => { const l = el('i', i === 0 ? 'red' : i < 5 ? 'yel' : 'grn'); l.db = db; c.append(l); return l; });
      c.leds = ls;
      return c;
    };
    const scaleCol = el('div', 'mscale');
    for (const [t] of marks) scaleCol.append(el('span', '', t));
    const L = col(), R = col();
    const paflLamp = el('div', 'pafllamp', 'PAFL');
    meters.append(L, scaleCol, R, paflLamp);
    onFrame(() => {
      const pafl = paflActive();
      paflLamp.classList.toggle('on', pafl);
      const m = pafl ? paflMeter() : busMeter('lr');
      for (const [c, db] of [[L, m.l], [R, m.r]]) c.leds.forEach(l => l.classList.toggle('on', db >= l.db));
    });
    // Qu-Drive port.
    const drive = el('div', 'quport');
    drive.append(el('div', 'pt', 'Qu-Drive'), el('div', 'usbport'));
    const driveLed = el('i', 'dled');
    drive.append(driveLed);
    onFrame(() => { const d = S.drive; driveLed.className = 'dled' + (d && d.state === 'rec' ? ' rec' : d && d.state === 'play' ? ' play' : ''); });
    // Engineer's monitor: phones level and Talk.
    const mon = el('div', 'monitor');
    mon.append(el('div', 'pt', "Engineer's monitor"), rotary(() => 'mon/level', 'Phones'));
    const talk = pkey('Talk', 'white', null);
    talkKey(talk);
    mon.append(talk);
    host.append(meters, drive, mon);
    host.querySelectorAll('.rot').forEach(r => { r.render(); watch('mon/level', () => r.render()); });
  });
}
// Talk: momentary; with the latch option a quick press latches.
function talkKey(b) {
  let t0 = 0, wasOn = false;
  b.addEventListener('pointerdown', () => { t0 = performance.now(); wasOn = on('talk/on'); setParam('talk/on', 1); });
  b.addEventListener('pointerup', () => {
    const quick = performance.now() - t0 < 500;
    if (on('talk/latch') && quick) setParam('talk/on', wasOn ? 0 : 1);
    else setParam('talk/on', 0);
  });
  watch('talk/on', v => b.classList.toggle('lit', v >= 0.5));
}

// =============================================================== mix assign and layer keys
function renderMixAssign() {
  const host = document.getElementById('mixassign');
  host.innerHTML = '';
  const pre = pkey('Pre Fade', 'white'), asg = pkey('Assign', 'white');
  pre.dataset.holdkey = 'prefade';
  asg.dataset.holdkey = 'assign';
  holdKey('prefade', pre);
  holdKey('assign', asg);
  host.append(pre, asg, el('div', 'hint', '+ press Sel'));

  const lk = document.getElementById('layerkeys');
  lk.innerHTML = '';
  lk.append(el('div', 'pt', 'Layers'));
  const up = pkey('', 'white layer', () => pressLayer('upper'));
  const lo = pkey('', 'white layer', () => pressLayer('lower'));
  up.id = 'layerUp'; lo.id = 'layerLo';
  lk.append(up, el('div', 'custlbl', '— Custom'), lo);
}
// Pressing both layer keys selects the Custom layer: here, pressing the active key again.
let lastLayerPress = { k: null, t: 0 };
function pressLayer(k) {
  const now = performance.now();
  if (on('custom/only')) { S.layer = 'custom'; renderBank(); return; }
  if ((lastLayerPress.k && lastLayerPress.k !== k && now - lastLayerPress.t < 400) || S.layer === k) S.layer = S.layer === 'custom' ? k : 'custom';
  else S.layer = k;
  lastLayerPress = { k, t: now };
  if (S.user === 'basic' && S.layer === 'upper') S.layer = 'lower';
  renderBank();
}

// ================================================================================= fader bank
let selKeys = [];
function renderBank() {
  const host = document.getElementById('bank');
  host.innerHTML = '';
  const up = document.getElementById('layerUp'), lo = document.getElementById('layerLo');
  if (up) {
    up.classList.toggle('lit', S.layer === 'upper' || S.layer === 'custom');
    lo.classList.toggle('lit', S.layer === 'lower' || S.layer === 'custom');
  }
  selKeys = [];
  withTag('bank', () => {
    const ids = layerIds(S.layer);
    const flip = S.flip > 0 && canFlip();
    const tops = el('div', 'tops'), labels = el('div', 'labels'), faders = el('div', 'faders');
    // Layer label strip: upper names, the custom write-on row, lower names.
    const rows = { upper: layerIds('upper'), custom: layerIds('custom'), lower: layerIds('lower') };
    for (const [row, list] of Object.entries(rows)) {
      const r = el('div', 'lrow r-' + row + (S.layer === row && !flip ? ' active' : ''));
      list.forEach((id, k) => {
        const cell = el('div', 'lc');
        if (flip && row === 'custom') cell.textContent = GEQ_FREQS[flipBand(k) - 1];
        else if (id) watchLabel(id, v => { cell.textContent = row === 'custom' ? (v || quName(id)) : quName(id) + (row === 'upper' && /^(fx|mix)\//.test(id) ? (busById(id).kind === 'fx' ? ' Send' : '') : '') ; if (row === 'lower' && v) cell.dataset.name = v; });
        else cell.textContent = row === 'custom' ? '' : '';
        r.append(cell);
      });
      labels.append(r);
    }
    ids.forEach((id, k) => {
      const top = el('div', 'stop');
      const mute = pkey('Mute', 'mute'), sel = pkey('Sel', 'green'), pafl = pkey('PAFL', 'grey');
      const leds = stripLeds();
      top.append(mute, sel, pafl, leds);
      tops.append(top);
      const fcol = el('div', 'fcol');
      const band = flip ? flipBand(k) : 0;
      const fpath = () => (flip ? `${S.mix}/geq/${band}` : stripFader(id));
      const f = fader(fpath, { geq: flip });
      const readout = el('div', 'fread');
      const nameplate = el('div', 'fnum', String(k + 1));
      fcol.append(f, readout, nameplate);
      faders.append(fcol);
      if (!id && !flip) { top.classList.add('empty'); f.render(); return; }

      mute.addEventListener('click', () => {
        if (flip || !id) return;
        if (isDca(id)) toggle(`dca/${num(id)}/mute`); else toggle(`${id}/mute`);
      });
      sel.addEventListener('click', () => {
        if (flip) { setParam(`${S.mix}/geq/${band}`, 0); return; }
        selPress(id);
      });
      pafl.addEventListener('click', () => {
        if (flip || !has(`${id}/pafl`)) return;
        toggle(`${id}/pafl`);
        if (on('pafl/selfollow') && on(`${id}/pafl`)) select(id);
      });
      selKeys.push({ id, sel, band });
      const fp = fpath();
      if (fp) watch(fp, v => { f.render(); readout.textContent = fmt(fp, v).replace(' dB', ''); });
      else f.render();
      onFrame(() => {
        if (flip) {
          const rta = S.rta ? S.rta[band + 1] : -90;
          const peak = rtaPeakIndex() === band + 1;
          leds.raw(peak, rta >= -30, rta >= -60);
          mute.classList.remove('lit', 'flash', 'flashlong');
          pafl.classList.remove('lit');
          return;
        }
        leds.set(stripLevel(id));
        const ms = muteState(id);
        mute.classList.toggle('lit', ms === 1);
        mute.classList.toggle('flash', ms === 2);
        mute.classList.toggle('flashlong', ms === 3);
        pafl.classList.toggle('lit', has(`${id}/pafl`) && on(`${id}/pafl`));
      });
    });
    host.append(tops, labels, faders);
    onFrame(updateSelKeys);
  });
  updateSelKeys();
}
function rtaPeakIndex() {
  if (!S.rta) return -1;
  let best = 0;
  for (let k = 1; k < S.rta.length; k++) if (S.rta[k] > S.rta[best]) best = k;
  return S.rta[best] > -70 ? best : -1;
}
// Sel LEDs: selection, or assignment / pre-fade while Assign / Pre Fade is held, or 0 dB in flip.
function updateSelKeys() {
  for (const { id, sel, band } of selKeys) {
    let lit;
    if (S.flip > 0 && canFlip()) lit = Math.abs(get(`${S.mix}/geq/${band}`) || 0) < 0.05;
    else if (S.hold === 'assign') { const p = assignPath(id); lit = !!p && on(p); }
    else if (S.hold === 'prefade') { const p = prePath(id); lit = !!p && on(p); }
    else lit = id === S.sel;
    sel.classList.toggle('lit', lit);
  }
  const ms = document.getElementById('masterSel');
  if (ms) ms.classList.toggle('lit', S.sel === S.mix);
}
function selPress(id) {
  if (S.hold === 'assign') { const p = assignPath(id); if (p) toggle(p); holdUsed(); return; }
  if (S.hold === 'prefade') { const p = prePath(id); if (p) toggle(p); holdUsed(); return; }
  if (S.hold && ['copy', 'paste', 'reset'].includes(S.hold)) {
    if (isDca(id)) return;
    if (S.hold === 'paste' && S.clip && S.clip.type === 'proc' && S.clip.kind !== kindOf(id) && !(isChannel(id) && /^(mono|stereo)$/.test(S.clip.kind))) { message('Cannot paste here'); return; }
    editBlock(id, 'proc');
    return;
  }
  select(id);
}

// ============================================================================== master strip
function renderMaster() {
  const host = document.getElementById('masterstrip');
  host.innerHTML = '';
  withTag('master', () => {
    const id = S.mix;
    const top = el('div', 'stop');
    const mute = pkey('Mute', 'mute', () => toggle(`${id}/mute`));
    const sel = pkey('Sel', 'green', () => masterSel());
    sel.id = 'masterSel';
    const pafl = pkey('PAFL', 'grey', () => { toggle(`${id}/pafl`); if (on('pafl/selfollow') && on(`${id}/pafl`)) select(id); });
    const leds = stripLeds();
    top.append(mute, sel, pafl, leds);
    const lr = pkey('LR', 'blue', () => setMix('lr'));
    lr.classList.toggle('lit', id === 'lr');
    const mid = el('div', 'mmid');
    const name = el('div', 'mname');
    watchLabel(id, v => { name.textContent = v || (id === 'lr' ? 'Master' : quName(id)); });
    mid.append(lr, name);
    const f = fader(() => `${id}/fader`);
    const readout = el('div', 'fread');
    watch(`${id}/fader`, v => { f.render(); readout.textContent = fmt(`${id}/fader`, v).replace(' dB', ''); });
    const fcol = el('div', 'fcol');
    fcol.append(f, readout);
    host.append(top, mid, fcol);
    onFrame(() => {
      leds.set(stripLevel(id));
      const ms = muteState(id);
      mute.classList.toggle('lit', ms === 1);
      mute.classList.toggle('flash', ms === 2);
      mute.classList.toggle('flashlong', ms === 3);
      pafl.classList.toggle('lit', on(`${id}/pafl`));
    });
  });
  // Mix select keys.
  const ms = document.getElementById('mixselect');
  ms.innerHTML = '';
  ms.append(el('div', 'pt', 'Mix Select'));
  const groups = [['fx/1', 'fx/2'], ['mix/1', 'mix/2', 'mix/3', 'mix/4'], ['mix/5', 'mix/6', 'mix/7']];
  for (const g of groups) {
    const box = el('div', 'mgroup');
    for (const id of g) {
      const b = pkey(quName(id), busById(id).stereo ? 'blue' : 'lblue', () => {
        if (editMix(id)) return;
        if (!userAllows('mix:' + id)) { message('Not available for this user'); return; }
        setMix(id);
      });
      b.classList.toggle('lit', S.mix === id);
      box.append(b);
    }
    ms.append(box);
  }
  updateSelKeys();
}
// Master Sel: selects the mix; with Assign / Pre Fade held it toggles all sources; with
// Copy/Paste/Reset it acts on the mix processing.
function masterSel() {
  const id = S.mix;
  if (S.hold === 'assign' || S.hold === 'prefade') {
    const paths = inputs().map(i => (S.hold === 'assign' ? assignPath(i.id) : prePath(i.id))).filter(Boolean);
    const allOn = paths.every(p => on(p));
    for (const p of paths) setParam(p, allOn ? 0 : 1);
    message(`${quName(id)} ${S.hold === 'assign' ? 'Assign' : 'Pre Fade'} All ${allOn ? 'Off' : 'On'}`);
    holdUsed();
    return;
  }
  if (S.hold && ['copy', 'paste', 'reset'].includes(S.hold)) { editBlock(id, 'proc'); return; }
  select(id);
}

// ================================================================================== SoftKeys
const tapTimes = {};
function renderSoftKeys() {
  const host = document.getElementById('softkeys');
  host.innerHTML = '';
  withTag('soft', () => {
    for (let k = 1; k <= S.info.softKeys; k++) {
      const b = pkey(`Soft ${k}`, 'white soft');
      b.addEventListener('pointerdown', () => softKey(k, true));
      b.addEventListener('pointerup', () => softKey(k, false));
      host.append(b);
      onFrame(() => b.classList.toggle('lit', softLit(k)));
    }
  });
}
function softLit(k) {
  const f = get(`soft/${k}/func`), a = get(`soft/${k}/arg`) || 0;
  switch (f) {
    case 1: { const id = customStripId(a); return !!id && muteState(id) % 2 === 1; }
    case 2: return on(`mg/${a + 1}/mute`);
    case 3: return on(`dca/${a + 1}/mute`);
    case 12: return paflActive();
    case 13: return !!S.drive && S.drive.state === 'play' && S.drive.mode === 'stereo';
    case 14: return !!S.drive && S.drive.state === 'rec' && S.drive.mode === 'stereo';
    case 15: return !!S.drive && S.drive.state === 'rec' && S.drive.mode === 'multi';
    case 16: return on('talk/on');
  }
  return false;
}
function softKey(k, down) {
  const f = get(`soft/${k}/func`), a = get(`soft/${k}/arg`) || 0;
  if (f === 16) { setParam('talk/on', down ? 1 : 0); return; }
  if (!down) return;
  switch (f) {
    case 1: { const id = customStripId(a); if (id) toggle(isDca(id) ? `dca/${num(id)}/mute` : `${id}/mute`); break; }
    case 2: toggle(`mg/${a + 1}/mute`); break;
    case 3: toggle(`dca/${a + 1}/mute`); break;
    case 4: case 5: case 6: {
      const fx = (a % 4) + 1, now = performance.now(), last = tapTimes[k] || 0;
      tapTimes[k] = now;
      const ms = now - last;
      if (ms > 60 && ms < 2000) {
        if (f !== 6) setParam(`fx/${fx}/dly/time`, ms);
        if (f !== 5) setParam(`fx/${fx}/dly/rtime`, ms);
        message(`FX${fx} Tap ${Math.round(ms)} ms`);
      }
      break;
    }
    case 7: send(`scene recall ${a + 1}`); break;
    case 8: if (S.scene) send(`scene store ${S.scene}`); break;
    case 9: sceneGo(); break;
    case 10: stepNext(1); break;
    case 11: stepNext(-1); break;
    case 12: for (const p in S.params) if (p.endsWith('/pafl') && on(p)) setParam(p, 0); break;
    case 13: send(S.drive && S.drive.state === 'play' ? 'play stop' : `play ${S.driveFiles.filter(n => n.startsWith('QU-ST')).slice(-1)[0] || ''}`); break;
    case 14: send(S.drive && S.drive.state === 'rec' ? 'rec stop' : 'rec stereo'); break;
    case 15: send(S.drive && S.drive.state === 'rec' ? 'rec stop' : 'rec multi'); break;
  }
}
// Scene navigation: Next steps through stored scenes; Go recalls Next then advances.
function stepNext(dir) {
  const stored = S.scenes.map(s => s.n);
  if (!stored.length) return;
  let i = stored.indexOf(S.next);
  if (i < 0) i = dir > 0 ? -1 : stored.length;
  S.next = stored[clamp(i + dir, 0, stored.length - 1)];
  statusScene();
  refreshScreen('scenes');
}
function sceneGo() {
  if (!S.scenes.some(s => s.n === S.next)) return;
  send(`scene recall ${S.next}`);
  const stored = S.scenes.map(s => s.n);
  const i = stored.indexOf(S.next);
  if (i >= 0 && i + 1 < stored.length) S.next = stored[i + 1];
}

// ==================================================================================== users
// Admin: everything. Standard: no Setup. Basic: LR faders and mutes, scene recall only.
function userAllows(what) {
  if (S.user === 'admin') return true;
  if (S.user === 'standard') return what !== 'setup';
  return what === 'home' || what === 'scenes';
}

// ================================================================================ fit to view
function fit() {
  const c = document.getElementById('console');
  if (!c) return;
  const W = 1800, H = 1236;
  const mobile = window.innerWidth < 900;
  document.body.classList.toggle('mobile', mobile);
  if (mobile) { c.style.transform = ''; consoleScale = 1; return; }
  consoleScale = Math.min(window.innerWidth / W, window.innerHeight / H);
  c.style.transform = `scale(${consoleScale})`;
  c.style.left = Math.max(0, (window.innerWidth - W * consoleScale) / 2) + 'px';
}
window.addEventListener('resize', fit);
