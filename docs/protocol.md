# QNU16 control protocol

The control protocol is plain text over a WebSocket at `ws://<host>:8016/ws`. Each WebSocket
message carries one or more commands separated by newlines. Any client can use it, including the
bundled web UI, scripts, a MIDI bridge or show-control software.

Levels, thresholds and the signal generator use the **Qu dB scale**: 0 dB = 0 dBu = −18 dBFS,
the "0" LED on the strip meters. Meters and the RTA are in dBFS.

## Client → engine

| Command | Effect |
|---|---|
| `set <path> <value>` | Set a parameter. The value is clamped to its range. Toggles snap to 0/1 and choices round to an integer. As on the Qu, one change can cause others, which are broadcast too: linked channels follow each other, selecting a PAFL clears the others unless `pafl/additive` is on, and switching a channel into the AMM opens its fader to 0 dB. |
| `label <key> <text>` | Name a channel or bus: any input or bus id, plus `dca/N` and `mg/N`. Names are up to 6 characters. Empty text clears the name. |
| `state` | Request a full parameter and label dump. |
| `info` | Request topology (`i`) and parameter metadata (`k`). |
| `scene list` | Request the scene list (`s`). |
| `scene store <n> [name]` | Store the current settings in scene *n* (1–100). It keeps the scene's name and filter unless a new name is given. Names are up to 14 characters. |
| `scene recall <n>` | Recall scene *n* (see [Scenes](#scenes)). |
| `scene name <n> <name>` | Rename scene *n*. Naming an empty scene stores the current settings in it. |
| `scene clear <n>` | Clear scene *n*, including its filter. |
| `scene copy <from> <to>` | Copy a scene: its name, contents and filter. |
| `scene filter <n> <category> <0\|1>` | Block (1) or allow (0) a category in scene *n*'s own recall filter. |
| `scene reset` | Reset Mix Settings. |
| `rec multi` / `rec stereo` | Qu-Drive recording: multitrack (the 18 USB tracks) or stereo (LR). |
| `play <name>` | Play a recording (`QU-STnnn.WAV` or `QU-MTnnn`) back. |
| `rec stop` / `play stop` | Stop the transport. |
| `drive list` | Request the recordings on the drive (`q`). |

## Engine → client

| Message | Meaning |
|---|---|
| `i {json}` | Topology: `inputs` and `buses` (each with `id`, `name`, `kind`, `stereo`), `outputSources`, `usbSources`, `fxTargets`, `fxTypes`, `reverbModels`, `sceneCategories`, and counts. |
| `k\n<path> <min> <max> <default> <kind> [unit]…` | Metadata for every parameter. Kind is `c` (continuous), `t` (toggle) or `h` (choice). |
| `p <path> <value>` | A parameter changed. Changes are broadcast to every client, so several tablets stay in sync. |
| `l <key> <text>` | A label changed. |
| `s <current>\n<n> <filter> <name>\n…` | Scene list: the current scene (0 for none), then each stored scene with its blocked filter categories (comma-separated, or `-`). |
| `m v,v,…` | Meters, sent every 50 ms. For each input in `inputs` order: `in dB, post-fader dB, gain reduction dB, gate open (0/1)`. Then for each bus in `buses` order: `L dB, R dB, gain reduction dB`. Then `PAFL L, PAFL R`. Levels are dBFS peak, with −90 meaning silence. |
| `a v,…` | AMM gain for CH 1–16 in dB, every 50 ms (−90 for channels not in the AMM). |
| `r v,…` | RTA, sent every 50 ms: 31 third-octave bands, 20 Hz – 20 kHz, in dBFS. It analyses the PAFL signal. |
| `d <state> <mode> <seconds> <length> <dropouts> <file>` | Qu-Drive status: `rec`, `play` or `stop`; `multi` or `stereo`; elapsed and total seconds. Sent on connect, every 50 ms while running, and once when it stops. |
| `q\n<name>…` | Recordings on the drive. |
| `e <text>` | An error. It goes only to the client that caused it. |

On connect, the engine sends `i`, `k`, a full `p`/`l` dump, `s`, `d` and `q`.

## Topology (matches the Qu-16)

| Inputs | ids |
|---|---|
| 16 mono channels | `ch/1` … `ch/16` (physical inputs 1–16) |
| 3 stereo channels | `st/1` … `st/3` (physical inputs 17–22, as L/R pairs) |
| 4 stereo FX returns | `fxret/1` … `fxret/4` (fed by the internal FX engines) |

| Buses | ids |
|---|---|
| Main LR (stereo) | `lr` |
| Mix 1–4 (mono), Mix 5-6, 7-8, 9-10 (stereo) | `mix/1` … `mix/7` |
| FX sends 1–2 (mono) | `fx/1`, `fx/2` |

The Qu-16 has no groups or matrices. The engine still supports them through the
`kGroups`/`kMatrices` constants (as on a Qu-24/32), but they are set to zero.

## Parameter paths

`<in>` is an input id and `<bus>` is a bus id. `N` is 1-based. Defaults are the Qu's Reset Mix
Settings.

### Input channels (`ch/N`, `st/N`)

Processing runs in the Qu's order: preamp → FX insert → HPF → ducker → gate → PEQ → comp → delay
→ fader, mute, pan.

| Path | Range | Default |
|---|---|---|
| `<in>/src` | 0 Local, 1 USB (Qu-Drive playback) | 0 |
| `<in>/gain`, `<in>/polarity` | −24 … +24 dB (digital trim: the ADA8200's preamp gains are set on the unit); toggle | 0; 0 |
| `<in>/insert/on`, `<in>/insert/type` | Insert key; 0 FX insert, 1 ducker | 0; 0 |
| `<in>/hpf/on`, `<in>/hpf/freq` | toggle; 20 … 2000 Hz (12 dB/oct) | 0; 100 |
| `<in>/duck/gang` | follow the ducker gang (`duck/*`) instead of this channel's settings | 0 |
| `<in>/duck/trigger` | 0 none, 1–16 CH, 17–19 ST (post-preamp) | 0 |
| `<in>/duck/thresh`, `depth` | −72 … +18 dB; 0 … 60 dB | −30; 15 |
| `<in>/duck/attack`, `hold`, `release` | 20 … 600 ms; 40 ms … 10 s; 250 ms … 5 s | 50; 500; 1000 |
| `<in>/gate/on`, `thresh`, `depth` | toggle; −72 … +18 dB; 0 … 60 dB | 0; −36; 20 |
| `<in>/gate/attack`, `hold`, `release` | 50 µs … 300 ms; 10 ms … 5 s; 10 ms … 1 s | 0.05; 100; 300 |
| `<in>/eq/on` | toggle | 1 |
| `<in>/eq/B/freq`, `gain`, `width` (B = 1–4: LF, LM, HM, HF) | 20 … 20k Hz; ±15 dB; 1/9 … 1.5 octaves. LF and HF go up to 2, and above 1.5 they are shelves (the widest setting). | 63/200/2.5k/10k; 0; 0.75 |
| `<in>/comp/on`, `thresh`, `ratio` | toggle; −46 … +18 dB; 1 … 50 (50 = ∞:1) | 0; 0; 3.2 |
| `<in>/comp/attack`, `release`, `gain` | 0.3 … 300 ms; 100 … 2000 ms (manual types); 0 … 18 dB | 10; 300; 0 |
| `<in>/comp/hard` | Hard Knee (off = soft) | 0 |
| `<in>/comp/type` | 0 Manual Peak, 1 Manual RMS, 2 Auto Slow Opto, 3 Auto Punchbag | 0 |
| `<in>/delay/on`, `<in>/delay/time` | toggle; 0 … 85 ms | 0; 0 |
| `<in>/pan` | −1 … +1 (constant-power pan for mono, balance for stereo) | 0 |
| `<in>/fader`, `mute`, `pafl`, `lr` | −90 (−∞) … +10 dB; toggles | −∞; 0; 0; 1 |
| `<in>/dout/trim` | direct out trim, −∞ … +10 dB | 0 |
| `<in>/mix/M/level`, `pre`, `on` (M = 1–7) | −∞ … +10 dB; toggle; toggle (Assign) | −∞; 1; 1 |
| `<in>/mix/M/pan` (stereo mixes, M = 5–7) | send pan (balance for stereo sources) | 0 |
| `<in>/fx/F/level`, `pre`, `on` (F = 1–2) | −∞ … +10 dB; toggle; toggle (Assign) | −∞; 0; 1 |
| `<in>/dca/D`, `<in>/mg/M` | assign toggles | 0 |

FX returns (`fxret/N`) have `eq/*`, `pan` (balance), `fader` (default 0 dB), `mute`, `pafl`,
`lr`, `mix/*`, `dca/*` and `mg/*`. They have no FX sends, so there are no feedback loops.

A channel's fader plus its DCAs can boost by at most +10 dB. DCA levels act at the fader
(post-fade only). A DCA's mute acts like a mute group, on pre-fade sends too.

### Masters

| Path | Applies to |
|---|---|
| `<bus>/fader`, `<bus>/mute`, `<bus>/pafl` | all (LR −∞, mixes and FX sends −4 dB) |
| `<bus>/mg/M` | mute group assigns: LR, mixes, FX sends |
| `<bus>/balance` | stereo buses (output balance) |
| `<bus>/presrc` | mixes, FX sends: where pre-fade sends tap the channel: 0 Post-Preamp, 1 Pre-EQ (post gate), 2 Post-EQ (default), 3 Post-All |
| `<bus>/insert/on` | LR, mixes: FX insert, before the PEQ |
| `<bus>/eq/*` (as inputs), `<bus>/comp/*` | LR, mixes |
| `<bus>/geq/on`, `<bus>/geq/1` … `geq/28` (±12 dB, constant 1/3 octave, 31.5 Hz – 16 kHz) | LR, mixes (in, flat) |
| `<bus>/delay/on`, `<bus>/delay/time` (0 … 170 ms) | LR, mixes |

### FX (`fx/N/…`, N = 1–4)

| Path | Range |
|---|---|
| `fx/N/patch` | 0 Unassigned, 1 Insert, 2 Mix>Return, 3 Ch>Return. Default Mix>Return |
| `fx/N/src` | Index into `fxTargets`: 0 none, 1–19 CH 1–16 / ST 1–3, then the buses (LR, Mix 1 … 9-10, FX 1, FX 2). Mix>Return takes a bus; Ch>Return an input (its direct out); Insert an input or LR / a mix. Default FX1/FX2 from their send buses, FX3/FX4 none. Buses other than the FX sends feed their engine one block (≤ 5 ms) late. |
| `fx/N/mix` | Insert dry/wet, 0 … 100 % |
| `fx/N/type` | 0 Reverb, 1 Delay, 2 ADT, 3 Chorus, 4 Symphonic, 5 Flanger, 6 Phaser, 7 Gated Verb. Default FX1 plate reverb, FX2 delay, FX3 chorus, FX4 room |
| `fx/N/rev/model`, `decay`, `predelay`, `damp`, `size`, `lfcut`, `hfcut` | Classic/Hall/Room/Plate; 0.2 … 10 s; 0 … 200 ms; HF decay 0 … 100 %; 50 … 100 %; 20 … 1000 Hz; 1 … 20 kHz |
| `fx/N/dly/time`, `rtime`, `feedback`, `hicut`, `pingpong` | left and right taps 1 … 2000 ms; 0 … 95 %; 1 … 20 kHz; toggle |
| `fx/N/adt/sep`, `thick` | 5 … 100 ms; 0 … 100 % |
| `fx/N/cho/rate`, `depth`, `delay` | 0.05 … 5 Hz; 0 … 10 ms; 5 … 30 ms |
| `fx/N/sym/rate`, `depth` | 0.05 … 5 Hz; 0 … 10 ms |
| `fx/N/flg/rate`, `depth`, `delay`, `feedback` | 0.05 … 5 Hz; 0 … 5 ms; 0.5 … 10 ms; 0 … 90 % |
| `fx/N/pha/rate`, `depth`, `feedback`, `stages` | 0.05 … 5 Hz; 0 … 100 %; 0 … 90 %; 4/6/8/12 |
| `fx/N/gvb/thresh`, `hold`, `release` | gated verb: −72 … +18 dB; 10 … 1000 ms; 5 … 500 ms (with the `rev/*` tail) |

### Global

| Path | Range |
|---|---|
| `dca/D/fader`, `dca/D/mute` | −∞ … +10 dB (0); toggle |
| `mg/M/mute` | toggle (mute group master) |
| `out/O/src` (O = 1–18) | index into `outputSources`. Default: 1–2 LR, 3–12 Mix 1–10, 13–14 Alt out (PAFL), 15–18 off |
| `dout/src`, `dout/fader`, `dout/mute` | direct outs: 0 Post-Preamp, 1 Post-Insert, 2 Post-PEQ, 3 Post-Comp, 4 Post-Delay (default); follow fader (1); follow mute (1) |
| `usb/T/src` (T = 1–18), `usb/chsrc` | Qu-Drive multitrack tracks: index into `usbSources` (default CH 1–16, LR); channel tracks take 0 the insert send (post-preamp) or 1 the direct out |
| `link/K/on` (K = 1–8: CH 1-2 … 15-16) | stereo link. Linking copies the odd channel to the even one; after that they follow each other |
| `link/K/preamp`, `sc`, `fader`, `pan` | link options: gain and source; dynamics sidechain (one stereo chain); fader and mute; pan (as width: the even channel mirrors the odd one). Default all on. Processing, sends and assigns are always linked; polarity never |
| `duck/trigger`, `thresh`, `depth`, `attack`, `hold`, `release` | the ducker gang, as `<in>/duck/*` |
| `pafl/additive`, `inafl`, `outafl` | PAFL keys add up (0); input AFL instead of PFL (0); output AFL instead of PFL (1) |
| `pafl/selfollow`, `mixfollow`, `lr` | Sel follows PAFL (1); master PAFL follows the mix keys (1); LR to PAFL when nothing is selected (1) |
| `pafl/delay`, `pafl/trim` | 0 … 85 ms; PFL trim −24 … 0 dB |
| `amm/on`, `amm/hpf`, `amm/lpf`, `amm/follow` | automatic mic mixer in (1); sidechain HPF 20 … 2000 Hz (250), LPF 120 Hz … 20 kHz (5k); follow fader (0) |
| `amm/C/on`, `amm/C/prio` (C = 1–16) | channel in the AMM (0); priority −15 … +15 dB (0) |
| `talk/on`, `talk/src`, `talk/gain`, `talk/lr`, `talk/mix/M` | Talk key; input 1–22 (0 none); ±24 dB; assigns |
| `talk/hpf`, `talk/hpffreq`, `talk/latch` | HPF in (1), 20 … 2000 Hz (120, 12 dB/oct); latching Talk key option (0) |
| `gen/mute`, `gen/type`, `gen/freq`, `gen/level`, `gen/lr`, `gen/mix/M` | signal generator: mute; 0 sine, 1 white, 2 pink (default), 3 band-pass noise; 20 Hz … 20 kHz; −∞ … +18 dB (−10); assigns. Off while Talk is active |
| `mon/level` | PAFL (headphone) level, −∞ … +10 dB |
| `custom/S/strip` (S = 1–16), `custom/only` | Custom layer: 0 unassigned, 1–23 inputs, 24–33 masters (bus order), 34–37 DCA 1–4. "Custom layer only" |
| `soft/K/func`, `soft/K/arg` (K = 1–4) | SoftKeys: 0 Unassigned, 1 Channel Mute, 2 Mute Group, 3 DCA Mute, 4 Tap Tempo L+R, 5 Tap Tempo L, 6 Tap Tempo R, 7 Recall Scene, 8 Scene Store Current, 9 Scene Recall Go, 10 Scene Next, 11 Scene Previous, 12 PAFL Clear, 13 Qu-Drive Stereo Play, 14 Qu-Drive Stereo Rec, 15 Qu-Drive Multitrack Rec, 16 Talkback. `arg` is the channel (custom-strip numbering), group, FX or scene (0-based). Default Mute Group 1–4 |
| `filter/<category>` | global scene recall filter (1 = blocked). Default: `custom` and `soft` blocked |
| `safe/<id>` | scene safe for an input or a master |

## Scenes

Scenes are 100 numbered memories (`scenes/NNN.scene`). A scene stores every mix setting. It does
not store the PAFL setup, the signal generator, the monitor level, filters, safes or preferences
(`talk/latch`, `custom/only`).

Recall sets every stored parameter from the scene, or to its default if the scene doesn't mention
it, except:

- categories blocked by the global filter (`filter/<category>`) or by the scene's own filter;
- parameters and names of safe channels and masters (`safe/<id>`).

The categories follow the Qu's Scene Recall Filter page:

| Category | Covers |
|---|---|
| `inpatch`, `preamp`, `fxpatch` | input source (Local/USB); gain and polarity; FX patching |
| `chproc`, `fxparam` | input processing (HPF, ducker, gate, PEQ, comp, delay, insert, linking); FX types and parameters |
| `innames`, `infaders`, `inmutes` | input names; faders and pan; mutes |
| `outpatch` | output patch, USB tracks, direct-out settings |
| `mixproc`, `lrproc` | mix and FX send processing; LR processing |
| `mixnames`, `mixfaders`, `mixmutes` | master names; faders and balance; mutes |
| `amm`, `custom`, `soft`, `talk` | AMM; custom layer; SoftKeys; talkback |
| `mixsends`, `fxsends`, `mutedca` | mix sends and LR assign (and pre-fade sources); FX sends; mute group and DCA assigns |
| `masters` | DCA and mute group masters, and their names |

Reset Mix Settings (`scene reset`) sets every stored parameter to its default and clears the
names. It ignores filters and safes.

## Qu-Drive

The drive is the `--recordings` directory, laid out as the Qu's USB drive:

- stereo: `QU-STnnn.WAV`, LR, 2 channels, 24-bit;
- multitrack: a folder `QU-MTnnn` with `TRK01.WAV` … `TRK18.WAV`, mono 24-bit, from the USB track
  patch (`usb/T/src`).

Playback goes to the channels whose source is USB (`<in>/src` = 1): stereo recordings play on ST3,
multitrack tracks 1–16 on CH 1–16 and 17–18 on ST1.

## Signal flow

```
input (Local / USB) → preamp ─┬→ insert send (USB tracks, Post-Preamp sources)
                              └→ FX insert → HPF → ducker → gate ─┬→ Pre-EQ tap
                                   → PEQ ─┬→ Post-EQ tap (default pre-fade source)
                                   → comp → delay ─┬→ PFL (pre-fader, ignores mute)
                                                   ├→ direct out (source, trim, follow fader/mute)
                                                   ├→ pre-fade mix / FX sends (at their tap, × mute)
          fader × DCAs (≤ +10 dB) × mute × mute groups × AMM gain ─┬→ post-fade mix / FX sends
                                                                   └→ LR (panned), input AFL
FX engines ← FX sends / mixes / LR (Mix>Return), channel direct outs (Ch>Return), or inline (Insert)
FX engines → FX returns (as input channels)
talkback, signal generator → LR / mixes (assignable)
LR / mixes → FX insert → PEQ → GEQ → comp → delay → (output PFL) → fader × mute groups → outputs, output AFL
PAFL → delay → RTA;  PAFL × monitor level → PAFL / Alt outputs
```

A mono source sends to a stereo mix following its send pan. A stereo source sends to a mono mix as
(L+R)/2.
