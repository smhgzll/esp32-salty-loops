# Acid303 provenance

| Part | Source | Revision | License |
| --- | --- | --- | --- |
| PolyBLEP oscillator producing Open303's SAW303 / SQUARE303 waveforms, 2× halfband decimator, held-note array, DAW event handling (slide / accent / legato flags, pitch bend), parameters, presets | original (smhgzll) | — | MIT |
| Voice logic: note priority, trigger vs. slide, accent, the measured cutoff / env-mod mapping, the signal chain and its fixed filter settings | adapted from `rosic_Open303.cpp` of [Open303](https://github.com/RobinSchmidt/Open303) (Robin Schmidt) | `313bf0d9ade7c1dcb6b3a74f5ea1780a29d70074` | MIT |
| `TeeBeeFilter` (TB-303 mode), `AnalogEnvelope`, `DecayEnvelope`, `LeakyIntegrator`, `OnePoleFilter`, `BiquadFilter` and their helpers | Open303, `Source/DSPCode` | same | MIT |

## Changes against Open303

Recorded in `third_party/patches/open303-esp32-daw.patch` (applies with `patch -p1 --binary` to the
upstream checkout and reproduces `third_party/Open303`):

- `double` → `float` in every compiled file, built with `-fsingle-precision-constant`: the ESP32-S3
  FPU is single precision and double math is emulated in software there. The Xtensa object
  references no double-precision helpers (checked by `scripts/build-esp32.sh`).
- `exp2`, `log2`, `round` in `GlobalFunctions.h` disabled: they clash with the C++ library's float
  overloads, which compute the same values.

Not used, and why (`plugins/acid303/acid303.cpp` replaces them):

- `MipMappedWaveTable` / `BlendOscillator`: two 2048 × 12-level tables (≈ 200 KB in float) built
  with an FFT that allocates with `new[]`. Replaced by a PolyBLEP oscillator that produces the same
  waveforms directly: SAW303, and SQUARE303 = −tanh(69.98 · saw + 4.37) shifted by half a period,
  blended by the Waveform parameter.
- 4× oversampling with `EllipticQuarterBandFilter`, a 12th-order direct-form IIR that is not stable
  in single precision. Replaced by 2× oversampling (oscillator, pre-filter high-pass and the ladder;
  the TB-303 coefficient approximations are fitted for oversampled rates) and a 23-tap halfband FIR
  (flat to 16 kHz, −80 dB at 30 kHz).
- `std::list` of held notes (allocates on every note-on): a fixed array of 16 keys with the same
  last-note priority and slide-back on release.
- The built-in `AcidSequencer`: the DAW's step sequencer and piano roll play the notes; their
  slide and accent flags map to Open303's slide and accent.

Output level: −15 dB fixed gain after the Level parameter (Open303 uses −12 dB; 3 dB more headroom
for accented notes with the filter fully open).
