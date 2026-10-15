// SPDX-License-Identifier: GPL-3.0-or-later
// Test helpers: a probe instrument that logs every event with its absolute sample position, and a
// harness that runs an Engine with a project the way daw::Session does (without the protocol).
#pragma once

#include <math.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "daw/compile.hpp"
#include "daw/engine.hpp"
#include "daw/plugin_host.hpp"

struct ProbeEvent {
    uint64_t at;  // absolute sample
    uint16_t type, flags;
    uint8_t key;
    int32_t id;
    float vel, value;
    uint32_t param;
    int slot;
    uint32_t mbeats;  // NOTE_ON with DAW_NOTE_FLAG_CHASE: elapsed 1/1000 quarter notes
};
inline std::vector<ProbeEvent> g_probe;
inline void (*g_probeHook)(int, const daw_process_t *) = nullptr;

struct daw_plugin_instance {
    const daw_host_api_t *host;
    int slot;
    float level;
    int held;
};

namespace probe {
inline int g_nextSlot = 0;
inline daw_plugin_instance_t *create(const daw_host_api_t *h, uint32_t, uint32_t)
{
    auto *p = (daw_plugin_instance_t *)h->alloc(h->host_ctx, sizeof(daw_plugin_instance_t), 8, DAW_MEM_FAST | DAW_MEM_ZERO);
    p->host = h;
    p->slot = g_nextSlot++;
    return p;
}
inline void destroy(daw_plugin_instance_t *p) { p->host->free(p->host->host_ctx, p); }
inline int32_t activate(daw_plugin_instance_t *) { return DAW_OK; }
inline void deactivate(daw_plugin_instance_t *) {}
inline void process(daw_plugin_instance_t *p, const daw_process_t *c)
{
    if (g_probeHook) g_probeHook(p->slot, c);
    for (uint32_t i = 0; i < c->frames; i++)
        c->outputs[0][i] = c->outputs[1][i] = 0;
    for (uint32_t i = 0; i < c->event_count; i++) {
        const daw_event_t &e = c->events[i];
        ProbeEvent pe{c->sample_pos + e.offset, e.type, e.flags, e.u.note.key, e.u.note.note_id, e.u.note.velocity, 0, 0, p->slot,
                      e.type == DAW_EV_NOTE_ON && (e.flags & DAW_NOTE_FLAG_CHASE) ? e.u.note.chase_mbeats : 0};
        if (e.type == DAW_EV_PARAM) {
            pe.param = e.u.param.id;
            pe.value = e.u.param.value;
        }
        g_probe.push_back(pe);
        if (e.type == DAW_EV_NOTE_ON) {
            c->outputs[0][e.offset] = 0.5f;  // a one-sample click marks the note
            p->held++;
        }
        if (e.type == DAW_EV_NOTE_OFF && p->held > 0)
            p->held--;
        if (e.type == DAW_EV_ALL_NOTES_OFF || e.type == DAW_EV_ALL_SOUND_OFF)
            p->held = 0;
    }
}
inline void setp(daw_plugin_instance_t *p, uint32_t, float v) { p->level = v; }
inline float getp(const daw_plugin_instance_t *p, uint32_t) { return p->level; }
inline uint32_t voices(const daw_plugin_instance_t *p) { return p->held > 0 ? 1u : 0u; }
inline const daw_param_desc_t kParams[] = {
    {0, "level", "Level", "", "", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0, 1, 0.5f, DAW_PARAM_AUTOMATABLE, 0, nullptr}};
inline const daw_plugin_descriptor_t kDesc = {
    sizeof(daw_plugin_descriptor_t), DAW_PLUGIN_API_MAJOR, DAW_PLUGIN_API_MINOR, "test.probe", "Probe", "test", "0.0.1", "MIT",
    "event recorder", "Test", DAW_PLUGIN_INSTRUMENT, 1, DAW_CAP_MIDI_STREAM | DAW_CAP_POLYPHONIC | DAW_CAP_SLIDE, 64, 1, kParams, 0, nullptr, nullptr,
    64, 0, 0, 0, create, destroy, activate, deactivate, process, setp, getp, nullptr, nullptr, voices};
// the same recorder asking for note chase (ABI 1.2), for channels whose plugin id is "test.chase"
inline const daw_plugin_descriptor_t kChaseDesc = {
    sizeof(daw_plugin_descriptor_t), DAW_PLUGIN_API_MAJOR, DAW_PLUGIN_API_MINOR, "test.chase", "Chase probe", "test", "0.0.1", "MIT",
    "event recorder (note chase)", "Test", DAW_PLUGIN_INSTRUMENT, 1, DAW_CAP_NOTE_CHASE, 1, 1, kParams, 0, nullptr, nullptr,
    64, 0, 0, 0, create, destroy, activate, deactivate, process, setp, getp, nullptr, nullptr, voices};
}  // namespace probe

// runs an engine on a project; every channel gets a probe instance (slot = channel index)
struct Harness {
    daw::Engine eng;
    daw::Project proj;
    std::vector<float> L, R;
    uint64_t pos = 0;
    uint32_t version = 1;
    explicit Harness(daw::Project p, uint32_t block = 256) : eng(daw::Engine::Config{44100, block}), proj(std::move(p))
    {
        g_probe.clear();
        probe::g_nextSlot = 0;
        for (size_t i = 0; i < proj.channels.size(); i++) {
            auto *inst = daw::PluginInstance::create(proj.channels[i].inst.pluginId == "test.chase" ? &probe::kChaseDesc : &probe::kDesc,
                                                     44100, block);
            inst->activate();
            post(daw::EngineCmd{daw::EngineCmd::SetInstrument, (uint8_t)i, 0, 0, 0, {.p = inst}});
            daw::EngineCmd lv{daw::EngineCmd::ChannelLevel, (uint8_t)i, 0, 0, 0};
            lv.f[0] = 1.0f;
            lv.f[1] = 0;
            post(lv);
        }
        daw::EngineCmd t{daw::EngineCmd::TrackLevel, 0, 0, 0, 0};
        t.f[0] = 1.0f;
        post(t);
        recompile();
        daw::EngineCmd b{daw::EngineCmd::SetTempo};
        b.d = proj.bpm;
        post(b);
        daw::EngineCmd ts{daw::EngineCmd::SetTimeSig, proj.ts.num, proj.ts.den};
        post(ts);
    }
    void post(const daw::EngineCmd &c)
    {
        if (!eng.post(c))
            abort();
    }
    void recompile()
    {
        auto slotOf = [&](daw::Id ch) { return proj.channelIndex(ch); };
        auto resolve = [&](const daw::AutomationTarget &t, daw::PlayAuto &a) {
            if (t.kind == daw::AutomationTarget::ChannelParam) {
                a.kind = daw::AT_CHAN_PARAM;
                a.slot = (uint8_t)proj.channelIndex(t.channel);
                a.paramId = 0;
                a.min = 0;
                a.max = 1;
                a.scale = DAW_SCALE_LINEAR;
                return true;
            }
            if (t.kind == daw::AutomationTarget::Tempo) {
                a.kind = daw::AT_TEMPO;
                a.min = 60;
                a.max = 180;
                return true;
            }
            return false;
        };
        auto *m = daw::compilePlay(proj, slotOf, resolve, version++);
        post(daw::EngineCmd{daw::EngineCmd::SwapPlay, 0, 0, 0, 0, {.p = m}});
        auto *mix = daw::compileMix(proj);
        post(daw::EngineCmd{daw::EngineCmd::SwapMix, 0, 0, 0, 0, {.p = mix}});
    }
    void cmd(uint8_t type, uint8_t a = 0, uint32_t u = 0)
    {
        daw::EngineCmd c{type, a, 0, 0, u};
        post(c);
    }
    void run(double seconds)
    {
        const uint32_t block = eng.config().blockFrames;
        uint64_t n = (uint64_t)(seconds * 44100);
        std::vector<float> l(block), r(block);
        for (uint64_t done = 0; done < n; done += block) {
            eng.render(l.data(), r.data(), block);
            L.insert(L.end(), l.begin(), l.end());
            R.insert(R.end(), r.begin(), r.end());
            pos += block;
            daw::Retired rt;
            while (eng.popRetired(rt)) {
                if (rt.kind == daw::Retired::Play)
                    daw::freePlay((const daw::PlayModel *)rt.p);
                else if (rt.kind == daw::Retired::Mix)
                    daw::freeMix((const daw::MixModel *)rt.p);
                else
                    daw::PluginInstance::destroy((daw::PluginInstance *)rt.p);
            }
        }
    }
    std::vector<ProbeEvent> ons(int slot = -1) const
    {
        std::vector<ProbeEvent> v;
        for (auto &e : g_probe)
            if (e.type == DAW_EV_NOTE_ON && (slot < 0 || e.slot == slot))
                v.push_back(e);
        return v;
    }
    std::vector<ProbeEvent> offs(int slot = -1) const
    {
        std::vector<ProbeEvent> v;
        for (auto &e : g_probe)
            if (e.type == DAW_EV_NOTE_OFF && (slot < 0 || e.slot == slot))
                v.push_back(e);
        return v;
    }
};

// a project with `n` channels and one empty pattern
inline daw::Project testProject(int n, double bpm = 120)
{
    daw::Project p;
    p.bpm = bpm;
    p.ensureMixer();
    for (int i = 0; i < n; i++) {
        daw::Channel c;
        c.id = p.newId();
        c.name = "ch" + std::to_string(i);
        c.inst.pluginId = "test.probe";
        c.track = 0;
        p.channels.push_back(c);
    }
    daw::Pattern pat;
    pat.id = p.newId();
    pat.name = "P1";
    p.patterns.push_back(pat);
    p.currentPattern = pat.id;
    p.normalise();
    return p;
}
