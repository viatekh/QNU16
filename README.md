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

## Features (v0.1)

| | QNU16 v0.1 | Qu-16 |
|---|---|---|
| Mic inputs | 16 (with UMC1820 + ADA8200) | 16 |
| Per-channel processing | Trim, polarity, HPF, gate, 4-band PEQ, compressor | Same, plus delay |
| Mixes | Main L/R, 4 aux (pre/post per send), PFL | Main L/R, 4 mono + 3 stereo mixes |
| DCAs / mute groups | 4 DCAs (with mute) | 4 DCAs, 4 mute groups |
| Scenes | Unlimited, plain-text files | 100 |
| Remote control | Any browser, multiple clients in sync, open protocol | iPad app |
| Built-in FX | Not yet (roadmap) | 4 FX engines |
| Multitrack recording | Not yet (roadmap; JACK makes it easy) | 18×2 USB |
| Motorised faders | Via external MCU surface (roadmap) | 17 built-in |
| Latency | ≈5 ms round trip | ≈1–2 ms |
| Price | **≈ £540** with 16 preamps | ≈ £2,000+ |

The DSP is cheap. The full 16-channel worst case (every processor on, every send active) uses about
**6% of one x86 core**. Run `qnu16-bench` on a Pi 5 to measure it there.

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

The input files' channels feed mixer channels 1–16 in order. The output is the main L/R mix, or
all 8 outputs with `--all-outputs`.

## Using the surface

- **MAIN / AUX 1–4** (top bar) choose what the faders control. In an aux layer, the faders become
  that mix's sends ("sends on faders"), and **PRE** toggles pre/post-fader per channel.
- **SEL** opens the channel processing panel: input/HPF/routing, gate, EQ with a live response
  curve, compressor with gain reduction, and sends.
- Tap a channel or bus name to rename it. Double-click a fader to set it to 0 dB. Shift-drag a
  fader for fine control.
- **STORE / RECALL** save and load scenes. Every connected device stays in sync.

## Repository layout

```
engine/src/dsp/       biquad, gate, compressor (header-only, allocation-free)
engine/src/mixer.*    16-channel mixer: signal flow, buses, DCAs, PFL, meters
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

1. **v0.2:** FX engines (reverb, delay) on 2 internal FX buses, channel delay, mute groups, and
   stereo aux pairs.
2. **v0.3:** Mackie Control bridge for X-Touch motorised faders, and a MIDI learn function.
3. **v0.4:** Multitrack record and playback (virtual soundcheck from the desk), plus USB-stick
   scene export.
4. **v1.0:** A custom I/O board (TDM codecs, for example CS42448 or AK4619, straight into the Pi's
   I2S) to remove the USB interface and cut latency to about 1.5 ms.

## License

TBD.
