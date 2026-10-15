// SPDX-License-Identifier: GPL-3.0-or-later
// Compare an actual worker thread with the serial engine through chord overlaps,
// shared routing, parameter events, live replacements, removals and transport changes.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string.h>
#include <limits>
#include "check.hpp"
#include "daw/session.hpp"
#include "daw/plugin_host.hpp"
#include "../../host/render_worker.hpp"
using namespace daw;

// Misbehaving test plugins: an instrument that outputs NaN while a note sounds, an effect that
// outputs +Inf. The engine must contain both, on either core.
namespace bad {
struct Inst { bool on = false; };
daw_plugin_instance_t *create(const daw_host_api_t *, uint32_t, uint32_t) { return reinterpret_cast<daw_plugin_instance_t *>(new Inst); }
void destroy(daw_plugin_instance_t *i) { delete reinterpret_cast<Inst *>(i); }
int32_t activate(daw_plugin_instance_t *) { return DAW_OK; }
void deactivate(daw_plugin_instance_t *) {}
uint32_t voices(const daw_plugin_instance_t *i) { return reinterpret_cast<const Inst *>(i)->on ? 1 : 0; }
void processNan(daw_plugin_instance_t *i, const daw_process_t *c)
{
    auto &st = *reinterpret_cast<Inst *>(i);
    for (uint32_t e = 0; e < c->event_count; e++) {
        if (c->events[e].type == DAW_EV_NOTE_ON) st.on = true;
        if (c->events[e].type == DAW_EV_ALL_SOUND_OFF) st.on = false;
    }
    for (uint32_t f = 0; f < c->frames; f++)
        c->outputs[0][f] = c->outputs[1][f] = st.on && f == c->frames / 2 ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
}
void processInf(daw_plugin_instance_t *, const daw_process_t *c)
{
    for (uint32_t f = 0; f < c->frames; f++) {
        c->outputs[0][f] = c->inputs[0][f];
        c->outputs[1][f] = f == 7 ? std::numeric_limits<float>::infinity() : c->inputs[1][f];
    }
}
daw_plugin_descriptor_t make(const char *id, uint32_t kind, void (*process)(daw_plugin_instance_t *, const daw_process_t *))
{
    daw_plugin_descriptor_t d{};
    d.struct_size = sizeof d;
    d.api_major = DAW_PLUGIN_API_MAJOR;
    d.api_minor = DAW_PLUGIN_API_MINOR;
    d.id = id;
    d.name = id;
    d.version = "0.0.0";
    d.kind = kind;
    d.max_voices = 1;
    d.create = create;
    d.destroy = destroy;
    d.activate = activate;
    d.deactivate = deactivate;
    d.process = process;
    d.active_voices = voices;
    return d;
}
}  // namespace bad

int main()
{
    RenderWorker thread;
    struct Forward { RenderWorker *thread; unsigned jobs = 0; } forward{&thread};
    Engine::Worker worker{&forward,
        [](void *p, void (*fn)(void *), void *arg) {
            auto &f = *static_cast<Forward *>(p); ++f.jobs;
            f.thread->api.start(f.thread->api.context, fn, arg);
        },
        [](void *p) { auto &f = *static_cast<Forward *>(p); f.thread->api.join(f.thread->api.context); }};
    Engine serial({44100, 256}), parallel({44100, 256, 0, &worker});
    Session a(serial, {}), b(parallel, {});
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream file(root / "examples/performance/chords-overlap.json");
    std::stringstream stream; stream << file.rdbuf();
    const auto project = stream.str();
    std::string error;
    CHECK(a.importProject(project.data(), project.size(), &error));
    CHECK(b.importProject(project.data(), project.size(), &error));
    auto post = [&](const EngineCmd &cmd) { CHECK(serial.post(cmd)); CHECK(parallel.post(cmd)); };
    post({EngineCmd::SetMode, 1}); post({EngineCmd::Play});
    bool equal = true;
    float al[256], ar[256], bl[256], br[256];
    for (unsigned block = 0; block < 3600; ++block) {
        if (block == 1600) {
            EngineCmd cmd{EngineCmd::ChannelLevel, 4, 0, 7}; cmd.f[0] = .7f; cmd.f[1] = -.2f;
            post(cmd);  // bass and pad now sum into the same track, in their original order
        }
        if (block == 1800) {
            EngineCmd cmd{EngineCmd::Param, 0, 6}; cmd.u32 = 23; cmd.f[0] = 1700;
            post(cmd);
        }
        if (block == 2000) {
            EngineCmd cmd{EngineCmd::SetInstrument, 6};
            const auto *desc = PluginRegistry::find("org.smhgzll.3xosc");
            cmd.p = PluginInstance::create(desc, 44100, 256); CHECK(cmd.p); CHECK(static_cast<PluginInstance *>(cmd.p)->activate()); CHECK(serial.post(cmd));
            cmd.p = PluginInstance::create(desc, 44100, 256); CHECK(cmd.p); CHECK(static_cast<PluginInstance *>(cmd.p)->activate()); CHECK(parallel.post(cmd));
        }
        if (block == 2400) { EngineCmd cmd{EngineCmd::SetInstrument, 4}; cmd.p = nullptr; post(cmd); }
        if (block == 2600) post({EngineCmd::Pause});
        if (block == 2700) post({EngineCmd::Continue});
        if (block == 3000) post({EngineCmd::Stop});
        if (block == 3050) post({EngineCmd::Play});
        if (block == 3500) post({EngineCmd::NotesOff, 1});
        serial.render(al, ar, 256); parallel.render(bl, br, 256);
        if (memcmp(al, bl, sizeof al) || memcmp(ar, br, sizeof ar)) { equal = false; break; }
        if (block % 32 == 0) { a.tick(block * 6); b.tick(block * 6); }
    }
    CHECK(equal);
    CHECK(forward.jobs > 1000);
    CHECK(serial.status().voices == parallel.status().voices);
    CHECK(parallel.status().nanResets == 0);
    CHECK(parallel.status().cpuGuardDrops == 0);
    CHECK(parallel.status().cmdOverflow == 0);
    {
        // Two MacroSynth instances (Braids): each draws noise from its own generator state, so
        // they may render on both cores at once and the mix still matches the serial renderer
        // bit for bit (the plucked model uses the generator on every strike).
        const unsigned before = forward.jobs;
        Engine one({44100, 256}), two({44100, 256, 0, &worker});
        const auto *desc = PluginRegistry::find("org.smhgzll.macrosynth");
        for (Engine *e : {&one, &two})
            for (uint8_t slot = 0; slot < 2; ++slot) {
                EngineCmd install{EngineCmd::SetInstrument, slot};
                auto *inst = PluginInstance::create(desc, 44100, 256);
                CHECK(inst);
                CHECK(inst->activate());
                CHECK(inst->setParam("model", 28.0f));  // plucked string
                install.p = inst;
                CHECK(e->post(install));
            }
        bool same = true;
        for (unsigned block = 0; block < 400; ++block) {
            if (block % 20 == 0)
                for (Engine *e : {&one, &two})
                    for (uint8_t slot = 0; slot < 2; ++slot) {
                        EngineCmd note{EngineCmd::Note, slot, (uint8_t)(48 + slot * 7 + block / 20 % 12), 1};
                        note.f[0] = .8f;
                        CHECK(e->post(note));
                    }
            one.render(al, ar, 256);
            two.render(bl, br, 256);
            same &= !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar);
        }
        CHECK(same);
        CHECK(two.status().voices >= 2);
        CHECK(forward.jobs > before);  // and they did run on the worker
    }
    // Effect chains on both cores: insert chains, send buses, a shared bus and the FM6 loop,
    // in pattern and song mode, must match the serial renderer bit for bit; the worker must
    // actually have taken effect chains.
    for (const char *name : {"8-tracks.json", "8-tracks-shared.json", "6-tracks.json", "fm6-electric-piano-loop.json", "4-tracks-live.json"})
        for (int mode = 0; mode < 2; ++mode) {
            std::ifstream f(root / "examples/performance" / name);
            std::stringstream ss;
            ss << f.rdbuf();
            const auto text = ss.str();
            Engine es({44100, 256}), ep({44100, 256, 0, &worker});
            Session sa(es, {}), sb(ep, {});
            CHECK(sa.importProject(text.data(), text.size(), &error));
            CHECK(sb.importProject(text.data(), text.size(), &error));
            for (Engine *e : {&es, &ep}) {
                CHECK(e->post({EngineCmd::SetMode, (uint8_t)mode}));
                CHECK(e->post({EngineCmd::Play}));
            }
            const unsigned before = forward.jobs;
            bool same = true;
            double energy = 0;
            for (unsigned block = 0; block < 1400 && same; ++block) {
                es.render(al, ar, 256);
                ep.render(bl, br, 256);
                same = !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar);
                for (int i = 0; i < 256; ++i)
                    energy += (double)al[i] * al[i];
                if (block % 32 == 0) {
                    sa.tick(block * 6);
                    sb.tick(block * 6);
                }
            }
            if (!same)
                fprintf(stderr, "%s mode %d: parallel output differs\n", name, mode);
            CHECK(same);
            CHECK(energy > 1.0);                          // the arrangement plays
            // effect chains at the same routing depth (inserts, or a delay and a reverb bus) start
            // the second phase in every block; series chains (live: one shared bus) and the FM6
            // loop (its delay runs with its channel strip) only offload instruments
            const bool parallelChains = !strstr(name, "fm6") && !strstr(name, "live");
            CHECK(forward.jobs - before > (parallelChains ? 2700u : strstr(name, "fm6") ? 0u : 1000u));
            CHECK(ep.status().nanResets == 0 && ep.status().fxFailures == 0);
        }
    // A minimum job size keeps small instruments on the audio core: same output, no hand-offs
    // for the chord fixture (its chains are in series, so no effect phase either).
    {
        Engine es({44100, 256}), ep({44100, 256, 0, &worker, 1000});
        Session sa(es, {}), sb(ep, {});
        CHECK(sa.importProject(project.data(), project.size(), &error));
        CHECK(sb.importProject(project.data(), project.size(), &error));
        for (Engine *e : {&es, &ep}) {
            CHECK(e->post({EngineCmd::SetMode, 1}));
            CHECK(e->post({EngineCmd::Play}));
        }
        const unsigned before = forward.jobs;
        bool same = true;
        for (unsigned block = 0; block < 400 && same; ++block) {
            es.render(al, ar, 256);
            ep.render(bl, br, 256);
            same = !memcmp(al, bl, sizeof al) && !memcmp(ar, br, sizeof ar);
        }
        CHECK(same);
        CHECK(forward.jobs == before);
    }
    // NaN from an instrument and Inf from an effect are contained on both cores: the block is
    // muted / the effect taken out of the chain, the output stays finite, the rest keeps playing.
    {
        static const daw_plugin_descriptor_t nanInst = bad::make("test.nan", DAW_PLUGIN_INSTRUMENT, bad::processNan);
        static const daw_plugin_descriptor_t infFx = bad::make("test.inf", DAW_PLUGIN_EFFECT, bad::processInf);
        for (Engine::Worker *w : {(Engine::Worker *)nullptr, &worker}) {
            Engine e({44100, 256, 0, w});
            const auto *sine = PluginRegistry::find("org.smhgzll.sine");
            for (uint8_t slot = 0; slot < 3; ++slot) {  // sine, NaN, sine: both cores get work
                EngineCmd install{EngineCmd::SetInstrument, slot};
                install.p = PluginInstance::create(slot == 1 ? &nanInst : sine, 44100, 256);
                CHECK(install.p);
                CHECK(static_cast<PluginInstance *>(install.p)->activate());
                CHECK(e.post(install));
                EngineCmd note{EngineCmd::Note, slot, (uint8_t)(57 + slot), 1};
                note.f[0] = .8f;
                CHECK(e.post(note));
            }
            for (int t = 1; t <= 2; ++t) {  // two insert chains with the bad effect: one per core
                EngineCmd route{EngineCmd::ChannelLevel, (uint8_t)(t == 1 ? 0 : 2), 0, (uint8_t)t};
                route.f[0] = .7f;
                CHECK(e.post(route));
                EngineCmd fx{EngineCmd::SetFx, (uint8_t)t, 0};
                fx.p = PluginInstance::create(&infFx, 44100, 256);
                CHECK(fx.p);
                CHECK(static_cast<PluginInstance *>(fx.p)->activate());
                CHECK(e.post(fx));
            }
            bool finite = true;
            double energy = 0;
            for (unsigned block = 0; block < 64; ++block) {
                e.render(al, ar, 256);
                for (int i = 0; i < 256; ++i) {
                    finite &= std::isfinite(al[i]) && std::isfinite(ar[i]);
                    energy += (double)al[i] * al[i];
                }
            }
            CHECK(finite);
            CHECK(energy > 1.0);                       // the sines still play
            CHECK(e.status().nanResets >= 3);          // the NaN block, then each effect once
            CHECK(e.status().fxFailures == 2);         // both effects taken out, dry passes
        }
    }
    TEST_MAIN_END("test_parallel");
}
