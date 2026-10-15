// SPDX-License-Identifier: MIT
// Plugin conformance checks (dawplug validate). Every check either passes or records a failure
// message; warnings do not fail.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#include "dawsdk_host/host.hpp"
#include "validate.hpp"

using namespace dawhost;

namespace {

struct Ctx {
    std::vector<std::string> fails, warns;
    void fail(const std::string &m) { fails.push_back(m); }
    void warn(const std::string &m) { warns.push_back(m); }
};

bool allFinite(const std::vector<float> &x)
{
    for (float v : x)
        if (!(v == v) || fabsf(v) > 1e30f)
            return false;
    return true;
}

std::vector<float> testInput(uint32_t sr, double seconds, bool left)
{
    std::vector<float> x((size_t)(sr * seconds));
    uint32_t s = left ? 12345u : 54321u;
    for (size_t i = 0; i < x.size(); i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        double t = (double)i / sr;
        double burst = fmod(t, 0.5) < 0.1 ? 1.0 : 0.0;
        x[i] = (float)(0.3 * sin(2 * M_PI * 220.0 * t) + 0.2 * burst * ((int32_t)s / 2147483648.0));
    }
    return x;
}

std::vector<TimedEvent> defaultSequence(const daw_plugin_descriptor_t *d)
{
    if (d->capabilities & DAW_CAP_DRUM_KIT)
        return sequenceDrums(2, 120);
    if (d->capabilities & DAW_CAP_POLYPHONIC) {
        auto s = sequenceChords(60, 4, 0.5);
        return s;
    }
    return sequenceScale(48, 16, 0.125, 0.1, 0.8f);
}

RenderResult renderAny(Instance &in, const std::vector<TimedEvent> &ev, double seconds, uint32_t block = 256)
{
    if (in.desc()->kind == DAW_PLUGIN_EFFECT) {
        auto l = testInput(in.sampleRate(), seconds, true), r = testInput(in.sampleRate(), seconds, false);
        return render(in, ev, seconds, block, &l, &r);
    }
    return render(in, ev, seconds, block);
}

bool validId(const char *s)
{
    if (!s || !*s || strlen(s) > 96 || !strchr(s, '.'))
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '_'))
            return false;
    return true;
}

bool validSemver(const char *s)
{
    int a, b, c;
    return s && sscanf(s, "%d.%d.%d", &a, &b, &c) == 3;
}

void checkDescriptor(const daw_plugin_descriptor_t *d, Ctx &c)
{
    if (!daw_plugin_abi_compatible(d)) {
        c.fail("ABI: incompatible descriptor (api " + std::to_string(d->api_major) + "." + std::to_string(d->api_minor) + ")");
        return;
    }
    if (!validId(d->id))
        c.fail(std::string("id: not a lower-case reverse-DNS id: ") + (d->id ? d->id : "(null)"));
    if (!validSemver(d->version))
        c.fail("version: not semantic (x.y.z)");
    for (const char *s : {d->name, d->vendor, d->license, d->category, d->description})
        if (!s || !*s)
            c.fail("descriptor: name / vendor / license / category / description must be set");
    if (d->kind != DAW_PLUGIN_INSTRUMENT && d->kind != DAW_PLUGIN_EFFECT)
        c.fail("kind: unknown");
    if (d->kind == DAW_PLUGIN_INSTRUMENT && d->max_voices == 0)
        c.fail("max_voices: an instrument needs at least 1");
    if ((d->capabilities & DAW_CAP_POLYPHONIC) && d->max_voices < 2)
        c.fail("POLYPHONIC with max_voices < 2");
    if ((d->capabilities & DAW_CAP_STATE_BLOB) && (!d->save_state || !d->load_state))
        c.fail("STATE_BLOB without save_state/load_state");
    for (auto fn : {(void *)d->create, (void *)d->destroy, (void *)d->activate, (void *)d->deactivate,
                    (void *)d->process, (void *)d->set_param, (void *)d->get_param})
        if (!fn)
            c.fail("descriptor: a mandatory function pointer is NULL");
    std::set<uint32_t> ids;
    std::set<std::string> keys;
    for (uint32_t i = 0; i < d->param_count; i++) {
        const daw_param_desc_t &p = d->params[i];
        std::string k = p.key ? p.key : "";
        if (!ids.insert(p.id).second)
            c.fail("param " + k + ": duplicate id");
        if (k.empty() || !keys.insert(k).second)
            c.fail("param #" + std::to_string(i) + ": missing or duplicate key");
        if (!p.name || !*p.name)
            c.fail("param " + k + ": no display name");
        if (!(p.min < p.max))
            c.fail("param " + k + ": min must be < max");
        if (p.def < p.min || p.def > p.max)
            c.fail("param " + k + ": default outside min..max");
        if (p.type > DAW_PARAM_ENUM || p.scale > DAW_SCALE_BIPOLAR)
            c.fail("param " + k + ": unknown type / scale");
        if (p.scale == DAW_SCALE_LOG && p.min <= 0)
            c.fail("param " + k + ": log scale needs min > 0");
        if (p.type == DAW_PARAM_ENUM) {
            if (!p.enum_labels || p.enum_count == 0)
                c.fail("param " + k + ": enum without labels");
            else if ((uint32_t)(p.max - p.min + 1.5f) != p.enum_count)
                c.fail("param " + k + ": enum_count must equal max-min+1");
            else
                for (uint32_t e = 0; e < p.enum_count; e++)
                    if (!p.enum_labels[e])
                        c.fail("param " + k + ": NULL enum label");
        }
    }
    for (uint32_t i = 0; i < d->preset_count; i++) {
        const daw_preset_desc_t &pr = d->presets[i];
        if (!pr.name || !*pr.name)
            c.fail("preset #" + std::to_string(i) + ": no name");
        if (pr.values)
            for (uint32_t k = 0; k < d->param_count; k++)
                if (pr.values[k] < d->params[k].min || pr.values[k] > d->params[k].max)
                    c.fail(std::string("preset ") + pr.name + ": value of " + d->params[k].key + " out of range");
    }
}

void checkLifecycle(const daw_plugin_descriptor_t *d, Ctx &c)
{
    for (uint32_t sr : {22050u, 44100u, 48000u}) {
        {
            Instance a(d, sr, 256);
            if (!a.ok()) {
                c.fail("create/activate failed at " + std::to_string(sr) + " Hz");
                return;
            }
            if (sr == 44100) {
                if (a.host().fastBytes > d->mem_fast_bytes)
                    c.warn("FAST memory " + std::to_string(a.host().fastBytes) + " B exceeds the declared estimate " +
                           std::to_string(d->mem_fast_bytes) + " B");
                if (a.host().largeBytes > d->mem_large_bytes)
                    c.warn("LARGE memory " + std::to_string(a.host().largeBytes) + " B exceeds the declared estimate " +
                           std::to_string(d->mem_large_bytes) + " B");
            }
        }
    }
    // repeated create / destroy, every block returned
    for (int i = 0; i < 20; i++) {
        HostServices hs;
        daw_plugin_instance_t *p = d->create(&hs.api, 44100, 256);
        if (!p) {
            c.fail("create failed on iteration " + std::to_string(i));
            return;
        }
        d->activate(p);
        d->deactivate(p);
        d->destroy(p);
        if (hs.liveBlocks != 0) {
            c.fail("leak: " + std::to_string(hs.liveBlocks) + " blocks / " + std::to_string(hs.liveBytes) +
                   " bytes still allocated after destroy()");
            return;
        }
    }
}

void checkRender(const daw_plugin_descriptor_t *d, Ctx &c)
{
    const bool instr = d->kind == DAW_PLUGIN_INSTRUMENT;
    // 1. silence in, silence out (instrument, no events)
    if (instr) {
        Instance a(d);
        auto r = render(a, {}, 0.5);
        if (peak(r.l) > 1e-6f || peak(r.r) > 1e-6f)
            c.fail("an instrument without notes is not silent");
    }
    // 2. the default sequence: finite, audible, sane level, no allocation in process()
    Instance a(d);
    auto seq = defaultSequence(d);
    auto r = renderAny(a, seq, 2.5);
    if (!allFinite(r.l) || !allFinite(r.r))
        c.fail("NaN / Inf in the output of the default sequence");
    if (instr && rms(r.l) + rms(r.r) < 1e-4)
        c.fail("the default sequence is (nearly) silent");
    if (peak(r.l) > 4.0f || peak(r.r) > 4.0f)
        c.warn("peak above +12 dBFS in the default sequence");
    if (a.host().rtViolations)
        c.fail("process() allocated, freed or logged (" + std::to_string(a.host().rtViolations) + " calls)");
    // 3. determinism
    Instance b(d);
    auto r2 = renderAny(b, seq, 2.5);
    if (hashPcm(r.l, r.r) != hashPcm(r2.l, r2.r))
        c.fail("two instances render the same input differently (non-deterministic)");
    // 4. the event offset is honoured: nothing before a note starting at frame 100
    if (instr) {
        Instance e(d);
        std::vector<float> L(256), R(256);
        daw_event_t on = noteOn(d->capabilities & DAW_CAP_DRUM_KIT ? 36 : 60, 1.0f, 1);
        on.offset = 100;
        e.process(256, {on}, L.data(), R.data());
        float pre = 0, post = 0;
        for (int i = 0; i < 100; i++)
            pre = fmaxf(pre, fmaxf(fabsf(L[i]), fabsf(R[i])));
        for (int i = 100; i < 256; i++)
            post = fmaxf(post, fmaxf(fabsf(L[i]), fabsf(R[i])));
        if (pre > 1e-6f)
            c.fail("sound before the note-on offset (sample-accurate events not honoured)");
        if (post <= 1e-6f) {
            // slow attacks are legal; look at the next block
            e.process(256, {}, L.data(), R.data());
            for (int i = 0; i < 256; i++)
                post = fmaxf(post, fabsf(L[i]));
            if (post <= 1e-7f)
                c.warn("no output within 512 frames of a note-on");
        }
        // 5. all sound off: silent within one block, no voices
        daw_event_t off{};
        off.type = DAW_EV_ALL_SOUND_OFF;
        e.process(256, {off}, L.data(), R.data());
        e.process(256, {}, L.data(), R.data());
        if (peak(L) > 1e-4f || peak(R) > 1e-4f)
            c.fail("still sounding one block after ALL_SOUND_OFF");
        if (e.voices() != 0)
            c.fail("voices still active after ALL_SOUND_OFF");
    }
    // 6. all notes off: every voice ends (release <= 10 s)
    if (instr) {
        Instance e(d);
        std::vector<TimedEvent> ev;
        for (int k = 0; k < 6; k++)
            ev.push_back({0.01 * k, noteOn((uint8_t)(48 + k * 5), 0.9f, k)});
        daw_event_t off{};
        off.type = DAW_EV_ALL_NOTES_OFF;
        ev.push_back({0.5, off});
        auto rr = render(e, ev, 10.5);
        std::vector<float> tail(rr.l.end() - 4410, rr.l.end());
        if (peak(tail) > 1e-4f)
            c.fail("still sounding 10 s after ALL_NOTES_OFF (stuck voice)");
    }
    // 7. extreme parameters stay finite
    {
        Instance e(d);
        for (int pass = 0; pass < 3; pass++) {
            for (uint32_t i = 0; i < d->param_count; i++) {
                const daw_param_desc_t &p = d->params[i];
                float v = pass == 0 ? p.min : (pass == 1 ? p.max : p.min + (p.max - p.min) * (float)((i * 7919) % 101) / 100.0f);
                e.setParam(p.id, v);
            }
            auto rr = renderAny(e, defaultSequence(d), 1.0);
            if (!allFinite(rr.l) || !allFinite(rr.r)) {
                c.fail(std::string("NaN / Inf with all parameters at ") + (pass == 0 ? "min" : pass == 1 ? "max" : "mixed values"));
                break;
            }
            if (peak(rr.l) > 16.0f || peak(rr.r) > 16.0f)
                c.warn(std::string("peak above +24 dBFS with parameters at ") + (pass == 0 ? "min" : pass == 1 ? "max" : "mixed values"));
        }
    }
    // 8. out-of-range and NaN parameter values are clamped
    {
        Instance e(d);
        for (uint32_t i = 0; i < d->param_count; i++) {
            const daw_param_desc_t &p = d->params[i];
            e.setParam(p.id, p.max + 1000.0f);
            float v = e.getParam(p.id);
            if (!(v >= p.min && v <= p.max))
                c.fail(std::string("param ") + p.key + ": an out-of-range value is not clamped");
            e.setParam(p.id, NAN);
            v = e.getParam(p.id);
            if (!(v >= p.min && v <= p.max))
                c.fail(std::string("param ") + p.key + ": NaN is not rejected");
        }
    }
    // 9. every preset renders
    for (uint32_t i = 0; i < d->preset_count; i++) {
        Instance e(d);
        e.loadPreset(i);
        auto rr = renderAny(e, defaultSequence(d), 1.0);
        if (!allFinite(rr.l) || !allFinite(rr.r))
            c.fail(std::string("preset ") + d->presets[i].name + ": NaN / Inf");
        if (instr && rms(rr.l) + rms(rr.r) < 1e-5)
            c.warn(std::string("preset ") + d->presets[i].name + ": silent with the default sequence");
    }
    // 10. voices end after their notes (no stuck notes)
    if (instr && d->active_voices) {
        Instance e(d);
        auto rr = render(e, defaultSequence(d), 12.0);
        std::vector<float> tail(rr.l.end() - 44100, rr.l.end());
        if (peak(tail) > 1e-4f)
            c.fail("output 10 s after the last note-off (stuck voice or endless tail)");
        if (e.voices() != 0)
            c.fail(std::to_string(e.voices()) + " voices still active 10 s after the last note-off");
    }
}

}  // namespace

// a synthetic sample for AUDIO slots: 1.5 s, a decaying saw at C4 (root key 60), 44.1 kHz stereo;
// FILE slots: a test SoundFont (sf2) or tracker module (mod)
static const daw_resource_t *syntheticResource(const daw_resource_slot_desc_t &slot)
{
    static AudioFile a;
    static FileResource sf2, mod;
    if (!(slot.types & DAW_RES_MASK(DAW_RES_AUDIO))) {
        if ((slot.types & DAW_RES_MASK(DAW_RES_FILE)) && slot.extensions && strstr(slot.extensions, "mod")) {
            if (mod.bytes.empty()) {
                mod.bytes = makeTestModule("mod");
                mod.name = "test.mod";
            }
            return mod.resource();
        }
        if (!(slot.types & DAW_RES_MASK(DAW_RES_FILE)) || !slot.extensions || !strstr(slot.extensions, "sf2"))
            return nullptr;
        if (sf2.bytes.empty()) {
            sf2.bytes = makeTestSoundFont();
            sf2.name = "test.sf2";
        }
        return sf2.resource();
    }
    if (a.channels == 0) {
        a.channels = 2;
        a.rate = 44100;
        a.rootKey = 60;
        a.name = "synthetic.wav";
        for (int c = 0; c < 2; c++) {
            a.ch[c].resize(66150);
            double ph = 0;
            for (size_t i = 0; i < a.ch[c].size(); i++) {
                ph += 261.6256 / 44100.0;
                ph -= floor(ph);
                a.ch[c][i] = (float)((2 * ph - 1) * 0.5 * exp(-(double)i / 30000.0));
            }
        }
    }
    return a.resource();
}

// ABI 1.1: resource slots described sanely; without resources the plugin is silent and safe
static void checkResources(const daw_plugin_descriptor_t *d, Ctx &c)
{
    if (!daw_plugin_has_resources(d))
        return;
    for (uint32_t s = 0; s < d->resource_slot_count; s++) {
        const auto &sl = d->resource_slots[s];
        if (!sl.key || !*sl.key || !sl.name || !sl.types)
            c.fail("resource slot " + std::to_string(s) + ": missing key, name or types");
    }
    setDefaultResources(nullptr);
    {
        Instance a(d);
        auto r = render(a, defaultSequence(d), 1.0);
        if (!allFinite(r.l) || peak(r.l) > 1e-6f || peak(r.r) > 1e-6f)
            c.fail("without its resources the plugin is not silent (or not finite)");
        // a wrong resource type is refused
        FileResource f;
        f.bytes = {1, 2, 3, 4};
        for (uint32_t s = 0; s < d->resource_slot_count; s++)
            if (!(d->resource_slots[s].types & DAW_RES_MASK(DAW_RES_FILE)) && a.setResource(s, f.resource()) == DAW_OK)
                c.fail("resource slot " + std::to_string(s) + " accepted a resource type it does not declare");
    }
    setDefaultResources(syntheticResource);
}

ValidationResult validatePlugin(const daw_plugin_descriptor_t *d)
{
    setDefaultResources(syntheticResource);
    Ctx c;
    checkResources(d, c);
    checkDescriptor(d, c);
    if (c.fails.empty()) {
        checkLifecycle(d, c);
        checkRender(d, c);
    }
    return {c.fails, c.warns};
}
