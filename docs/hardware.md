# Hardware guide

QNU16 runs on any Linux machine with JACK. The target is a small, silent box with a class-compliant
multichannel USB interface, controlled from a tablet.

## Reference build (16 mic inputs)

| Part | Example | Approx. price | Notes |
|---|---|---|---|
| Computer | Raspberry Pi 5 (8 GB) + active cooler + 27 W PSU | £90 | Or an Intel N100 mini PC (~£130): more headroom, often steadier USB audio |
| Storage | 32 GB A2 microSD or a USB SSD | £10 | |
| Interface | Behringer UMC1820 | £230 | 8 mic preamps, 10 outputs, ADAT in/out, class compliant |
| Expander | Behringer ADA8200 | £180 | 8 more preamps over ADAT, giving 16 in total |
| Wi-Fi AP | Any dual-band access point | £30 | A dedicated network for the tablet. Don't rely on venue Wi-Fi |
| Control | Any tablet or phone browser | – | |
| **Total** | | **≈ £540** | |

An Allen & Heath Qu-16 costs about £2,000+.

### Optional hands-on control

| Part | Price | Status |
|---|---|---|
| Behringer X-Touch (9 motorised faders, Mackie Control) | ~£450 | Planned (MCU bridge on the roadmap) |
| Behringer X-Touch Compact (16 motorised faders) | ~£300 | Planned |
| Any MIDI fader box (e.g. Korg nanoKONTROL2) | ~£50 | Planned |

## Output mapping

| JACK port | UMC1820 output | Typical use |
|---|---|---|
| `main_L`, `main_R` | 1–2 | FOH (front-of-house) amps or speakers |
| `aux_1` … `aux_4` | 3–6 | Monitor wedges or in-ears |
| `pfl_L`, `pfl_R` | 7–8 | Engineer's headphones (PFL, or the main mix when nothing is soloed) |

## Latency budget (48 kHz)

| Stage | Time |
|---|---|
| ADC + DAC (interface) | ~1.0 ms |
| JACK buffer, 64 × 3 periods | ~4.0 ms |
| ADAT hop (channels 9–16 only) | ~0.1 ms |
| **Round trip** | **≈ 5 ms** |

Dedicated consoles are typically ~1–2 ms. 5 ms is fine for front-of-house and wedges and noticeable on in-ear vocals. To reduce it:
use `JACK_PERIOD=32` on an x86 box, or move to the custom I/O board on the roadmap.

## Tuning checklist

- Plug the interface into a **USB 3 (blue) port** directly, with no hub.
- Disable Wi-Fi power saving on the box (`iw dev wlan0 set power_save off`), or better, use Ethernet
  to the access point.
- Set the CPU governor to `performance`. `deploy/setup-pi.sh` does this.
- Watch for xruns (dropouts): `journalctl -u qnu16-jackd -f`.
- Run `qnu16-bench` on the target. A Pi 5 should report well under 30% load for the full 16-channel
  worst case.
