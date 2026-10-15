# Building

Requirements (Nobara / Fedora): `cmake ninja-build gcc gcc-c++ python3 tar`. For the ESP32-S3
libraries: ESP-IDF **v5.5.5** (its xtensa-esp32s3-elf GCC 14.2). `scripts/bootstrap.sh` checks
everything, installs nothing, and records the ESP-IDF path in `.idf_path` (found from `$IDF_PATH`,
`~/.espressif/idf-env.json` or `~/esp/esp-idf`).

| Script | Output |
| --- | --- |
| `build-host.sh` | `build-host/lib/libdaw_plugin_*.a`, `build-host/bin/{dawplug,test_*}` |
| `test-host.sh` | PASS/FAIL per test, log `build-host/test.log`, renders `build-host/renders/*.wav` |
| `benchmark.sh` | `build-host/bench.jsonl` |
| `build-esp32.sh` | `build-esp32/lib/libdaw_plugin_*.a` (xtensa, ESP-IDF code-generation flags) |
| `package.sh [names]` | `dist/<id>-<version>.espplug` |

Host compiler: `$DAWSDK_HOST_CC/CXX`, else `$CC/$CXX` if installed, else gcc / clang.

## Code generation

Plugins are compiled with `-O2 -ffunction-sections -fdata-sections -ffp-contract=off` (C++ also
`-fno-exceptions -fno-rtti -fno-threadsafe-statics`). For the ESP32-S3 the toolchain file adds
ESP-IDF v5.5's flags for the chip: `-mlongcalls -mdisable-hardware-atomics -fno-builtin-memcpy
-fno-builtin-memset -fno-builtin-bzero`. `-ffp-contract=off` keeps host and target arithmetic
closer (no fused multiply-add on one side only); libm differences remain, so the DAW compares
host and target renders with tolerances, not bit for bit.

Each plugin becomes one relocatable object: `ld -r` of its objects, then
`objcopy --keep-global-symbol=<entry>`; the archive holds that object only.
