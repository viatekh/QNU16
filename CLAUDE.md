# QNU16 development notes

- Build and test: `cmake -S . -B build && cmake --build build -j && ./build/qnu16-tests`.
  `./build/qnu16-tests <substring>` runs a subset.
- Try the UI without hardware: `./build/qnu16d --backend dummy`, then open http://localhost:8016/.
- The audio thread (`Mixer::process`) must never allocate, lock or do I/O. Parameters are
  `std::atomic<float>` in `ParamRegistry`, which is fixed after the `Mixer` constructor. Meters
  go back to the UI through atomics.
- To add a parameter, register it in the `Mixer` constructor (`engine/src/mixer.cpp`), mirror its
  range in `meta()` in `ui/app.js`, and document it in `docs/protocol.md`.
- The UI is plain JS with no build step. Keep it dependency-free so it works offline on the
  desk's own Wi-Fi.
- New DSP needs a test in `tests/` that measures the real signal (as the sine-through tests do),
  not just coefficients.
