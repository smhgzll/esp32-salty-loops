# Next steps

In priority order. Each step ends with its tests passing and PROGRESS.md / KNOWN_ISSUES.md updated.

1. **Complete the hardware checklist** (docs/HARDWARE_TESTS.md H1–H10): boot / I2S initialization
   passed; STA, short kick / hi-hat playback and firmware updates have partial coverage. Verify the DAC
   tones, 30-minute multi-channel playback, four copies of each effect and Wi-Fi AP / fallback. Record
   real CPU, render times, underruns and heap in docs/PERFORMANCE.md. The current 8-channel
   project's stopped load is now 9.8 %, with Kick + Hat averaging 41.6 % after optimization.
   The 136-case catalog matrix is measured; six-effect 4/6/8-track live examples pass, while
   dense sustained chains still exceed capacity. A tested optional 32 kHz profile lowers the
   clean eight-track arrangement's load by about 27 % (narrower audio bandwidth).
2. **Reverb on the critical path**: the Reverb is bound by PSRAM traffic (~22 % of a block at
   Full quality; Light, the network at half rate, 14 %: docs/PERFORMANCE.md). Open: whether Light
   should be offered when the load is high, or the default for new reverbs (a sound decision);
   16-bit lines would halve the traffic again. Also worth measuring: the master chain and routing
   tail on the audio core.
3. **Calibrate the CPU guard** (KI-8): with H4's numbers, check the 90 % guard / 75 % admission
   thresholds against real underruns, replace the plugins' estimated costs with measured ones and
   warn in the browser before adding a plugin that would not fit.
   The previous six/eight-voice 3xOSC E1M1 still skips after the DSP pass: Full has 406 skips /
   146 overruns in 180 s; Light has 172 / 1 and was restored to Full. Compare short
   peak render times, voice admission and the DrumLab → Reverb critical path before
   changing thresholds; one-second means alone miss these peaks.
4. **SoundFont channels share one font index** (KI-19): each channel parses the whole font
   (about 350 KB of PSRAM with TimGM6mb). Sharing needs memory that outlives the instance that
   allocated it, which the plugin ABI does not offer yet (an ABI 1.2 shared-resource cache, or the
   host parsing the font). Until then a GM song is limited to about 20 SoundFont parts.
5. **Tracker modules** (KI-20, docs/TRACKER.md): split songs are late in blocks because every
   Tracker channel runs the whole player at the same tick start. Options: one player instance
   shared by the channels of a song with per-channel output buffers (needs libxmp's mixer to
   write per tracker channel, and an ABI way for instances to share one player), or staggering
   the split instances' player work. Also open: `M!K!` / `FLT4` MODs (one-line loader change in
   libxmp-lite), listening tests of the 70 % default stereo separation. Dense songs: the player's
   tick (up to 3.7 ms in `ac-ootb.it`, on one core while the other waits) is now the largest
   serial part; and the 24-41 ms render block within a minute of each restart and in quick
   stop → play transitions (KI-9) needs tracing on the serial console (a play during an autosave
   could wait for it, or the autosave start a few seconds after a stop).
6. **M8 plugins**, one at a time, each with SDK tests, validation, provenance and a DAW import:
   OPL3 (optional; license decision first, docs/LICENSES.md).
7. **USB-MIDI device mode** (TinyUSB MIDI class on the S3's USB-OTG port).
8. **Long-run stability**: 24 h playback with clients connecting / disconnecting, heap watermarks,
   on the board.
9. **M11**: evaluate ELF loading in a separate branch only after the above (docs/PLUGIN_INTEGRATION.md).
