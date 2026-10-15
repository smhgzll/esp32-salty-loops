# Hardware tests

**First hardware run: 2026-10-11, firmware 0.1.0.** H1 PASS; H7 PARTIAL (STA connection,
HTTP / WebSocket and Chromium startup). H3 / H4 / H9 PARTIAL after the drum / pipeline fixes: short
single-kick and Kick + hi-hat playback, per-instance CPU measurements and healthy OTA updates passed. Analog audio,
30-minute playback and hardware rollback remain UNVERIFIED. The full 136-case plugin matrix
and 4/6/8-track live arrangements now have measured coverage (details below).
QEMU covers the firmware's logic, storage, network and
update paths (docs/QEMU.md); the results table below records the real board checks.

When you run a test, record the date, firmware version (`/api/status` → `firmware`), the result
and the measured numbers in the table at the end, and change its status to PASS / FAIL. Numbers
that matter for docs/PERFORMANCE.md (CPU, render time, underruns) come only from these runs.

## Setup

Board: ESP32-S3 N16R16 (16 MB flash, 16 MB octal PSRAM) on an adapter made for the ESP32-WROOM-32
(its pin labels are wrong for the S3). DAC: PCM5102A module.

The board connected on 2026-10-11 was detected as ESP32-S3 rev v0.2 with **32 MB octal flash
and 16 MB octal PSRAM**. Its local overrides are `DAW_FLASH_SIZE_MB=32` and
`DAW_OCTAL_FLASH=1` in `config/local.env`. The build uses OPI DTR at 80 MHz, with the existing
16 MB partition map and an unused upper 16 MB. Octal flash selection and the 80 MHz flash /
PSRAM combination follow the [ESP-IDF v5.5.5 guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/flash_psram_config.html).

| PCM5102A | ESP32-S3 GPIO | adapter label |
|----------|---------------|---------------|
| BCK | **GPIO16** | IO33 |
| LCK (WS) | **GPIO6** | IO34 |
| DIN | **GPIO7** | IO35 |
| VIN | 3V3 | |
| GND, SCK, FMT, FLT, DEMP | GND | |
| XSMT | 3V3 (unmuted) | |

Module jumpers (solder bridges on the back): 1 = L, 2 = L, 3 = H, 4 = L. SCK to GND makes the
PCM5102A generate its system clock from BCK (no MCLK wire).

These are the **measured** pins of the reference build. Do not change them to the GPIO4/5/6 of
example projects. Other pins: only after measuring your own board, through `config/local.env`
(`DAW_I2S_BCLK_GPIO`, `DAW_I2S_WS_GPIO`, `DAW_I2S_DOUT_GPIO`) — and never GPIO26–37 (octal flash /
PSRAM), 19–20 (USB), 43–44 (UART0 console), 0 / 3 / 45 / 46 (strapping).

I2S: I2S0, master, TX only, Philips format, 44100 Hz, stereo, 32-bit slots carrying signed 24-bit
samples (left-justified in the slot), DMA 4 descriptors × 256 frames, `auto_clear` on (an underrun
plays silence instead of repeating a buffer). Block size 256 frames = 5.8 ms.

```sh
cp config/local.env.example config/local.env   # set Wi-Fi + admin passwords (not committed)
scripts/build-hardware.sh                      # prints the pins it built with
scripts/flash.sh --monitor
```

## H1 — boot and audio output start (PASS, 2026-10-11)

Expected on the serial console: `esp32-s3-daw 0.1.0 (hardware profile)`, flash size matching the
board (32 MB on the tested board), `PSRAM 16 MB`, `plugins: 18 linked`,
`I2S0 master TX: 44100 Hz, Philips, 32-bit slots (24-bit data), BCLK GPIO16,
WS GPIO6, DOUT GPIO7, DMA 4 x 256`, then the Wi-Fi address. No `E (...)` lines, no panic.
If PSRAM is not found: check the module really has 16 MB octal PSRAM; the hardware profile
expects octal mode. PASS here covers initialization and running I2S without reported underruns;
the analog DAC output still needs H2.

## H2 — diagnostic tones (UNVERIFIED)

Browser → device settings → "Test signals" (or WebSocket `{"t":"diag.tone","kind":...}`).
Listen with headphones / a line input; a scope or a sound card recording is better.

| tone | expected |
|------|----------|
| `sine440` | 440 Hz at −12 dBFS on both channels, clean (no clicks every 5.8 ms = block glitches, no buzz) |
| `leftright` | 1 s of 440 Hz on the **left** only, then 1 s of 660 Hz on the **right** only, alternating. Swapped = WS polarity / wiring |
| `lowlevel` | 1 kHz at −60 dBFS: quiet but clean; distortion here means bit / format errors (FMT jumper, slot width) |
| `silence` | digital silence: only the DAC's noise floor. Hiss bursts or tones = interference from Wi-Fi / USB |
| `click` | a short 1760 Hz click every 500 ms (the metronome sound) |

Format errors show up as: loud noise (wrong bit width / FMT), an octave-shifted or distorted tone
(wrong slot width or sample rate), one channel silent (DIN on the wrong pin).

## H3 — playback stability (PARTIAL, 2026-10-11)

After the kick CPU fix, the user's project played Kick 2 for 35 s and the original analog kick
settings for 25 s: underruns / overruns / guard skips / note drops / event drops / NaN resets
stayed at zero; channel and master meters showed audio. Analog output was not listened to.
The later hi-hat / pipeline fix played the user's Kick 2 + Closed Hat pattern for 75 s with
Chromium opening the page during playback: ten error counters stayed zero, mean load 41.6 %,
both channel meters nonzero. The longer and four-client checks below remain UNVERIFIED.
The final image additionally passed 75 s on the user's subsequently selected Bassline
(23.3 % mean CPU, all ten error counters zero), plus real-board window / reload checks.

The catalog pass additionally ran each six-effect 4/6/8-track live project for 120 s on the
final 44.1 kHz image: mean DSP 65.97 / 65.88 / 68.28 %, startup and steady counter deltas zero,
every channel audible and all expected instances loaded. The generated dense/shared sustained
fixtures deliberately exceed capacity. Optional 32 kHz live fixtures pass 12 s windows at
48.08 / 48.14 / 49.83 %. These runs do not replace the 30-minute check below.

Load the demo project, song mode, loop on, play for 30 minutes, with one browser connected and the
mixer open (meters running). Expected in `/api/status` / the diagnostics page: `underruns` 0,
`overruns` 0, `audioLate` small, `cpu` stable; no audible dropouts. Record `cpu`, `renderUsAvg`,
`renderUsMax`, `budgetUs` (5805 µs per block), internal heap minimum.

Repeat with 4 browsers connected (phones included) and someone dragging notes in the piano roll.

## H4 — CPU load per plugin (PARTIAL, 2026-10-11)

Single active voice: optimized analog Kick 15.2 % per plugin / 48.8 % total render load;
Kick 2 with the Tight preset 17.6 % / 51.2 %. Both include the unchanged 8-channel project's
Limiter and Delay. After the follow-up hi-hat / pipeline fix, the same project with Kick 2 +
Closed Hat averaged 41.6 % total (Kick 2 17.6 %, Hat 10.2 %, Limiter 3.5 %, idle Delay 0.8 %),
with 9.8 % stopped load. Parameters, timings and firmware hashes are in docs/PERFORMANCE.md.
The catalog pass adds 136 hardware cases across all instruments/effects, DrumLab modes/kit
pieces, MacroSynth models, FM6 algorithms, 3xOSC waves and 4/8/max voices. All 18 default
cases pass. 129/136 pass overall; FM6 at eight voices and six dense sustained arrangements
exceed capacity. Three heaviest MacroSynth models additionally pass four-voice cases.
Raw reports, practical polyphony and 4/6/8-track examples are in docs/PERFORMANCE.md.
Four copies of every effect and predictive guard/admission calibration remain UNVERIFIED.

For each instrument: a pattern with 8 simultaneous notes (chord + hold), and with the plugin's
maximum voices; read `cpu` and `renderUsMax`. For each effect: one instance on the master with the
demo playing, then four. These numbers decide the per-plugin voice limits; record them in
docs/PERFORMANCE.md with the firmware version.

CPU guard: with MacroSynth at 4 voices on several channels (or FM6 at 8), hold chords until the
load meter reads "CPU FULL": `cpuGuardDrops` grows, `overruns` / I2S underruns stay at or near 0,
no clicks. Then `channel.add` is refused with `cpu` while the 1 s load is above 75 %. Adjust
`CONFIG_DAW_CPU_GUARD_PERMILLE` / `_ADMIT_PERMILLE` if underruns appear before the guard trips.

## H5 — flash work never interrupts audio unexpectedly (UNVERIFIED)

- While playing: Save to a slot → refused with "stop playback first" (by design).
- Stop: Save → audio idle anyway; no panic, the slot appears in the project list.
- Edit while stopped, wait 10 s: autosave (serial log), no dropouts on a running diagnostic tone?
  The tone *will* drop out during the write (cache disabled): expected, and the reason autosave
  waits for the transport to stop. Measure how long (scope on DIN).

## H6 — power loss during a save (UNVERIFIED)

Make a change, save to slot 3, and pull the USB cable within the first second of the save
(repeat 10×, at different moments). After power-up, slot 3 must load either the old or the new
project — never fail, never a mixture. (Logic verified in `tests/core/test_storage.cpp` with
simulated power cuts at every byte; this test checks the real flash.)

## H7 — Wi-Fi (PARTIAL, 2026-10-11)

AP mode: join the access point with the password from `config/local.env` (or the random one printed
on the console), open `http://192.168.4.1/` (or `http://esp32daw.local/` where mDNS works).
STA mode: the address appears on the console. Measure: first page load time, WebSocket round trip
(diagnostics page), audio stability while a phone reloads the page repeatedly.

STA connection and DHCP passed on 2026-10-11 (the local network address is omitted), as did HTTP,
admin login, WebSocket welcome / ping and Chromium startup without JavaScript errors.
Short page opening / reload checks during playback also passed with the drum / UI fixes.
AP / fallback, page load and round-trip measurements, and sustained four-client traffic remain
UNVERIFIED.

## H8 — MIDI (UNVERIFIED)

Desktop Chrome with a USB MIDI keyboard (Web MIDI): notes, sustain pedal, pitch bend, CC.
`scripts/midi-bridge.py --input hw:1,0 --host <device>` from a Linux computer: the same.
Hold a chord and close the browser tab: the notes must stop within a second.
External clock (a sequencer sending MIDI clock + Start through the bridge; receiving Start /
Continue switches the DAW to follow it): tempo locks within a few beats and follows changes,
Stop pauses, song position pointer seeks.

## H9 — firmware update and rollback over Wi-Fi (PARTIAL, 2026-10-11)

Authenticated uploads of the drum / pipeline / UI-fix hardware images succeeded. Each update switched to the
inactive OTA slot, restarted, confirmed `otaState: valid`, and restored identical exported
project JSON. The final image hash is recorded in docs/PERFORMANCE.md. Hardware rollback and
refusal of incompatible images have not been exercised on the board.

Upload `build-hw/esp32_s3_daw.bin` from the Device page (log in first). Expected: refused for a
QEMU image (`build-qemu/esp32_s3_daw.bin`); accepted for the hardware image, restart, the device
comes back on the other OTA slot (`partition` in `/api/status`), `otaState` becomes `valid` after
a few seconds; the current project is still there. The rollback path is verified in QEMU only
(the test hook does not exist in hardware builds); on hardware, a firmware that fails to start
audio within 4 s rolls back by itself.

## H10 — thermal and power (UNVERIFIED)

One hour of playback with Wi-Fi active: module temperature by touch / IR thermometer, no brown-out
resets (`boot` counter unchanged), supply current if a USB meter is at hand.

## Results

| test | date | firmware | result | numbers / notes |
|------|------|----------|--------|-----------------|
| H1 | 2026-10-11 | 0.1.0 | PASS | 32 MB octal flash, 16 MB PSRAM memory test OK; 18 plugins; I2S GPIO16 / 6 / 7; no startup error / panic; analog output not tested |
| H2 | | | UNVERIFIED | |
| H3 | 2026-10-11 | 0.1.0, drum / pipeline fix | PARTIAL | previous 60 s single-kick tests + 75 s Kick / Hat + final 75 s Bassline with UI checks; ten error counters zero in these runs; analog sound and long / four-client playback UNVERIFIED |
| H4 | 2026-10-11 | 0.1.0, drum / pipeline fix | PARTIAL | current stopped 9.8 %; Kick / Hat total 41.6 %, plugin 17.6 % / 10.2 %; earlier analog Kick 15.2 %; remaining matrix and guard calibration UNVERIFIED |
| H5 | | | UNVERIFIED | |
| H6 | | | UNVERIFIED | |
| H7 | 2026-10-11 | 0.1.0 | PARTIAL | STA DHCP, HTTP, admin login, WebSocket, Chromium startup and short page reloads while playing PASS; AP / fallback, latency measurements and sustained four-client traffic UNVERIFIED |
| H8 | | | UNVERIFIED | |
| H9 | 2026-10-11 | 0.1.0, drum / pipeline / UI fix | PARTIAL | authenticated OTA, inactive slot, healthy restart, valid image and identical project JSON; hardware rollback UNVERIFIED |
| H10 | | | UNVERIFIED | |
