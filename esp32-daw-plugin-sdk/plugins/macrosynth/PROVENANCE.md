# MacroSynth provenance

| Part | Source | Revision | License |
| --- | --- | --- | --- |
| Polyphonic voice pool, note / pitch-bend / expression handling, rendering at 2× the host rate with pitch correction, 23-tap halfband decimator, ADSR amplifier, parameters, presets | original (smhgzll) | — | MIT |
| `MacroOscillator` with `AnalogOscillator`, `DigitalOscillator` and their tables (`resources.cc`), the shape enum (`settings.h`), `excitation.h`, `svf.h`, `parameter_interpolation.h` | [Mutable Instruments eurorack](https://github.com/pichenettes/eurorack), `braids/` (Émilie Gillet) | `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4` | MIT |
| `stmlib.h`, `utils/dsp.h`, `utils/random.{h,cc}` | [stmlib](https://github.com/pichenettes/stmlib) (Émilie Gillet) | `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` | MIT |

## Changes against upstream

One, in `third_party/patches/braids-esp32-daw.patch` (applies with `patch -p1` to the eurorack
checkout and reproduces `braids/digital_oscillator.{h,cc}` and `braids/macro_oscillator.h`
exactly; every other file, stmlib included, is byte-identical to upstream):

- `DigitalOscillator` draws its noise (the plucked / blown / fluted / vowel / noise models...) from
  a generator state its owner sets (`set_random_state`, also on `MacroOscillator`) instead of
  stmlib's static `Random`: same algorithm, but MacroSynth instances share no state, so two can
  render at the same time on the ESP32-S3's two cores (the DAW used to keep them on one core).
  The voices of one instance share its state, so a single instance's output is byte-identical to
  before (seven presets and five noise-using models rendered and compared).

Braids is integer DSP, so the Xtensa object references no double-precision helpers (checked by
`scripts/build-esp32.sh`).

## How the module's code is driven

- Braids' tables are computed for its 96 kHz output rate. The voices run at twice the host rate
  (88.2 kHz at 44.1 kHz, exactly 96 kHz at 48 kHz); the pitch passed to `set_pitch()` (1/128
  semitone) is offset by `128 · 12 · log2(96000 / (2 · rate))` (+188 at 44.1 kHz) so notes are in
  tune, and a halfband FIR takes the output down to the host rate.
- `Render()` is called with at most 24 samples, Braids' own block size (`temp_buffer_[24]`).
- `Strike()` on every note-on excites the physical and percussive models (plucked, struck, kick...).
- Timbre and Color (0..1) map to Braids' 0..32767 parameters, smoothed per block.
- Each instance holds the noise generator state its oscillators draw from (the patch above),
  reseeded at every `activate()` so renders are reproducible.
- `MacroOscillator` is 17 KB; the four voices' oscillators are allocated in `DAW_MEM_LARGE`
  (PSRAM on the ESP32-S3).

Not used: Braids' envelope, quantizer, signature waveshaper, VCA, settings storage, UI and the
STM32 drivers.
