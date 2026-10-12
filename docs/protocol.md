# QNU16 control protocol

The control protocol is plain text over a WebSocket at `ws://<host>:8016/ws`. Each WebSocket
message carries one or more commands separated by newlines. Any client can use it, including the
bundled web UI, scripts, a MIDI bridge or show-control software.

## Client → engine

| Command | Effect |
|---|---|
| `set <path> <value>` | Set a parameter. The value is clamped to its range. Toggles snap to 0/1 and choices round to an integer. |
| `label <key> <text>` | Name a channel or bus: any input or bus id, plus `dca/N` and `mg/N`. Empty text clears the name. |
| `state` | Request a full parameter and label dump. |
| `info` | Request topology (`i`) and parameter metadata (`k`). |
| `scene list` / `scene save <name>` / `scene load <name>` | Scenes. Names are 1–48 characters from `A–Z a–z 0–9 space - _`. The UI numbers them `001 Name` … `100 Name`, as the Qu's 100 scene memories. Loading resets anything the scene doesn't mention to its default. |
| `rec multi` / `rec stereo` / `rec stop` | Recorder: 18 tracks (Ch 1–16 direct outs + LR) or stereo LR, written as a 32-bit float WAV. |

## Engine → client

| Message | Meaning |
|---|---|
| `i {json}` | Topology: `inputs` and `buses` (each with `id`, `name`, `kind`, `stereo`), `outputSources`, `fxTypes`, and counts. |
| `k\n<path> <min> <max> <default> <kind> [unit]…` | Metadata for every parameter. Kind is `c` (continuous), `t` (toggle) or `h` (choice). |
| `p <path> <value>` | A parameter changed. Changes are broadcast to every client, so several tablets stay in sync. |
| `l <key> <text>` | A label changed. |
| `s <current>\n<name>\n…` | Scene list. The first line holds the current scene. |
| `m v,v,…` | Meters, sent every 50 ms. For each input in `inputs` order: `in dB, post-fader dB, gain reduction dB, gate open (0/1)`. Then for each bus in `buses` order: `L dB, R dB, gain reduction dB`. Then `PAFL L, PAFL R`. Levels are dBFS peak, with −90 meaning silence. |
| `r v,…` | RTA, sent every 50 ms: 31 third-octave bands, 20 Hz – 20 kHz, in dBFS. It analyses the PAFL signal, so it shows LR when nothing is selected. |
| `d <state> <mode> <seconds> <dropouts> <file>` | Recorder status: `rec` or `stop`, `multi` or `stereo`. Sent on connect, every 50 ms while recording, and once on stop. |
| `e <text>` | An error. It goes only to the client that caused it. |

On connect, the engine sends `i`, `k`, a full `p`/`l` dump, `s`, then `d`.

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
| FX sends 1–2 (mono, feed FX engines 1 and 2) | `fx/1`, `fx/2` |

FX engines 3 and 4 have no send bus. Each takes a mix as its source (`fx/3/src`, `fx/4/src`), as on
the Qu-16. The Qu-16 has no groups or matrices. The engine still supports them through the
`kGroups`/`kMatrices` constants (as on a Qu-24/32), but they are set to zero.

## Parameter paths

`<in>` is an input id and `<bus>` is a bus id. `N` is 1-based.

### Input channels (`ch/N`, `st/N`)

| Path | Range | Default |
|---|---|---|
| `<in>/gain`, `<in>/polarity` | −24 … +24 dB (digital trim); toggle | 0; 0 |
| `<in>/hpf/on`, `<in>/hpf/freq` | toggle; 20 … 2000 Hz (12 dB/oct) | 0; 80 |
| `<in>/gate/on`, `thresh`, `range` | toggle; −80 … 0 dB; 0 … 80 dB | 0; −50; 40 |
| `<in>/gate/attack`, `hold`, `release` | 0.05 … 50; 0 … 500; 5 … 2000 ms | 0.5; 50; 150 |
| `<in>/eq/on` | toggle | 1 |
| `<in>/eq/B/freq`, `gain`, `width` (B = 1–4) | 20 … 20k Hz; ±15 dB; 1/9 … 1.5 octaves (bells only) | 100/400/2.5k/8k; 0; 1 |
| `<in>/eq/1/type`, `<in>/eq/4/type` | 0 = shelf, 1 = bell | 0 |
| `<in>/comp/on`, `thresh`, `ratio`, `knee`, `makeup` | toggle; −60 … 0 dB; 1 … 20; 0 … 12 dB; 0 … 24 dB | 0; −20; 3; 6; 0 |
| `<in>/comp/attack`, `release` | 0.1 … 100 ms; 10 … 2000 ms (manual types only) | 10; 100 |
| `<in>/comp/mode` | 0 Manual Peak, 1 Manual RMS, 2 Slow Opto, 3 Punchbag | 1 |
| `<in>/delay/on`, `<in>/delay/time` | toggle; 0 … 85 ms | 0; 0 |
| `<in>/pan` | −1 … +1 (constant-power pan for mono, balance for stereo) | 0 |
| `<in>/fader`, `mute`, `pafl`, `lr` | −90 (−∞) … +10 dB; toggles | 0; 0; 0; 1 |
| `<in>/mix/M/level`, `pre`, `on` (M = 1–7) | −90 … +10 dB; toggle; toggle (Assign) | −∞; 1; 1 |
| `<in>/fx/F/level`, `pre`, `on` (F = 1–2) | −90 … +10 dB; toggle; toggle (Assign) | −∞; 0; 1 |
| `<in>/dca/D`, `<in>/mg/M` | assign toggles | 0 |

FX returns (`fxret/N`) have `eq/*`, `pan` (balance), `fader`, `mute`, `pafl`, `lr`, `mix/*`, `dca/*`
and `mg/*`. They have no FX sends, so there are no feedback loops.

### Masters

| Path | Applies to |
|---|---|
| `<bus>/fader`, `<bus>/mute`, `<bus>/pafl` | all |
| `<bus>/balance` | stereo buses |
| `<bus>/eq/*` (as inputs), `<bus>/comp/*` | LR, mixes |
| `<bus>/geq/on`, `<bus>/geq/1` … `geq/28` (±12 dB, constant 1/3 octave, 31.5 Hz – 16 kHz) | LR, mixes |
| `<bus>/delay/on`, `<bus>/delay/time` (0 … 170 ms) | LR, mixes |

### FX engines (`fx/N/…`)

| Path | Range |
|---|---|
| `fx/N/type` | 0 Reverb, 1 Delay, 2 Chorus, 3 Flanger. Defaults: FX1 hall reverb, FX2 short reverb, FX3 delay, FX4 chorus |
| `fx/3/src`, `fx/4/src` | 0 off, 1–7 = the mix that feeds this engine (one block late, about 5 ms) |
| `fx/N/rev/decay`, `predelay`, `damp`, `size` | 0.2 … 10 s; 0 … 200 ms; 0 … 100 %; 50 … 100 % |
| `fx/N/dly/time`, `feedback`, `hicut`, `pingpong` | 1 … 2000 ms; 0 … 95 %; 1k … 20k Hz; toggle |
| `fx/N/cho/rate`, `depth`, `delay` | 0.05 … 5 Hz; 0 … 10 ms; 5 … 30 ms |
| `fx/N/flg/rate`, `depth`, `delay`, `feedback` | 0.05 … 5 Hz; 0 … 5 ms; 0.5 … 10 ms; 0 … 90 % |

### Global

| Path | Range |
|---|---|
| `dca/D/fader`, `dca/D/mute` | −90 … +10 dB; toggle |
| `mg/M/mute` | toggle (mute group master) |
| `out/O/src` (O = 1–18) | index into `outputSources`. Default: 1–2 LR, 3–12 Mix 1–10, 13–14 Alt out (PAFL), 15–18 off |
| `talk/on`, `talk/src`, `talk/gain`, `talk/hpf`, `talk/lr`, `talk/mix/M` | talkback: latch; input 1–22 (0 none); ±24 dB; 120 Hz HPF; assigns |
| `gen/on`, `gen/type`, `gen/freq`, `gen/level`, `gen/lr`, `gen/mix/M` | signal generator: sine/white/pink; 20 Hz–20 kHz; −90…0 dBFS; assigns |
| `mon/level` | PAFL (headphone) level, −90 … +10 dB |
| `custom/S/strip` (S = 1–16) | Custom layer: 0 none, 1–23 inputs, 24–33 masters (bus order), 34–37 DCA 1–4. Default DCA 1–4 |
| `soft/K/func` (K = 1–4) | SoftKey: 0 none, 1–4 mute group, 5–8 DCA mute, 9 talkback, 10–13 tap tempo FX1–4, 14/15 next/previous scene. Default mute groups 1–4 |

## Signal flow

```
input → gain/polarity ─┬→ direct out
                       └→ HPF → gate → PEQ → comp → delay ─┬→ PFL (pre-fader, ignores mute)
                                                           ├→ pre-fader mix / FX sends (× mute)
          fader × mute × mute groups × DCAs ───────────────┼→ post-fader mix / FX sends
                                                           └→ LR (panned)
FX1/FX2 sends → FX engines 1/2;  mix chosen by fx/3/src, fx/4/src → FX engines 3/4
FX engines → FX returns (as input channels)
talkback, signal generator → LR / mixes (assignable)
LR / mixes → PEQ → GEQ → comp → delay → fader → outputs, AFL
PAFL → RTA;  PAFL × monitor level → PAFL / Alt outputs
```

A mono source sends to a stereo mix following its pan. A stereo source sends to a mono mix as
(L+R)/2. When no PAFL is selected, the PAFL outputs carry LR.
