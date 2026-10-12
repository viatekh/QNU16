# QNU16 control protocol

The control protocol is plain text over a WebSocket at `ws://<host>:8016/ws`. Each WebSocket
message carries one or more commands separated by newlines. Any client can use it, including the
bundled web UI, scripts, a MIDI bridge or show-control software.

## Client → engine

| Command | Effect |
|---|---|
| `set <path> <value>` | Set a parameter. The value is clamped to its range, and toggles snap to 0/1. |
| `label <key> <text>` | Name a channel or bus (`ch/1`..`ch/16`, `aux/1`..`aux/4`, `dca/1`..`dca/4`, `main`). Empty text clears the name. |
| `state` | Request a full parameter and label dump. |
| `info` | Request engine info. |
| `scene list` | List stored scenes. |
| `scene save <name>` | Store the current state. Names are 1–48 characters from `A–Z a–z 0–9 space - _`. |
| `scene load <name>` | Recall a scene. Anything the scene doesn't mention resets to its default. |

## Engine → client

| Message | Meaning |
|---|---|
| `p <path> <value>` | A parameter changed. Changes are broadcast to every client, including the sender, so several tablets stay in sync. |
| `l <key> <text>` | A label changed. |
| `i {json}` | Engine info: channels, auxes, DCAs, sample rate, backend and output names. Sent on connect. |
| `s <current>\n<name>\n…` | Scene list. The first line holds the current scene name. |
| `m v,v,…` | Meters, sent every 50 ms. For each channel 1–16: `in dB, post-fader dB, gain reduction dB, gate open (0/1)`. Then the 8 outputs: `main L, main R, aux 1–4, PFL L, PFL R`. Levels are dBFS peak, with −90 meaning silence. |
| `e <text>` | An error. It goes only to the client that caused it. |

On connect, the engine sends `i`, then a full `p`/`l` dump, then `s`.

## Parameter paths

Channels are `N` = 1–16, aux buses `A` = 1–4, DCAs `D` = 1–4, and EQ bands `B` = 1–4. Band 1 is a low
shelf, bands 2–3 are peaking, and band 4 is a high shelf.

| Path | Range | Default |
|---|---|---|
| `ch/N/trim` | −24 … +24 dB | 0 |
| `ch/N/polarity` | toggle | 0 |
| `ch/N/hpf/on`, `ch/N/hpf/freq` | toggle; 20 … 400 Hz | 0; 80 |
| `ch/N/gate/on` | toggle | 0 |
| `ch/N/gate/thresh`, `range` | −80 … 0 dB; 0 … 80 dB | −50; 40 |
| `ch/N/gate/attack`, `hold`, `release` | 0.05 … 50; 0 … 500; 5 … 2000 ms | 0.5; 50; 150 |
| `ch/N/eq/on` | toggle | 1 |
| `ch/N/eq/B/freq`, `gain`, `q` | 20 … 20000 Hz; ±15 dB; 0.3 … 10 | 100/400/2.5k/8k; 0; 0.707/1/1/0.707 |
| `ch/N/comp/on` | toggle | 0 |
| `ch/N/comp/thresh`, `ratio`, `knee`, `makeup` | −60 … 0 dB; 1 … 20; 0 … 12 dB; 0 … 24 dB | −20; 3; 6; 0 |
| `ch/N/comp/attack`, `release` | 0.1 … 100; 10 … 2000 ms | 10; 100 |
| `ch/N/pan` | −1 (L) … +1 (R), constant power | 0 |
| `ch/N/fader` | −90 (−∞) … +10 dB | 0 |
| `ch/N/mute`, `solo`, `main` | toggles | 0, 0, 1 |
| `ch/N/aux/A/send`, `ch/N/aux/A/pre` | −90 … +10 dB; toggle | −∞; 1 (pre-fader) |
| `ch/N/dca/D` | toggle (assign) | 0 |
| `aux/A/fader`, `aux/A/mute` | −90 … +10 dB; toggle | 0; 0 |
| `dca/D/fader`, `dca/D/mute` | −90 … +10 dB; toggle | 0; 0 |
| `main/fader`, `main/mute` | −90 … +10 dB; toggle | 0; 0 |

## Signal flow per channel

```
input → trim/polarity → [in meter] → HPF → gate → 4-band EQ → compressor
      → pre-fader point ──┬─→ PFL bus (when soloed; ignores mute)
                          └─→ pre-fader aux sends (× mute)
      → fader × mute × DCAs ─┬─→ post-fader aux sends
                             └─→ pan → main L/R
```

The PFL outputs carry the soloed channels when any channel is soloed. Otherwise they carry the
main mix, so headphones always hear something.

## Example (Python)

```python
import asyncio, websockets  # pip install websockets

async def main():
    async with websockets.connect("ws://qnu16.local:8016/ws") as ws:
        await ws.send("set ch/1/fader -6\nset ch/1/comp/on 1\nlabel ch/1 Kick")
        async for msg in ws:
            if not msg.startswith("m "):
                print(msg[:80])

asyncio.run(main())
```
