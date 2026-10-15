# Licenses and third-party code

The SDK (headers, helpers, host, tools, scripts) is MIT, © smhgzll. Each plugin declares its own
SPDX license in its descriptor and package; a firmware that links plugins must satisfy all of
them (the DAW firmware is GPL-3.0-or-later, compatible with MIT / Apache-2.0 / LGPL-2.1+ code).

## Vendored code

| Code | Upstream | Revision | License | Used by | Modifications |
| --- | --- | --- | --- | --- | --- |
| DaisySP: `Drums/*`, `Filters/svf.*`, `Synthesis/oscillator.*`, `Utility/dsp.h` | https://github.com/electro-smith/DaisySP | `2c72eaf9eac5fc0dca1919d65d606da907832618` (2026-09-28) | MIT (Electrosmith; drum models ported from Mutable Instruments Plaits, MIT) | DrumLab | `third_party/patches/daisysp-esp32-daw.patch`: per-object RNG instead of libc `rand()`; resonator Q mapping and bass-drum decay range fixed; Plaits one-pole restored; no libm / division calls per sample (details: `plugins/drumlab/PROVENANCE.md`) |
| Open303: `TeeBeeFilter`, `AnalogEnvelope`, `DecayEnvelope`, `LeakyIntegrator`, `OnePoleFilter`, `BiquadFilter`, `GlobalDefinitions.h`, `GlobalFunctions.*`, `RealFunctions.*`, `NumberManipulations.*` (+ `rosic_Open303.*` unmodified, as the reference the voice logic was adapted from) | https://github.com/RobinSchmidt/Open303 | `313bf0d9ade7c1dcb6b3a74f5ea1780a29d70074` (2024-03-29) | MIT (Robin Schmidt) | Acid303 | `third_party/patches/open303-esp32-daw.patch`: double → float; `exp2` / `log2` / `round` left to the C++ library (details: `plugins/acid303/PROVENANCE.md`) |
| TinySoundFont: `tsf.h` | https://github.com/schellingb/TinySoundFont | `853a0a171759f1ddba0de1442133a75912bbeffa` | MIT (Bernhard Schelling) | SoundFont | `third_party/patches/tinysoundfont-esp32-daw.patch`: voice loop in single precision (details: `plugins/soundfont/PROVENANCE.md`) |
| Braids: `braids/macro_oscillator.*`, `analog_oscillator.*`, `digital_oscillator.*`, `resources.*`, `settings.h`, `excitation.h`, `svf.h`, `parameter_interpolation.h`; stmlib: `stmlib.h`, `utils/dsp.h`, `utils/random.*` | https://github.com/pichenettes/eurorack, https://github.com/pichenettes/stmlib | `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4`, `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` | MIT (Émilie Gillet) | MacroSynth | `third_party/patches/braids-esp32-daw.patch`: the digital oscillator's noise generator state is owned per instance instead of stmlib's static `Random` (details: `plugins/macrosynth/PROVENANCE.md`) |
| libxmp (the libxmp-lite file set: `include/xmp.h`, `src/*`, `src/loaders/{common,itsex,sample,xm_load,mod_load,s3m_load,it_load}.*`) | https://github.com/libxmp/libxmp | tag `libxmp-4.7.4`, `5d4f6aa2c7dcdf8e9cd8129fe579d45b36a302da` (2026-10-10) | MIT (Claudio Matsuoka, Hipolito Carraro Jr) | Tracker | `third_party/patches/libxmp-esp32-daw.patch`: a per-row hook in the scan (the row map for seeking), a carried fraction of a sample in the tick length, deferred mixing in parts; `plugins/tracker/xmp_shim.h` (force-included) redirects its C library calls (details: `plugins/tracker/PROVENANCE.md`) |

## Original effects and their published references

Reverb, Chorus, Flanger, EQ, Compressor and Distortion contain no third-party code. Published
designs they follow: the Schroeder / Moorer comb-and-all-pass reverb as in Jezar's public-domain
Freeverb (its delay lengths are used as tuning constants), the RBJ Audio EQ Cookbook (biquad
formulas), Giannoulis, Massberg & Reiss, "Digital Dynamic Range Compressor Design" (JAES 2012, soft
knee), Parker, Zavalishin & Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using
Continuous-Time Convolution" (DAFx 2016, ADAA).

## Reviewed for upcoming plugins (not vendored yet)

| Project | Revision checked | License | Planned use |
| --- | --- | --- | --- |
| Nuked-OPL3 (nukeykt) | `765ec962` | LGPL-2.1 | optional OPL3 |
| Dexed (asb2m10) | `eebb748b` | GPL-3.0 (the msfa engine files are Apache-2.0) | reference for FM6 |
| Synth_Dexed (probonopd) | `17249e56` | to be confirmed per file before use | reference for FM6 |
