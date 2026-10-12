# QNU16 development notes

- Build and test: `cmake -S . -B build && cmake --build build -j && ./build/qnu16-tests`.
  `./build/qnu16-tests <substring>` runs a subset.
- Try the UI without hardware: `./build/qnu16d --backend dummy`, then open http://localhost:8016/.
- The audio thread (`Mixer::process`) must never allocate, lock or do I/O. Parameters are
  `std::atomic<float>` in `ParamRegistry`, which is fixed after the `Mixer` constructor. Meters
  go back to the UI through atomics.
- Topology lives in `engine/src/topology.*`. Per-channel processing parameters are registered by
  `ChannelDsp::init` (`engine/src/strip.cpp`), routing and global ones in the `Mixer` constructor,
  and FX ones in `FxEngine::init`. The UI receives every parameter's range from the engine (the `k`
  message), so it needs no mirror. Document new paths in `docs/protocol.md`.
- `ui/app.js` mirrors the Qu-16 layout: SuperStrip, screen pages, layers, mix keys, master strip.
  Views rebuild under a watcher tag (`withTag`), so handlers from old views are pruned.
- The UI is plain JS with no build step. Keep it dependency-free so it works offline on the
  desk's own Wi-Fi.
- New DSP needs a test in `tests/` that measures the real signal (as the sine-through tests do),
  not just coefficients.
