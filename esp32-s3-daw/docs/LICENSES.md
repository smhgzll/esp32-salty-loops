# Licenses

## This project

**esp32-s3-daw** — Copyright (C) 2026 smhgzll — **GNU General Public License v3.0 or later**
(`LICENSE`). Every source file carries `SPDX-License-Identifier: GPL-3.0-or-later`.

The plugin SDK (**esp32-daw-plugin-sdk**, included alongside the DAW) is **MIT**, so plugins can be
written under any compatible license. Plugins linked into this firmware become part of a GPL
binary: their licenses must be GPL-compatible (MIT, BSD, Apache-2.0, ... are).

## What a build distributes

The firmware and `daw-host` serve `/licenses.txt`, generated at build time by
`scripts/gen-notices.py` from the actual inputs (linked plugins, their shipped license files,
libraries, ESP-IDF). It is linked from the device page. Keep it that way when adding code:
**a component whose notice is not in that file must not be linked.**

## Third-party code

| component | version | license | where | in the firmware? |
|---|---|---|---|---|
| ESP-IDF (and the components it bundles: FreeRTOS, lwIP, newlib, mbedTLS, Wi-Fi / PHY libraries, ...) | v5.5.5 | Apache-2.0 (+ per-component licenses, ESP-IDF `docs/en/COPYRIGHT.rst`) | toolchain / SDK, not vendored | yes |
| espressif/mdns | 1.14.0 | Apache-2.0 | `managed_components/` (fetched by the component manager, pinned in `dependencies.lock`) | hardware profile |
| cJSON | 1.7.19 | MIT | `third_party/cJSON/` (copied from ESP-IDF's `components/json`, unmodified) | yes, and daw-host |
| plugin ABI headers | 1.4 | MIT | `third_party/daw_plugin_abi/` (including `LICENSE`) | yes (headers) |
| plugins: Sine, 3xOSC, FM6, DrumLab, Acid303, MacroSynth, Sampler, SoundFont, Tracker; Reverb, Delay, Chorus, Flanger, EQ, Filter, Compressor, Limiter, Distortion, Utility | 0.1.0 | MIT (smhgzll) | sources in the sibling SDK; generated `plugins/packages/*.espplug` | yes |
| DaisySP (drum models, SVF, oscillator, DSP utilities) inside DrumLab | commit 2c72eaf9eac5fc0dca1919d65d606da907832618 | MIT (Electrosmith) | vendored in the SDK; patch and PROVENANCE in the SDK and in the package | yes |
| TinySoundFont (SoundFont 2 engine) inside SoundFont | commit 853a0a171759f1ddba0de1442133a75912bbeffa | MIT (Bernhard Schelling) | vendored in the SDK, voice loop ported to float; patch and PROVENANCE in the SDK and in the package | yes |
| Braids macro oscillator + parts of stmlib inside MacroSynth | eurorack 08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4, stmlib e3bd7c9cc00e4364166f9905c0509b6ffd0535ec | MIT (Émilie Gillet) | vendored in the SDK; instance-owned RNG patch and PROVENANCE in the SDK and package | yes |
| Open303 (TB-303 filter, envelopes, voice logic) inside Acid303 | commit 313bf0d9ade7c1dcb6b3a74f5ea1780a29d70074 | MIT (Robin Schmidt) | vendored in the SDK, converted to float; patch and PROVENANCE in the SDK and in the package | yes |
| libxmp-lite inside Tracker | 4.7.4, commit 5d4f6aa2c7dcdf8e9cd8129fe579d45b36a302da | MIT (Claudio Matsuoka, Hipolito Carraro Jr) | vendored in the SDK; patches, LICENSE and PROVENANCE in the SDK and package | yes |
| Preact | 10.29.8 | MIT | `web/node_modules` (npm, pinned in `package-lock.json`) | yes (web bundle) |
| Vite, TypeScript | 7.3.7, 5.9.3 | MIT, Apache-2.0 | build tools only | no |
| Playwright (Python) | 1.62.0 | Apache-2.0 | browser tests only | no |
| alsa-lib | system | LGPL-2.1-or-later | dynamically linked by `daw-host` only | no |

## Planned plugins and their upstream licenses

As known from the upstream projects today — to be confirmed against each upstream LICENSE file at
the pinned commit when it is vendored. Each will get that commit, the license file, a PROVENANCE
entry and its full patch in the SDK (as DrumLab has).

| plugin | upstream | license | note |
|---|---|---|---|
| OPL3 | Nuked-OPL3 (nukeykt) | LGPL-2.1-or-later | static linking into the firmware triggers LGPL relinking obligations: ship object files or keep it out |

## Reference material

An existing local ESP-IDF v5.5.5 installation and measured PCM5102A wiring were used during
initial setup. No code was copied from the unrelated reference project.

## Music and other resources

Local SoundFonts, tracker songs, ROMs, project exports and backups are excluded from the public
source tree. The documented MIDI examples retain their source attribution and public-domain
or project license in `examples/midi/README.md`. Generated performance samples are described
in `examples/performance/README.md`. The complete repository license map is `../LICENSING.md`
from the DAW directory.

## Authorship

Owner and author: smhgzll. No AI system is an author, copyright holder or contributor of this
project.
