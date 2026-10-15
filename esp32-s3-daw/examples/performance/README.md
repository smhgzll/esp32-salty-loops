# Performance projects

The generated 4/6/8-track projects loop four bars at 120 BPM; Pattern and Song mode work.
The chord regression fixtures use 128 BPM and should be measured in Song mode.
The source generator and hardware runner are [scripts/performance.py](../../scripts/performance.py).
Measurements and limits are in [docs/PERFORMANCE.md](../../docs/PERFORMANCE.md).

| File | Instruments | Effects | Purpose |
| --- | ---: | ---: | --- |
| `4-tracks-live.json` | 4 | 6 | Live groove, alternating 3xOSC/FM6 lead lines |
| `6-tracks-live.json` | 6 | 6 | Adds Acid303 and MacroSynth to the lead exchange |
| `8-tracks-live.json` | 8 | 6 | Adds Sampler and SoundFont; all eight engines produce audio |
| `{4,6,8}-tracks-shared.json` | 4/6/8 | 6 | Dense sustained lines, shared Filter/EQ, Delay/Reverb sends |
| `{4,6,8}-tracks.json` | 4/6/8 | 12/16/20 | Deliberate overload tests: a separate Filter/EQ on every insert |
| `chords-overlap.json` | 8 loaded, 4 sequenced | 2 | Kick, open hat, bass and three-note pad chords with overlapping 1.8 s releases |
| `three-synths-overlap.json` | 3 | 2 | Bass, pluck lead and pad play simultaneously; original envelopes and polyphony |
| `fm6-electric-piano-loop.json` | 9 loaded, 4 unmuted sequenced | 3 loaded, 2 enabled | User's 128 BPM Song loop: four-voice Electric Piano with drums, bass and Delay/Limiter; muted mono Pad retained |
| `fm6-eight-voices.json` | 1 | 0 | Eight simultaneous full six-operator Electric Piano voices, original envelope settings |

The instrument order is DrumLab, Sine, 3xOSC, FM6, Acid303, MacroSynth, Sampler, SoundFont.
Live projects use single-note lines and short releases, with the expensive leads alternating.
They use shared Filter/EQ, a Delay send, and master Compressor/Limiter/Utility. Dense projects
add sustained two-note 3xOSC/FM6/SoundFont lines and a Reverb send. The dense fixtures exceed
the tested board's 44.1 kHz capacity; a lower CPU reading with skipped notes is still a failure.
These are different arrangements, so their CPU numbers are not a controlled DSP before/after.
`fm6-eight-voices.json` is also a deliberate overload probe: the full six-operator Electric
Piano with eight release-overlapping voices exceeds the tested board's single-instance
capacity. The four-voice FM6 user-loop fixture passes after the 2026-10-12 optimization.

## Load on the board

1. Stop playback.
2. Browser → Samples: upload `perf-tone.wav` and `perf-sine.sf2` from this directory.
   The files are generated harmonic/sine fixtures; no external sound library is needed.
3. Device → Import project: choose a `*-live.json` file first. Import replaces the current
   in-memory project, so export your own work first when doing this manually.
4. Play in Pattern or Song mode. The benchmark runner does the backup/restore automatically.

## Reproduce

```sh
python3 scripts/performance.py generate
# DAW_ADMIN_PASSWORD must be set; do not put it in a command argument.
python3 scripts/performance.py board --host esp32daw.local --seconds 120 \
  --only tracks-live --out /tmp/live-performance.json
python3 scripts/performance.py board --host esp32daw.local --extended \
  --out /tmp/catalog-performance.json
# A custom project, using resources already present in the device's sample library:
python3 scripts/performance.py board --host esp32daw.local \
  --project examples/performance/8-tracks-live.json --out /tmp/custom-performance.json

build-host/bin/daw-host --render /tmp/8-tracks.wav \
  --project examples/performance/8-tracks-live.json \
  --resource-dir examples/performance --mode song --seconds 16
```

For the actual song regression, without rewriting unused sample resources:

```sh
python3 scripts/performance.py board --host esp32daw.local --mode song --seconds 120 \
  --no-upload-resources --project examples/performance/chords-overlap.json \
  --project examples/performance/three-synths-overlap.json --out /tmp/chords.json
```

The runner verifies instance counts, audio on every scheduled channel and ten error counters. It records
startup separately from the steady window. It restores the exported project, mode, position
and play/pause state in `finally`; backups are stored beside the report, outside this directory.
Hard power loss or `SIGKILL` cannot run that cleanup; retain the backup until testing finishes.
