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

| | QNU16 v0.2 | Qu-16 |
|---|---|---|
| Inputs | 16 mono + 3 stereo + 4 stereo FX returns | Same |
| Mixes | LR, 4 mono + 3 stereo mixes, 4 stereo groups, 2 stereo matrices | Same |
| Input processing | Gain, polarity, HPF, gate, 4-band PEQ (shelf/bell), compressor (peak/RMS), delay | Same, plus insert and ducker |
| Master processing | PEQ, 28-band GEQ, compressor, delay | Same |
| FX | 4 engines: reverb, delay, chorus, flanger | 4 engines, more types |
| DCAs / mute groups | 4 / 4 | 4 / 4 |
| Outputs | 18 patchable (buses, PAFL, FX returns, direct outs) | 12 XLR + 2 alt, plus USB |
| Surface | SuperStrip, touchscreen pages, layers A/B/C, mix keys, sends on faders, master strip; any browser, several at once | Physical surface + iPad app |
| Scenes | Unlimited, plain-text files | 100 |
| Motorised faders | Via an external MCU surface (roadmap) | 17 built-in |
| Multitrack recording | Not yet (roadmap) | 18×2 USB + Qu-Drive |
| Latency | ≈ 5–7 ms round trip | ≈ 1–2 ms |
| Price | **≈ £540** with 16 preamps | ≈ £2,000+ |

The worst case (every processor on every channel and master, all sends, all four FX) uses about
**14% of one x86 core**. Run `qnu16-bench` on a Pi 5 to measure it there.

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

The layout follows the Qu-16:

- **SuperStrip** (top left): knobs for the selected channel's gain, HPF, gate, 4-band EQ,
  compressor and pan. Press **IN** to switch a section on.
- **Screen** (top right): pages for processing (EQ graph with draggable bands, gate and compressor
  curves, GEQ, delay), routing, FX, scenes, setup (output patch, names) and meters.
- **Layers A / B / C** (bottom left): channels 1–16; ST1-3, FX returns, groups and DCAs; mix masters.
- **Mix keys** (bottom right): LR shows the channel faders. A mix or FX key puts its sends on the
  faders, with its master on the master strip. A group key turns the Sel keys into group-assign
  keys. A matrix key turns the faders into matrix source levels. **MG1–4** are the mute-group masters.
- Double-tap a strip name to rename it. Double-click a fader or knob to reset it. Shift-drag for
  fine control.

## Repository layout

```
engine/src/dsp/       biquad, gate, compressor, delay, GEQ (header-only, allocation-free)
engine/src/topology.* console layout (inputs, buses, output patch sources)
engine/src/strip.*    processing chain shared by inputs and masters
engine/src/fx.*       FX engines (reverb, delay, chorus, flanger)
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

1. **v0.3:** Mackie Control bridge for X-Touch motorised faders, a custom fader layer, copy/paste/reset
   keys, and an RTA on the EQ screens.
2. **v0.4:** Multitrack record and playback (virtual soundcheck from the desk), USB-stick scene
   export, and more FX types (gated reverb, ADT, phaser).
3. **v1.0:** A custom I/O board (TDM codecs straight into the Pi's I2S) to remove the USB interface
   and cut latency to about 1.5 ms.

## License

TBD.
