# Roadmap

The long view; the short, ordered list is NEXT_STEPS.md, the state is PROGRESS.md.

## 0.1 (now)

A complete pattern-and-song DAW on the device: channel rack with step sequencer, piano roll,
playlist with automation clips and their editor, mixer with effects and sends, eighteen plugins (eight instruments including a sampler, a SoundFont player and the Braids models, ten effects), a sample library, projects with
power-safe storage, multi-browser editing on desktop and phone, MIDI from browsers and Linux,
browser firmware updates with rollback. Verified on Linux and in QEMU; the board is next.

## 0.2 — on the board

- Hardware checklist run, real performance numbers, fixes from them.
- CPU guard thresholds checked on the board; plugin costs measured and shown before adding.

## 0.3 — sound palette (M8)

- Instruments: optional OPL3.
- Per-engine resource figures measured on the board and published in the plugin catalog.

## 0.4 — performance features

- Step recording (notes entered one step at a time while stopped).
- USB-MIDI device mode.
- Pattern clips with per-clip transpose, more clip operations; arrangement markers as jump targets.
- Per-note expression editing for plugins that support it.

## Later / research

- HTTPS on the device (memory permitting) or a pairing-based token scheme.
- Full-device backup and restore through the `stage` partition.
- **M11 — dynamic plugin loading**: an ELF loader prototype in a separate branch; signed packages
  only; off by default; documented as fully trusting the signer. Never at the cost of the stable
  core.

## Not planned

- Audio input / recording (the PCM5102A is output only; would need a codec board).
- External displays or SD cards (the browser is the display; storage is on-board flash).
