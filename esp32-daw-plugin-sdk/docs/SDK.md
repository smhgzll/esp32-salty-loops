# Writing a plugin

## C++ (recommended)

```cpp
#include "dawsdk/dsp.hpp"
#include "dawsdk/plugin.hpp"
using namespace dawsdk;

enum : uint32_t { P_GAIN, P_COUNT };
const daw_param_desc_t kParams[P_COUNT] = {
    {P_GAIN, "gain", "Gain", "Main", "dB", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, -24, 24, 0, DAW_PARAM_AUTOMATABLE, 0, nullptr}};

class MyGain : public Plugin<MyGain, P_COUNT> {
public:
    static const daw_plugin_descriptor_t *descriptor();
    void onParam(uint32_t id, float v) { g_.target = dsp::dbToGain(v); }
    void onStart(const daw_process_t &) { g_.snap(g_.target); }       // no glide from defaults
    void reset() { g_.setTime(0.01f, sampleRate()); }
    void render(const daw_process_t &c, uint32_t start, uint32_t n) {   // between events
        for (uint32_t i = start; i < start + n; i++) {
            float g = g_.next();
            c.outputs[0][i] = c.inputs[0][i] * g;
            c.outputs[1][i] = c.inputs[1][i] * g;
        }
    }
private:
    dsp::Smoother g_;
};
const daw_plugin_descriptor_t kDesc = { /* id, name, ..., */ DAWSDK_CALLBACKS(MyGain) };
const daw_plugin_descriptor_t *MyGain::descriptor() { return &kDesc; }
extern "C" const daw_plugin_descriptor_t *daw_plugin_entry_mygain(void) { return &kDesc; }
```

The adapter places the instance in host memory, initialises parameters to their defaults, clamps
incoming values (NaN included), splits each block at event offsets and routes `DAW_EV_PARAM` to
`onParam`. Hooks: `init` / `release` (allocate with `alloc<T>(n, DAW_MEM_FAST|LARGE)`), `reset`,
`onStart`, `onParam`, `onEvent`, `render`, `voices`.

Helpers: `dsp.hpp` (polynomial sine, fast exp2, PolyBLEP oscillator, ADSR, TPT SVF, RBJ biquad,
smoother, xorshift RNG, soft clip, balance pan law, denormal/NaN sanitising) and `voices.hpp`
(fixed voice pool with released-first, oldest-first stealing).

## C

See `examples/c-tremolo/tremolo.c`: define `struct daw_plugin_instance`, fill the descriptor by hand.

## Adding it to the build

1. `plugins/<name>/` (instruments) or `effects/<name>/` with the sources and a `plugin.json`
   (`name`, `id`, `entry`, `licenseFiles`, `provenance`, `dependencies`, `ui`).
2. One line in `plugins.cmake`: `daw_add_plugin(NAME <name> ENTRY <entry> DIR ... SOURCES ...)`.
3. `scripts/test-host.sh` → conformance (`dawplug validate`) runs automatically; add behaviour tests
   to `tests/test_plugins.cpp`.
4. `scripts/build-esp32.sh`, `scripts/package.sh <name>`.

## Testing a plugin by ear

```bash
build-host/bin/dawplug render 3xosc --preset 2 --seq scale --seconds 6 --out bass.wav
build-host/bin/dawplug render fx.delay --seconds 4 --out delay.wav     # effects get a test signal
build-host/bin/dawplug bench drumlab --voices 8
build-host/bin/dawplug render sampler --resource sample=piano.wav --seq chords --out piano.out.wav
```
