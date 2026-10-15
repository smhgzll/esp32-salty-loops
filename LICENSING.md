# Licensing and included resources

Copyright © 2026 [smhgzll](https://github.com/smhgzll) for the original project code.

This repository contains components under different licenses. Moving the DAW and SDK into
one repository preserves their existing license boundaries and upstream notices.

| Component | License | License text / attribution |
| --- | --- | --- |
| DAW firmware, portable engine, web application, Linux host and root helper scripts | GPL-3.0-or-later | [LICENSE](LICENSE), [DAW license](esp32-s3-daw/LICENSE) |
| Plugin SDK, helpers, tools and original bundled plugins | MIT | [SDK license](esp32-daw-plugin-sdk/LICENSE) |
| Copied plugin ABI headers | MIT | [ABI license](esp32-s3-daw/third_party/daw_plugin_abi/LICENSE) |
| cJSON | MIT | [License](esp32-s3-daw/third_party/cJSON/LICENSE) |
| DaisySP components used by DrumLab | MIT, with upstream notices | [License](esp32-daw-plugin-sdk/third_party/DaisySP/LICENSE), [provenance](esp32-daw-plugin-sdk/plugins/drumlab/PROVENANCE.md) |
| Open303 components used by Acid303 | MIT | [License](esp32-daw-plugin-sdk/third_party/Open303/License.txt), [provenance](esp32-daw-plugin-sdk/plugins/acid303/PROVENANCE.md) |
| TinySoundFont used by SoundFont | MIT | [License](esp32-daw-plugin-sdk/third_party/TinySoundFont/LICENSE), [provenance](esp32-daw-plugin-sdk/plugins/soundfont/PROVENANCE.md) |
| Braids and stmlib components used by MacroSynth | MIT | [Notices](esp32-daw-plugin-sdk/plugins/macrosynth/LICENSE.Braids), [provenance](esp32-daw-plugin-sdk/plugins/macrosynth/PROVENANCE.md) |
| libxmp-lite used by Tracker | MIT | [License](esp32-daw-plugin-sdk/third_party/libxmp/LICENSE), [provenance](esp32-daw-plugin-sdk/plugins/tracker/PROVENANCE.md) |

The full dependency inventories, pinned revisions and local modifications are documented in
[DAW licenses](esp32-s3-daw/docs/LICENSES.md) and [SDK licenses](esp32-daw-plugin-sdk/docs/LICENSES.md).
Vendored source, license texts, provenance files and patches remain in the source tree.
ESP-IDF, managed ESP components and npm dependencies are obtained during setup/build;
the build generates a `/licenses.txt` page for the linked software and browser runtime.

## Example music and samples

- `groove-format0.mid` and `groove-format1.mid` are generated original compositions under
  GPL-3.0-or-later.
- The three Mutopia MIDI examples have documented public-domain status, source links,
  typesetter attribution and hashes in [examples/midi/README.md](esp32-s3-daw/examples/midi/README.md).
- `perf-tone.wav` and `perf-sine.sf2` are generated test resources from this project, under
  GPL-3.0-or-later. Their generator and use are described in
  [examples/performance/README.md](esp32-s3-daw/examples/performance/README.md).
- The interface screenshot is project documentation. It does not bundle the music or
  SoundFonts shown in the interface.

Local SoundFonts, downloaded MOD/XM/S3M/IT songs, ROM images, private project exports and
backups are excluded by `.gitignore`. The project code license does not grant rights to
third-party music or sound banks that a user imports.

When adding a dependency or example resource, retain its license and attribution and update
the relevant inventory. Firmware distributions must also include the corresponding source
and notices required by their component licenses.
