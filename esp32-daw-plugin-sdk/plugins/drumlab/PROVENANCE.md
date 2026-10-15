# DrumLab provenance

| Part | Source | Revision | License |
| --- | --- | --- | --- |
| Voice management, clap model, key map, parameters, presets | original (smhgzll) | — | MIT |
| Kick / snare / hi-hat models (`AnalogBassDrum`, `SyntheticBassDrum`, `AnalogSnareDrum`, `SyntheticSnareDrum`, `HiHat`), `Svf`, `Oscillator`, `Utility/dsp.h` | [DaisySP](https://github.com/electro-smith/DaisySP) (Electrosmith; the drum models are ports of Mutable Instruments Plaits by Émilie Gillet) | `2c72eaf9eac5fc0dca1919d65d606da907832618` (2026-09-28) | MIT |

Modifications to DaisySP, all in `third_party/patches/daisysp-esp32-daw.patch` (applies with
`patch -p1` to the upstream checkout and reproduces `third_party/DaisySP` exactly):

- classes that called libc `rand()` hold a per-object generator (`LocalRand`) instead, so instances
  share no state, the output is deterministic and the audio thread makes no libc call;
- the resonator quality mapping of `AnalogSnareDrum` / `AnalogBassDrum` fixed (Plaits' Q converted
  to this `Svf`'s resonance; upstream passed Q directly, which made the resonators undamped);
- `AnalogBassDrum`'s decay range restored to Plaits' 0..1; Plaits' one-pole filter restored for the
  snare's pulse low-pass;
- single-precision math on the audio path: `AnalogBassDrum::Process` used double `sin` / `cos`,
  `AnalogSnareDrum::Process` and `HiHat::SetDecay` double literals / `fmax` — software-emulated on the
  ESP32-S3, whose FPU is single precision (found by the double-math check in `scripts/build-esp32.sh`).
- `AnalogBassDrum` and `SyntheticBassDrum` cache parameter and sample-rate coefficients until
  their inputs change, removing two `powf` calls and repeated divisions from every sample.
  The analog resonator sets frequency and Plaits Q together, avoiding unused intermediate damping
  and the Q → resonance → fourth-root conversion (two more per-sample `powf` calls). For Q ≥ 1,
  its frequency clamp makes the stability limit inactive, so its division is skipped too.
  The algorithms are unchanged:
  six renders of each kick model across decay / tone / tuning extremes matched the previous
  24-bit PCM byte for byte.
- `HiHat` caches its decay, tone, clocked-noise coefficients and both filter configurations
  until a parameter changes; `SquareNoise` caches oscillator phase increments until pitch
  changes. Eight closed / open hi-hat renders across decay / tone / tuning settings matched
  the previous 24-bit PCM byte for byte.
- `AnalogSnareDrum` and `SyntheticSnareDrum` cache parameter / sample-rate coefficients,
  resonator / noise-filter settings, mode gains and static modulation factors. Both sample
  algorithms are unchanged: eight snare renders across decay / tone / tuning / snappy settings
  matched the previous 24-bit PCM byte for byte.

- No libm call and no `__divsf3` call left on the per-sample path (2026-10-12; on the ESP32-S3
  every float division is a call into libgcc and `sinf` a call with range reduction):
  `SinPiQuarter` / `SinTwoPi` (Taylor polynomials, within 2e-9 / 6e-10 of the sine) replace
  `sinf` in `Svf::SetFreqAndQ` / `Svf::SetFreq` and `SyntheticBassDrum::DistortedSine`;
  `FastDiv` (a reciprocal seed from the float bits and three Newton steps, Xtensa only; exact division
  on the host) replaces the per-sample divisions of the bass drums' diode / sine shapers and
  transistor VCA, the snare's soft clipper and the resonator's Q; `Svf::Process` and
  `SetFreqAndQ` are inline (five calls per snare sample, two per kick); `SquareNoise` updates
  its phases in place (two `memcpy` calls per hi-hat sample); `fminf` became the inline `fmin`.
  Host renders of every piece across decay / tone / tuning extremes stay within 1-8 LSB of
  24-bit PCM (at least 109 dB below the signal); hi-hats and the clap are byte-identical. On the
  board DrumLab's share in a busy rock pattern fell 32.9 → 24.0 % (the DAW's PERFORMANCE.md).

The DaisySP code is compiled into DrumLab's single relocatable object with all symbols local except
`daw_plugin_entry_drumlab`.
