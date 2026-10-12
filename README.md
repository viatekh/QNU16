# QNU16

**An open-source 16-channel digital mixing console for a Raspberry Pi or a mini PC, controlled
from any tablet.**

QNU16 aims to compete with entry-level digital desks like the Allen & Heath Qu-16 for about a
quarter of the price. It pairs a cheap computer with a class-compliant USB interface and runs a
real-time mixing engine with a web control surface.

![QNU16 control surface](docs/img/surface.png)

```
 mics ──► UMC1820 (8 pre) ──USB──► Raspberry Pi 5 / N100 ──USB──► UMC1820 outs ──► FOH, 4 monitors, PFL
          ADA8200 (8 pre) ─ADAT─┘   qnu16d + JACK
                                      │  HTTP / WebSocket
                                      ▼
                             tablet · phone · laptop (any browser, several at once)
```

## Features vs the Qu-16

| | QNU16 v0.3 | Qu-16 |
|---|---|---|
| Inputs | 16 mono + 3 stereo + 4 stereo FX returns | Same |
| Mixes | LR, Mix 1–4 (mono), Mix 5-6/7-8/9-10 (stereo), FX1/FX2 sends | Same |
| Input processing | Gain/trim, polarity, HPF (20 Hz–2 kHz), gate, 4-band PEQ (width 1.5–1/9 oct, shelf/bell outer bands), compressor (Manual Peak, Manual RMS, Slow Opto, Punchbag), delay (85 ms) | Same, plus insert and ducker |
| Master processing | PEQ, 28-band GEQ with fader flip, compressor, delay | Same |
| FX | 4 engines (reverb, delay, chorus, flanger); FX3/FX4 fed from a mix | 4 engines, more types |
| RTA | 31 bands, follows PAFL, peak band in red; on Home, behind the GEQ and on the faders in fader flip | Same |
| DCAs / mute groups | 4 / 4 | 4 / 4 |
| Surface | SuperStrip; screen keys Home/Processing/Routing/FX/Scenes/Setup; layers 1-16, 17-32 and Custom; mix keys with Assign and Pre Fade; 4 SoftKeys; Copy/Paste/Reset/Fn; talkback; PAFL level | Physical surface + iPad app |
| Engineer's tools | Talkback, signal generator (sine/white/pink) | Same |
| Recording | 18-track (Ch 1–16 + LR) or stereo WAV | Qu-Drive 18-track / stereo, plus playback |
| Scenes | 100 numbered scenes, current/next, Go Next | 100, plus safes and filters |
| Outputs | 18 patchable (LR, mixes, PAFL, FX returns, direct outs) | 12 XLR + 2 Alt, plus USB |
| Motorised faders | Via an external MCU surface (roadmap) | 17 built-in |
| Latency | ≈ 5–7 ms round trip | ≈ 1–2 ms |
| Price | **≈ £540** with 16 preamps | ≈ £2,000+ |

The worst case (every processor on every channel and master, all sends, all four FX and the RTA)
uses about **13% of one x86 core**. Run `qnu16-bench` on a Pi 5 to measure it there.

## Quick start (no audio hardware needed)

```bash
sudo apt install build-essential cmake pkg-config libjack-jackd2-dev   # JACK is optional for the demo
cmake -S . -B build && cmake --build build -j
./build/qnu16-tests                       # DSP, mixer, protocol and server tests
./build/qnu16d --backend dummy            # synthetic test signals on all 16 inputs
# open http://localhost:8016/
```

## On real hardware

```bash
sudo deploy/setup-pi.sh       # deps, build, RT limits, systemd services
sudo nano /etc/default/qnu16  # set JACK_DEVICE (see `aplay -l`)
sudo reboot                   # then browse to http://<pi>.local:8016/
```

See [docs/hardware.md](docs/hardware.md) for the parts list, output mapping, latency budget and tuning.

## Offline rendering ("virtual soundcheck")

```bash
./build/qnu16-render -s scenes/Gig.scene -o mix.wav drums.wav bass.wav vox.wav
```

The input files' channels feed the 22 physical inputs in order (1–16 are Ch 1–16, 17–22 are ST1–3).
The output is outputs 1–2 (LR with the default patch), or all 18 with `--all-outputs`.

## Using the surface

The layout follows the Qu-16 top panel:

- **SuperStrip** (top left): the selected channel's gain, HPF, gate, PEQ (gain/freq/width per
  band), compressor, GEQ fader flip and pan. **IN** keys switch each block on.
- **Screen keys**: **HOME** (meters, RTA, recorder), **PROCESS** (preamp, HPF, gate, PEQ, GEQ,
  compressor, delay), **ROUTING**, **FX**, **SCENES** and **SETUP** (outputs, talkback, signal
  generator, custom layer, SoftKeys, names).
- **Layers**: **1** is Ch 1–16. **2** is ST1–3, the FX returns, the FX1/FX2 masters and the mix
  masters. **C** is the Custom layer, set up in SETUP → CUSTOM (DCA 1–4 by default).
- **Mix keys**: a mix or FX key puts its sends on the faders, with its master on the master strip.
  Latch **ASSIGN** or **PRE FADE**, then press Sel keys to assign channels or switch them
  pre/post. **GEQ FLIP** puts the selected master's GEQ on the faders, with the RTA on the meters.
- **COPY / PASTE / RESET**: latch the key, then press a Sel key (whole strip) or an IN key (one
  block). **Fn** on the Processing screen opens the factory library.
- **SoftKeys** default to mute groups 1–4. **TALK**, **PAFL LVL** and the recorder transport sit
  below them.
- Double-tap a strip name to rename it. Double-click a fader or knob to reset it. Shift-drag for
  fine control.

## Repository layout

```
engine/src/dsp/       biquad, gate, compressor, delay, GEQ, RTA (header-only, allocation-free)
engine/src/topology.* console layout (inputs, buses, output patch sources)
engine/src/strip.*    processing chain shared by inputs and masters
engine/src/fx.*       FX engines (reverb, delay, chorus, flanger)
engine/src/recorder.* multitrack / stereo WAV recorder (lock-free ring + writer thread)
engine/src/mixer.*    signal flow: sends, groups, matrices, DCAs, mute groups, PAFL, meters
engine/src/params.*   lock-free parameter registry and labels
engine/src/scene.*    scene files
engine/src/controller.*  text control protocol (docs/protocol.md)
engine/src/server/    HTTP + WebSocket server (no dependencies)
engine/src/backend/   JACK backend and a dummy backend for development
engine/tools/         qnu16d (daemon), qnu16-render, qnu16-bench
ui/                   web control surface (plain HTML/CSS/JS, no build step)
deploy/               Raspberry Pi / Debian setup and systemd units
tests/                unit and end-to-end tests
```

## Roadmap

1. **v0.4:** Mackie Control bridge for X-Touch motorised faders; recorder playback (virtual
   soundcheck); scene safes and filters.
2. **v0.5:** Channel insert and ducker, more FX types (gated reverb, ADT, phaser), user libraries.
3. **v1.0:** A custom I/O board (TDM codecs straight into the Pi's I2S) to remove the USB interface
   and cut latency to about 1.5 ms.

## License

TBD.
