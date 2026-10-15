// SPDX-License-Identifier: GPL-3.0-or-later
// The protocol through daw::Session, with real plugins and a running engine: handshake, every
// family of operations, validation, two clients, stale edits, stuck-note prevention, persistence.
#include <math.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
#include "check.hpp"
#include "daw/project_json.hpp"
#include "daw/session.hpp"
#include "module_fixture.hpp"
#include "sf2_fixture.hpp"

using namespace daw;

struct Out {
    int client;
    std::string text;
    MsgClass cls;
};

struct Rig {
    Engine eng;
    MemFlash flash{8 * 2 * 128 * 1024};
    ProjectStore store{flash, 8};
    MemFlash sampleFlash{128 * 4096};
    SampleStore samples{sampleFlash};
    MemFlash presetFlash{32 * 4096};
    SampleStore presetStore{presetFlash};
    PresetLibrary presets{presetStore};
    Session ses;
    std::vector<Out> out;
    uint32_t now = 0;
    double energy = 0;  // sum of squares of everything rendered
    int nextReq = 1;
    explicit Rig(bool withStore = true, bool flashStallsAudio = false, uint32_t cpuGuard = 0, uint32_t cpuAdmit = 0,
                 bool advanceAudioOnWait = false)
        : eng(Engine::Config{44100, 256, cpuGuard}),
          ses(eng, Session::Config{"test", "0.1.0", "host", withStore ? &store : nullptr, withStore ? &samples : nullptr, withStore ? &presets : nullptr, 10000, nullptr, nullptr, flashStallsAudio, cpuAdmit,
                                  advanceAudioOnWait ? std::function<void()>([this] {
                                      float l[256], r[256]; eng.render(l, r, 256);
                                  }) : std::function<void()>{}})
    {
        samples.mount();
        presets.mount();
        ses.setSender([this](int c, const std::string &t, MsgClass cls) { out.push_back({c, t, cls}); });
    }
    void render(double seconds)
    {
        float l[256], r[256];
        for (int i = 0; i < (int)(seconds * 44100 / 256); i++) {
            eng.render(l, r, 256);
            for (int k = 0; k < 256; k++)
                energy += (double)l[k] * l[k] + (double)r[k] * r[k];
            if (i % 4 == 0) {
                now += 23;
                ses.tick(now);
            }
        }
        ses.tick(now += 20);
    }
    // send a request, return the ack / err for it
    cJSON *req(int client, const std::string &json)
    {
        cJSON *j = cJSON_Parse(json.c_str());
        const int id = nextReq++;
        cJSON_AddNumberToObject(j, "id", id);
        std::string t = jsonText(j);
        size_t before = out.size();
        ses.handleText(client, t.data(), t.size());
        for (size_t i = before; i < out.size(); i++) {
            cJSON *m = cJSON_Parse(out[i].text.c_str());
            const cJSON *mid = cJSON_GetObjectItem(m, "id");
            if (out[i].client == client && mid && (int)mid->valuedouble == id)
                return m;
            cJSON_Delete(m);
        }
        return nullptr;
    }
    bool ok(int client, const std::string &json, double *newId = nullptr)
    {
        cJSON *a = req(client, json);
        bool good = a && !strcmp(cJSON_GetObjectItem(a, "t")->valuestring, "ack");
        if (!good && a)
            fprintf(stderr, "  %s -> %s\n", json.c_str(), cJSON_PrintUnformatted(a));
        if (newId && a && cJSON_GetObjectItem(a, "newId"))
            *newId = cJSON_GetObjectItem(a, "newId")->valuedouble;
        cJSON_Delete(a);
        return good;
    }
    std::string err(int client, const std::string &json)
    {
        cJSON *a = req(client, json);
        std::string code = a && cJSON_GetObjectItem(a, "code") ? cJSON_GetObjectItem(a, "code")->valuestring : "";
        cJSON_Delete(a);
        return code;
    }
    int count(const char *type, const char *path = nullptr, int client = -2)
    {
        int n = 0;
        for (auto &o : out) {
            if (client != -2 && o.client != client)
                continue;
            cJSON *m = cJSON_Parse(o.text.c_str());
            const cJSON *t = cJSON_GetObjectItem(m, "t"), *p = cJSON_GetObjectItem(m, "path");
            if (t && !strcmp(t->valuestring, type) && (!path || (p && !strcmp(p->valuestring, path))))
                n++;
            cJSON_Delete(m);
        }
        return n;
    }
};

static std::string sfmt(const char *f, double a = 0, double b = 0, double c = 0, double d = 0)
{
    char buf[512];
    snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}

int main()
{
    // handshake: nothing before hello; wrong protocol refused; welcome carries the full state
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        CHECK(r.err(1, R"({"t":"play"})") == "no_hello");
        CHECK(r.err(1, R"({"t":"hello","proto":99})") == "proto");
        CHECK(r.ok(1, R"({"t":"hello","proto":1})"));
        CHECK(r.count("welcome") == 1);
        cJSON *w = cJSON_Parse(r.out[r.out.size() - 2].text.c_str());
        CHECK(w && cJSON_GetObjectItem(w, "project") && cJSON_GetArraySize(cJSON_GetObjectItem(w, "plugins")) >= 7);
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(w, "project"), "channels")) == 8);
        cJSON_Delete(w);
        r.ses.handleText(1, "{garbage", 8);
        CHECK(r.out.back().text.find("\"code\":\"bad_json\"") != std::string::npos);
        CHECK(r.err(1, R"({"t":"no.such.op"})") == "unknown_op");
        std::string huge(kMaxMessageBytes + 10, ' ');
        r.ses.handleText(1, huge.data(), huge.size());
        CHECK(r.count("err") >= 1);
    }
    // transport and status: the demo project plays and makes sound through the real plugins
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        CHECK(r.ok(1, R"({"t":"play"})"));
        r.render(2.0);
        CHECK(r.eng.status().playing.load() == 1);
        CHECK(r.count("st") > 10);
        const double rms = sqrt(r.energy / (2.0 * 44100 * 2));
        if (!(rms > 0.01))
            fprintf(stderr, "demo pattern rms %.5f\n", rms);
        CHECK(rms > 0.01);
        CHECK(r.ok(1, R"({"t":"bpm","bpm":999})"));
        CHECK(r.ses.project().bpm == kMaxBpm);
        CHECK(r.ok(1, R"({"t":"mode","mode":"song"})"));
        CHECK(r.err(1, R"({"t":"mode","mode":"x"})") == "bad_arg");
        CHECK(r.ok(1, R"({"t":"stop"})"));
        r.render(0.1);
        CHECK(r.eng.status().playing.load() == 0);
        CHECK(r.count("diag") >= 1);
    }
    // editing: channel / pattern / steps / notes / mixer / fx / playlist / automation, with patches
    {
        Rig r;
        r.ses.boot();
        for (int c : {1, 2}) {
            r.ses.clientConnected(c);
            r.ok(c, R"({"t":"hello","proto":1})");
        }
        const Project &p = r.ses.project();
        const double ch0 = p.channels[0].id, pat0 = p.patterns[0].id;
        r.out.clear();
        CHECK(r.ok(1, sfmt(R"({"t":"step.toggle","pattern":%g,"channel":%g,"index":3})", pat0, ch0)));
        CHECK(r.ses.project().patterns[0].seqs[0].steps[3].on);
        CHECK(r.count("patch", "pattern", -1) == 1);  // broadcast to everyone
        CHECK(r.err(1, sfmt(R"({"t":"step.toggle","pattern":%g,"channel":%g,"index":999})", pat0, ch0)) == "bad_arg");
        CHECK(r.err(1, sfmt(R"({"t":"step.toggle","pattern":12345,"channel":%g,"index":1})", ch0)) == "not_found");
        // a new channel with a plugin, a step row in every pattern, its own insert
        double nc = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.sine","name":"Test"})", &nc));
        CHECK(nc > 0 && r.ses.project().channels.size() == 9);
        CHECK(r.ses.project().channels.back().track == 9);
        CHECK(r.err(1, R"({"t":"channel.add","plugin":"org.smhgzll.fx.delay"})") == "bad_arg");  // not an instrument
        CHECK(r.err(1, R"({"t":"channel.add","plugin":"nope"})") == "bad_arg");
        // notes: add, update, stale update, delete
        cJSON *a = r.req(1, sfmt(R"({"t":"notes.add","pattern":%g,"channel":%g,"notes":[[0,0,480,60,0.9,0,0,0.5,1,0],[0,960,480,64,0.7,0.5,0,0.5,1,0]]})", pat0, nc));
        CHECK(a && !strcmp(cJSON_GetObjectItem(a, "t")->valuestring, "ack"));
        const double id1 = cJSON_GetArrayItem(cJSON_GetObjectItem(a, "ids"), 0)->valuedouble;
        cJSON_Delete(a);
        CHECK(r.ok(1, sfmt(R"({"t":"notes.update","pattern":%g,"channel":%g,"notes":[{"id":%g,"key":67,"pan":-2}]})", pat0, nc, id1)));
        const ChannelSeq *seq = r.ses.project().patterns[0].find((Id)nc);
        CHECK(seq && seq->notes.size() == 2 && seq->notes[0].key == 67 && seq->notes[0].pan == -1.0f);
        CHECK(r.ok(2, sfmt(R"({"t":"notes.del","pattern":%g,"channel":%g,"ids":[%g]})", pat0, nc, id1)));
        // client 1 edits the note client 2 just deleted: a stale edit, refused, nothing changes
        CHECK(r.err(1, sfmt(R"({"t":"notes.update","pattern":%g,"channel":%g,"notes":[{"id":%g,"key":50}]})", pat0, nc, id1)) == "not_found");
        CHECK(r.ses.project().patterns[0].find((Id)nc)->notes.size() == 1);
        CHECK(r.err(1, sfmt(R"({"t":"notes.add","pattern":%g,"channel":%g,"notes":[[0,-5,480,60]]})", pat0, nc)) == "bad_arg");
        // a whole-row replace (paste / quantize from the piano roll)
        CHECK(r.ok(1, sfmt(R"({"t":"notes.set","pattern":%g,"channel":%g,"notes":[[5,0,240,60],[5,240,240,62],[0,480,240,64]]})", pat0, nc)));
        seq = r.ses.project().patterns[0].find((Id)nc);
        CHECK(seq->notes.size() == 3 && seq->notes[0].id != seq->notes[1].id);
        // continuous controls use "set" messages, not whole-list patches
        r.out.clear();
        CHECK(r.ok(1, sfmt(R"({"t":"channel.set","channel":%g,"vol":0.5,"pan":-0.25})", nc)));
        CHECK(r.count("set") == 1 && r.count("patch") == 0);
        CHECK(r.ok(1, sfmt(R"({"t":"channel.param","channel":%g,"key":"tune","v":7})", nc)));
        CHECK(r.ok(1, sfmt(R"({"t":"channel.param","channel":%g,"key":"tune","v":1000})", nc)));  // clamped
        float *tune = const_cast<Project &>(r.ses.project()).channel((Id)nc)->inst.param("tune");
        CHECK(tune && *tune == 24.0f);
        CHECK(r.err(1, sfmt(R"({"t":"channel.param","channel":%g,"key":"nope","v":1})", nc)) == "bad_arg");
        CHECK(r.ok(1, sfmt(R"({"t":"channel.preset","channel":%g,"name":"Sub"})", nc)));
        CHECK(r.err(1, sfmt(R"({"t":"channel.preset","channel":%g,"name":"Nope"})", nc)) == "not_found");
        // instrument swap while playing (fade-out, retire, no crash)
        r.ok(1, R"({"t":"play"})");
        r.render(0.5);
        CHECK(r.ok(1, sfmt(R"({"t":"channel.plugin","channel":%g,"plugin":"org.smhgzll.3xosc","preset":"Pluck"})", nc)));
        r.render(0.5);
        // mixer: routing, cycles, sends, fx chain
        CHECK(r.ok(1, R"({"t":"track.set","track":3,"route":4})"));
        CHECK(r.err(1, R"({"t":"track.set","track":4,"route":3})") == "cycle");
        CHECK(r.ses.project().mixer[4].route == 0);
        CHECK(r.ok(1, R"({"t":"track.send","track":5,"slot":0,"target":10,"level":0.5})"));
        CHECK(r.err(1, R"({"t":"track.send","track":10,"slot":0,"target":5,"level":0.5})") == "cycle");
        double fx1 = 0, fx2 = 0;
        CHECK(r.ok(1, R"({"t":"fx.add","track":2,"plugin":"org.smhgzll.fx.filter"})", &fx1));
        CHECK(r.ok(1, R"({"t":"fx.add","track":2,"plugin":"org.smhgzll.fx.delay","index":0})", &fx2));
        CHECK(r.ses.project().mixer[2].fx[0].id == (Id)fx2 && r.ses.project().mixer[2].fx[1].id == (Id)fx1);
        CHECK(r.ok(1, sfmt(R"({"t":"fx.move","fx":%g,"index":0})", fx1)));
        CHECK(r.ses.project().mixer[2].fx[0].id == (Id)fx1);
        CHECK(r.ok(1, sfmt(R"({"t":"fx.param","fx":%g,"key":"cutoff","v":300})", fx1)));
        CHECK(r.ok(1, sfmt(R"({"t":"fx.set","fx":%g,"enabled":false})", fx2)));
        r.render(0.3);
        CHECK(r.ok(1, sfmt(R"({"t":"fx.del","fx":%g})", fx2)));
        CHECK(r.err(1, R"({"t":"fx.add","track":2,"plugin":"org.smhgzll.3xosc"})") == "bad_arg");
        for (int i = 0; i < 3; i++)
            r.ok(1, R"({"t":"fx.add","track":7,"plugin":"org.smhgzll.fx.utility"})");
        CHECK(r.err(1, R"({"t":"fx.add","track":7,"plugin":"org.smhgzll.fx.utility"})") == "ok" ||
              r.ses.project().mixer[7].fx.size() == (size_t)kMaxFxPerTrack);
        CHECK(r.err(1, R"({"t":"fx.add","track":7,"plugin":"org.smhgzll.fx.utility"})") == "limit");
        r.render(0.3);
        // playlist: add, move, split, stale, delete
        double clip = 0;
        CHECK(r.ok(1, sfmt(R"({"t":"clip.add","kind":"pattern","ref":%g,"track":3,"start":15360})", pat0), &clip));
        CHECK(r.ok(1, sfmt(R"({"t":"clip.update","clips":[{"id":%g,"start":19200,"track":4}]})", clip)));
        double right = 0;
        CHECK(r.ok(1, sfmt(R"({"t":"clip.split","clip":%g,"at":21120})", clip), &right));
        CHECK(const_cast<Project &>(r.ses.project()).clip((Id)right)->offset == 1920);
        CHECK(r.ok(2, sfmt(R"({"t":"clip.del","ids":[%g]})", clip)));
        CHECK(r.err(1, sfmt(R"({"t":"clip.update","clips":[{"id":%g,"start":0}]})", clip)) == "not_found");
        CHECK(r.err(1, sfmt(R"({"t":"clip.add","kind":"pattern","ref":%g,"track":99,"start":0})", pat0)) == "bad_arg");
        // automation: a filter cutoff sweep on the fx, placed on the playlist
        double au = 0;
        CHECK(r.ok(1, sfmt(R"({"t":"auto.add","name":"Sweep","target":{"kind":"fxParam","fx":%g,"param":"cutoff"},"points":[[0,0],[3840,1]]})", fx1), &au));
        CHECK(r.ok(1, sfmt(R"({"t":"clip.add","kind":"automation","ref":%g,"track":5,"start":0})", au)));
        CHECK(r.ok(1, R"({"t":"mode","mode":"song"})"));
        r.render(1.0);
        CHECK(r.ok(1, sfmt(R"({"t":"auto.del","auto":%g})", au)));
        // patterns: add / dup / rename / length / select / move / delete (with its clips)
        double np = 0, dp = 0;
        CHECK(r.ok(1, R"({"t":"pattern.add","name":"Fill"})", &np));
        // with a fixed length and without selecting it (MIDI file import)
        double fixedPat = 0;
        const Id selBefore = r.ses.project().currentPattern;
        CHECK(r.ok(1, R"({"t":"pattern.add","name":"Fixed","length":23040,"select":false})", &fixedPat));
        CHECK(r.ses.project().currentPattern == selBefore);
        CHECK(const_cast<Project &>(r.ses.project()).pattern((Id)fixedPat)->lengthTicks == 23040);
        CHECK(r.err(1, R"({"t":"pattern.add","length":999999999})") == "bad_arg");
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.del","pattern":%g})", fixedPat)));
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.dup","pattern":%g})", pat0), &dp));
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.set","pattern":%g,"name":"Renamed","length":7680,"swing":0.3})", dp)));
        CHECK(r.err(1, sfmt(R"({"t":"pattern.set","pattern":%g,"length":-1})", dp)) == "bad_arg");
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.select","pattern":%g,"queue":false})", dp)));
        CHECK(r.ses.project().currentPattern == (Id)dp);
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.move","pattern":%g,"index":0})", np)));
        CHECK(r.ses.project().patterns[0].id == (Id)np);
        const size_t clipsBefore = r.ses.project().clips.size();
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.del","pattern":%g})", pat0)));
        CHECK(r.ses.project().clips.size() < clipsBefore);
        // a channel delete removes its rows and disables automation on it
        CHECK(r.ok(1, sfmt(R"({"t":"channel.del","channel":%g})", nc)));
        CHECK(r.ses.project().channels.size() == 8);
        r.render(0.5);
        CHECK(r.eng.status().nanResets.load() == 0);
    }
    // live notes: a disconnecting client never leaves a note hanging
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        const double sine = r.ses.project().channels[7].id;  // the Sine channel of the demo
        CHECK(r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":69,"vel":1,"on":true})", sine)));
        r.render(0.3);
        CHECK(r.eng.status().voices.load() >= 1);
        r.ses.clientDisconnected(1);
        r.render(1.0);
        CHECK(r.eng.status().voices.load() == 0);
        // MIDI bytes: note on through running status, CC 123 = all notes off
        r.ses.clientConnected(2);
        r.ok(2, R"({"t":"hello","proto":1})");
        CHECK(r.ok(2, sfmt(R"({"t":"midi","channel":%g,"data":[144,60,100,64,90]})", sine)));
        r.render(0.2);
        CHECK(r.eng.status().voices.load() == 2);
        CHECK(r.ok(2, sfmt(R"({"t":"midi","channel":%g,"data":[176,123,0]})", sine)));
        r.render(1.0);
        CHECK(r.eng.status().voices.load() == 0);
        CHECK(r.err(2, R"({"t":"midi","data":5})") == "bad_arg");
        // a burst of on/off pairs inside one block (regression: offs were sorted before ons)
        std::string burst = "[";
        for (int i = 0; i < 100; i++)
            burst += (i ? "," : "") + sfmt("144,%g,100,128,%g,0", 36 + i % 48, 36 + i % 48);
        burst += "]";
        for (int k = 0; k < 4; k++)
            CHECK(r.ok(2, sfmt(R"({"t":"midi","channel":%g,"data":)", sine) + burst + "}"));
        r.render(1.5);
        CHECK(r.eng.status().voices.load() == 0);
    }
    // external MIDI clock: tempo follows timestamped ticks at 100 BPM despite arrival jitter
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        r.ok(1, R"({"t":"midi","data":[250]})");  // start
        const double tickMs = 60000.0 / 100.0 / 24.0;
        for (int i = 0; i < 24 * 8; i++) {
            const double ts = 1000 + i * tickMs + ((i * 7919) % 5 - 2) * 0.3;  // +-0.6 ms jitter
            r.ses.handleText(1, sfmt(R"({"t":"midi","data":[248],"ts":%g})", ts).c_str(), sfmt(R"({"t":"midi","data":[248],"ts":%g})", ts).size());
            if (i % 6 == 0)
                r.render(0.02);
        }
        CHECK_NEAR(r.ses.project().bpm, 100.0, 0.5);
    }
    // projects: save / list / load / delete; autosave only while stopped; boot restores it
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        CHECK(r.ok(1, R"({"t":"project.rename","name":"My Song"})"));
        CHECK(r.ok(1, R"({"t":"project.save","slot":2})"));
        CHECK(r.err(1, R"({"t":"project.save","slot":0})") == "bad_arg");
        CHECK(r.store.info(2).valid && r.store.info(2).name == "My Song");
        CHECK(r.ok(1, R"({"t":"project.new","template":"empty"})"));
        CHECK(r.ses.project().channels.empty());
        CHECK(r.ok(1, R"({"t":"project.load","slot":2})"));
        CHECK(r.ses.project().name == "My Song" && r.ses.project().channels.size() == 8);
        CHECK(r.err(1, R"({"t":"project.load","slot":5})") == "storage");
        // autosave: playing -> deferred; stopped + 10 s -> written
        r.ok(1, R"({"t":"bpm","bpm":99})");
        r.ok(1, R"({"t":"play"})");
        const uint32_t seqBefore = r.store.info(0).seq;
        r.render(12.0);
        CHECK(r.store.info(0).seq == seqBefore);
        r.ok(1, R"({"t":"stop"})");
        r.render(11.0);
        CHECK(r.store.info(0).seq > seqBefore);
        // where flash writes stall the audio (ESP32-S3), saving / deleting wait for the stop
        {
            Rig e(true, true);
            e.ses.boot();
            e.ses.clientConnected(1);
            e.ok(1, R"({"t":"hello","proto":1})");
            CHECK(e.ok(1, R"({"t":"play"})"));
            e.render(0.1);
            CHECK(e.err(1, R"({"t":"project.save","slot":3})") == "playing");
            CHECK(e.err(1, R"({"t":"project.delete","slot":2})") == "playing");
            CHECK(!e.store.info(3).valid);
            e.ok(1, R"({"t":"stop"})");
            e.render(0.1);
            CHECK(e.ok(1, R"({"t":"project.save","slot":3})"));
            CHECK(e.store.info(3).valid);
        }
        Rig r2;  // a "reboot" on the same flash
        r2.flash.data() = r.flash.data();
        r2.ses.boot();
        CHECK(r2.ses.project().bpm == 99.0 && r2.ses.project().name == "My Song");
        CHECK(r.ok(1, R"({"t":"project.delete","slot":2})"));
        CHECK(!r.store.info(2).valid);
    }
    // a project with a missing plugin loads, keeps it, and plays the rest
    {
        Rig r;
        Project p = defaultProject();
        p.channels[1].inst.pluginId = "com.example.missing";
        std::string text = projectToText(p), err;
        CHECK(r.ses.importProject(text.data(), text.size(), &err));
        CHECK(r.ses.project().channels[1].inst.missing);
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        r.ok(1, R"({"t":"play"})");
        r.render(1.0);
        CHECK(r.ses.exportProject().find("com.example.missing") != std::string::npos);
    }
    // the remaining operation families: channel order, steps -> notes, playlist tracks, markers,
    // time signature, test tones, MIDI transport messages
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        const Project &p = r.ses.project();
        const Id first = p.channels[0].id, second = p.channels[1].id;
        CHECK(r.ok(1, sfmt(R"({"t":"channel.move","channel":%g,"index":1})", (double)first)));
        CHECK(p.channels[0].id == second && p.channels[1].id == first);
        CHECK(r.err(1, R"({"t":"channel.move","channel":999999,"index":0})") == "not_found");
        // steps -> notes: the converted row holds one note per enabled step, and plays the roll
        const Pattern &beat = p.patterns[0];
        const ChannelSeq *kick = nullptr;
        for (auto &q : beat.seqs)
            if (q.channel == first)
                kick = &q;
        CHECK(kick != nullptr);
        if (kick) {
            size_t on = 0;
            for (auto &st : kick->steps)
                on += st.on;
            CHECK(r.ok(1, sfmt(R"({"t":"seq.mode","pattern":%g,"channel":%g,"pianoRoll":true})", (double)beat.id, (double)first)));
            for (auto &q : p.patterns[0].seqs)
                if (q.channel == first)
                    CHECK(q.pianoRoll && q.notes.size() == on && on > 0);
        }
        // playlist tracks
        const size_t nt = p.tracks.size();
        CHECK(r.ok(1, R"({"t":"ptrack.set","add":true})"));
        CHECK(p.tracks.size() == nt + 1);
        CHECK(r.ok(1, sfmt(R"({"t":"ptrack.set","index":%g,"name":"Drums","mute":true})", (double)nt)));
        CHECK(p.tracks[nt].name == "Drums" && p.tracks[nt].mute);
        CHECK(r.err(1, R"({"t":"ptrack.set","index":500,"name":"x"})") == "not_found");
        // markers: add, move, delete
        double mk = 0;
        CHECK(r.ok(1, R"({"t":"marker","action":"add","tick":7680,"name":"Chorus"})", &mk));
        CHECK(p.markers.size() == 1 && p.markers[0].tick == 7680 && p.markers[0].name == "Chorus");
        CHECK(r.ok(1, sfmt(R"({"t":"marker","action":"move","marker":%g,"tick":3840})", mk)));
        CHECK(p.markers[0].tick == 3840);
        CHECK(r.ok(1, sfmt(R"({"t":"marker","action":"del","marker":%g})", mk)));
        CHECK(p.markers.empty());
        // time signature
        CHECK(r.ok(1, R"({"t":"timesig","num":7,"den":8})"));
        CHECK(p.ts.num == 7 && p.ts.den == 8);
        CHECK(r.err(1, R"({"t":"timesig","num":4,"den":3})") == "bad_arg");
        CHECK(r.err(1, R"({"t":"timesig","num":0,"den":4})") == "bad_arg");
        // test tones: a 440 Hz sine is audible while stopped; off is silent again
        CHECK(r.err(1, R"({"t":"diag.tone","kind":"loud"})") == "bad_arg");
        CHECK(r.ok(1, R"({"t":"diag.tone","kind":"sine440"})"));
        r.energy = 0;
        r.render(0.5);
        CHECK(r.energy > 100.0);  // 0.25 amplitude, two channels, 22050 frames: ~1378
        CHECK(r.ok(1, R"({"t":"diag.tone","kind":"off"})"));
        r.render(0.05);
        r.energy = 0;
        r.render(0.5);
        CHECK(r.energy < 1e-6);
        // MIDI start / song position / stop / continue drive the transport
        auto midi = [&](const char *bytes) {
            r.ok(1, std::string(R"({"t":"midi","data":[)") + bytes + "]}");
            r.render(0.05);
        };
        midi("250");  // start
        CHECK(r.eng.status().playing.load() == 1);
        midi("252");  // stop -> pause
        CHECK(r.eng.status().playing.load() == 2);
        midi("242,16,0");  // song position pointer: 16 sixteenths = tick 3840
        CHECK(r.eng.status().tick.load() == 16 * 240);
        midi("251");  // continue
        CHECK(r.eng.status().playing.load() == 1);
    }
    // samples (ABI 1.1 resources): import, assign to a sampler channel, play, export / import,
    // delete -> reported missing, replace -> reloaded
    {
        auto wav = [](double freq, uint32_t frames) {  // 16-bit mono WAV with a smpl root of 69
            std::vector<uint8_t> w;
            auto p16 = [&](uint32_t x) { w.push_back((uint8_t)x), w.push_back((uint8_t)(x >> 8)); };
            auto p32 = [&](uint32_t x) { p16(x & 0xFFFF), p16(x >> 16); };
            auto tag = [&](const char *t) { w.insert(w.end(), t, t + 4); };
            tag("RIFF"), p32(0), tag("WAVE"), tag("fmt "), p32(16), p16(1), p16(1), p32(44100), p32(88200), p16(2), p16(16);
            tag("smpl"), p32(36);
            for (int i = 0; i < 3; i++)
                p32(0);
            p32(69);
            for (int i = 0; i < 5; i++)
                p32(0);
            tag("data"), p32(frames * 2);
            for (uint32_t i = 0; i < frames; i++)
                p16((uint16_t)(int16_t)lrint(12000 * sin(2 * M_PI * freq * i / 44100.0)));
            return w;
        };
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        auto a = wav(440, 44100);
        std::string err;
        CHECK(r.ses.importSample("Tone A.wav", a.data(), a.size(), &err));
        CHECK(!r.ses.importSample("bad.wav", (const uint8_t *)"RIFF\0\0\0\0WAVEjunk", 16, &err));
        CHECK(r.ses.samplesJson().find("\"Tone A.wav\"") != std::string::npos);
        double chId = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.sampler","name":"Smp"})", &chId));
        CHECK(r.err(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"nope","sample":"Tone A.wav"})", chId)) == "bad_arg");
        CHECK(r.err(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"sample","sample":"none.wav"})", chId)) == "not_found");
        CHECK(r.ok(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"sample","sample":"Tone A.wav"})", chId)));
        const Channel *c = nullptr;
        for (auto &x : r.ses.project().channels)
            if (x.id == (Id)chId)
                c = &x;
        CHECK(c && c->inst.resource("sample") && *c->inst.resource("sample") == "Tone A.wav" && c->inst.missingResources.empty());
        // A failed binding must answer with an error and retain the previous playable resource.
        const std::string beforeBinding = r.ses.exportProject();
        CHECK(r.samples.putRaw("Wrong.sf2", SampleType::SoundFont, (const uint8_t *)"RIFF\0\0\0\0sfbk", 12, &err));
        CHECK(r.err(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"sample","sample":"Wrong.sf2"})", chId)) == "resources");
        CHECK(r.ses.exportProject() == beforeBinding);
        r.render(0.05);
        r.energy = 0;
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":69,"vel":1,"on":true})", chId));
        r.render(0.3);
        CHECK(r.energy > 10.0);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":69,"on":false})", chId));
        // the project keeps the reference; another session with the same library plays it
        const std::string doc = r.ses.exportProject();
        CHECK(doc.find("\"resources\":{\"sample\":\"Tone A.wav\"}") != std::string::npos);
        Rig r2;
        r2.sampleFlash.data() = r.sampleFlash.data();
        r2.samples.mount();
        CHECK(r2.ses.importProject(doc.data(), doc.size(), &err));
        for (auto &x : r2.ses.project().channels)
            if (x.inst.pluginId == "org.smhgzll.sampler")
                CHECK(x.inst.missingResources.empty());
        // delete while playing on the device is refused; stopped, it goes, and a reload reports it
        CHECK(r.ok(1, R"({"t":"samples.delete","name":"Tone A.wav"})"));
        CHECK(r.err(1, R"({"t":"samples.delete","name":"Tone A.wav"})") == "not_found");
        CHECK(r.ok(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"sample","sample":""})", chId)));
        CHECK(r.err(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"sample","sample":"Tone A.wav"})", chId)) == "not_found");
        Rig r3;  // a project referring to a sample the library does not have: loads, reports it
        CHECK(r3.ses.importProject(doc.data(), doc.size(), &err));
        bool reported = false;
        for (auto &x : r3.ses.project().channels)
            reported |= x.inst.pluginId == "org.smhgzll.sampler" && x.inst.missingResources.size() == 1;
        CHECK(reported);
        CHECK(r3.ses.exportProject().find("\"Tone A.wav\"") != std::string::npos);  // still referenced
        // replacing the file under the same name reloads the channels that use it
        auto b = wav(880, 22050);
        CHECK(r3.ses.importSample("Tone A.wav", b.data(), b.size(), &err));
        for (auto &x : r3.ses.project().channels)
            if (x.inst.pluginId == "org.smhgzll.sampler")
                CHECK(x.inst.missingResources.empty());
    }
    // user presets: save the current sound, load it on another channel, survive a reboot, delete
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        double a = 0, b = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.3xosc","name":"A"})", &a));
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.3xosc","name":"B"})", &b));
        CHECK(r.ok(1, sfmt(R"({"t":"channel.param","channel":%g,"key":"cutoff","v":444})", a)));
        CHECK(r.ok(1, sfmt(R"({"t":"preset.save","channel":%g,"name":"My Lead"})", a)));
        CHECK(r.err(1, sfmt(R"({"t":"preset.save","channel":%g,"name":"Init"})", a)) == "bad_arg");  // a factory name
        CHECK(r.ok(1, sfmt(R"({"t":"channel.preset","channel":%g,"name":"My Lead"})", b)));
        auto cutoff = [&](double id) {
            for (auto &c : r.ses.project().channels)
                if (c.id == (Id)id)
                    for (auto &kv : c.inst.params)
                        if (kv.first == "cutoff")
                            return kv.second;
            return -1.0f;
        };
        CHECK_NEAR(cutoff(b), 444.0, 0.01);
        CHECK(r.err(1, sfmt(R"({"t":"channel.preset","channel":%g,"name":"Nope"})", b)) == "not_found");
        // another plugin does not see it; a "reboot" on the same flash still has it
        double s = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.sine","name":"S"})", &s));
        CHECK(r.err(1, sfmt(R"({"t":"channel.preset","channel":%g,"name":"My Lead"})", s)) == "not_found");
        PresetLibrary again(r.presetStore);
        again.mount();
        CHECK(again.find("org.smhgzll.3xosc", "My Lead") != nullptr && again.list().size() == 1);
        CHECK(r.ok(1, R"({"t":"preset.delete","plugin":"org.smhgzll.3xosc","name":"My Lead"})"));
        CHECK(r.err(1, R"({"t":"preset.delete","plugin":"org.smhgzll.3xosc","name":"My Lead"})") == "not_found");
        again.mount();
        CHECK(again.list().empty());
    }
    // an instrument swap while the old one sounds: the next note goes to the new instrument at once
    // (regression: the old instance kept receiving notes during its ~45 ms fade, so they were lost)
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        double ch = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.3xosc","name":"X"})", &ch));
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":60,"vel":1,"on":true})", ch));
        r.render(0.2);
        CHECK(r.eng.status().voices.load() >= 1);
        CHECK(r.ok(1, sfmt(R"({"t":"channel.plugin","channel":%g,"plugin":"org.smhgzll.sine"})", ch)));
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":72,"vel":1,"on":true})", ch));  // no render in between
        r.render(0.2);
        const int slot = [&] {
            for (auto &c : r.ses.project().channels)
                if (c.id == (Id)ch)
                    return 1;
            return 0;
        }();
        CHECK(slot == 1);
        // after the crossfade only the sine plays, and it plays the note sent right after the swap
        CHECK(r.eng.status().voices.load() == 1);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":72,"on":false})", ch));
        r.render(1.0);
        CHECK(r.eng.status().voices.load() == 0);
    }
    // MIDI recording: live notes while a pattern plays become piano-roll notes at the ticks they sounded
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        double ch = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.sine","name":"Rec"})", &ch));
        const Id pat = r.ses.project().patterns[0].id;
        r.ok(1, sfmt(R"({"t":"pattern.select","pattern":%g})", (double)pat));
        r.ok(1, R"({"t":"mode","mode":"pattern"})");
        r.ok(1, R"({"t":"bpm","bpm":120})");  // 1920 ticks per second
        CHECK(r.ok(1, R"({"t":"record","on":true,"quantize":240})"));
        r.ok(1, R"({"t":"play"})");
        r.render(0.26);  // about tick 500
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":64,"vel":0.9,"on":true})", ch));
        r.render(0.25);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":64,"on":false})", ch));
        r.render(0.1);
        auto notesOf = [&](Id channel) -> std::vector<Note> {
            for (auto &pt : r.ses.project().patterns)
                if (pt.id == pat)
                    for (auto &q : pt.seqs)
                        if (q.channel == channel)
                            return q.notes;
            return {};
        };
        auto ns = notesOf((Id)ch);
        CHECK(ns.size() == 1);
        if (ns.size() == 1) {
            CHECK(ns[0].key == 64 && ns[0].start % 240 == 0);
            CHECK(ns[0].start >= 240 && ns[0].start <= 720);             // quantized to the step near tick 500
            CHECK(ns[0].len >= 400 && ns[0].len <= 560);                  // about 0.25 s = 480 ticks
            CHECK_NEAR(ns[0].vel, 0.9, 0.01);
        }
        // a note held over the pattern's end wraps: start near the end, length into the next loop
        const int32_t plen = [&] {
            for (auto &pt : r.ses.project().patterns)
                if (pt.id == pat)
                    return r.ses.project().patternLength(pt);
            return 0;
        }();
        const double toEnd = (plen - (double)r.eng.status().tick.load()) / 1920.0;
        r.render(toEnd - 0.1);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":67,"vel":0.8,"on":true})", ch));
        r.render(0.3);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":67,"on":false})", ch));
        r.render(0.1);
        ns = notesOf((Id)ch);
        bool wrapped = false;
        for (auto &n : ns)
            wrapped |= n.key == 67 && n.start >= plen - 480 && n.len >= 500;
        CHECK(ns.size() == 2 && wrapped);
        // disarmed: playing does not record; a note held when recording stops ends at the pattern end
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":71,"vel":0.8,"on":true})", ch));
        r.render(0.1);
        CHECK(r.ok(1, R"({"t":"record","on":false})"));
        r.render(0.3);
        ns = notesOf((Id)ch);
        CHECK(ns.size() == 3);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":72,"vel":0.8,"on":true})", ch));
        r.render(0.2);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":72,"on":false})", ch));
        r.render(0.2);
        CHECK(notesOf((Id)ch).size() == 3);
    }
    // CPU guard (engine) and admission (session), driven by a simulated render load
    {
        Rig r(true, false, 900, 750);
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        const double ch = r.ses.project().channels[0].id;
        r.render(1.1);
        CHECK(r.eng.status().loadPermille.load() < 900);  // this host renders far below the deadline
        double fx = 0;
        CHECK(r.ok(1, R"({"t":"fx.add","track":1,"plugin":"org.smhgzll.fx.delay"})", &fx));
        auto &st = r.eng.status();
        st.testLoadPermille.store(950);
        r.render(1.1);
        CHECK(st.loadPermille.load() > 900);
        CHECK(st.cpuPermille.load() > 750);
        const uint32_t drops = st.cpuGuardDrops.load();
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":60,"vel":1,"on":true})", ch));
        r.render(0.1);
        CHECK(st.cpuGuardDrops.load() == drops + 1);  // the new note is skipped
        CHECK(st.voices.load() == 0);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":60,"on":false})", ch));
        CHECK(r.err(1, R"({"t":"channel.add","plugin":"org.smhgzll.sine"})") == "cpu");
        CHECK(r.err(1, R"({"t":"fx.add","track":1,"plugin":"org.smhgzll.fx.eq"})") == "cpu");
        CHECK(r.ok(1, sfmt(R"({"t":"fx.set","fx":%g,"enabled":false})", fx)));  // disabling always works
        CHECK(r.err(1, sfmt(R"({"t":"fx.set","fx":%g,"enabled":true})", fx)) == "cpu");
        CHECK(r.ok(1, sfmt(R"({"t":"fx.set","fx":%g,"mix":0.5})", fx)));
        // the st message counts the skipped notes for the browser
        bool skipsShown = false;
        for (auto &o : r.out)
            skipsShown |= o.text.find("\"cpuSkips\":1") != std::string::npos;
        CHECK(skipsShown);
        // recovery: below the threshold (minus its hysteresis) notes play again
        st.testLoadPermille.store(0);
        r.render(1.1);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":62,"vel":1,"on":true})", ch));
        r.render(0.1);
        CHECK(st.voices.load() >= 1);
        CHECK(st.cpuGuardDrops.load() == drops + 1);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":62,"on":false})", ch));
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.sine"})"));
        CHECK(r.ok(1, sfmt(R"({"t":"fx.set","fx":%g,"enabled":true})", fx)));
    }
    // Replacing an overloaded project must not admit against its old one-second average.
    // The live owner advances audio while teardown waits, just as the board does.
    {
        Rig r(true, false, 900, 750, true);
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        auto &st = r.eng.status();
        st.testLoadPermille.store(950);
        r.render(1.1);
        CHECK(st.cpuPermille.load() > 750);
        CHECK(r.err(1, R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})") == "cpu");
        st.overruns.fetch_add(3);
        st.cpuGuardDrops.fetch_add(5);
        const uint32_t overruns = st.overruns.load(), skips = st.cpuGuardDrops.load();
        st.testLoadPermille.store(0);  // no new block yet: the published load is still high
        CHECK(r.ok(1, R"({"t":"project.new","template":"empty"})"));
        CHECK(st.cpuPermille.load() < 750);
        CHECK(st.loadPermille.load() < 900);
        CHECK(st.overruns.load() == overruns && st.cpuGuardDrops.load() == skips);
        for (int i = 0; i < 4; i++)
            CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})"));
        CHECK(r.ses.project().channels.size() == 4);
        CHECK(r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":60,"vel":1,"on":true})",
                           (double)r.ses.project().channels[0].id)));
        r.render(0.012);
        CHECK(st.cpuGuardDrops.load() == skips);  // old guard hysteresis is gone too
        r.render(1.1);  // publishing must not bring the previous project's average back
        CHECK(st.cpuPermille.load() < 750);
        st.testLoadPermille.store(950);
        r.render(1.1);
        CHECK(r.err(1, R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})") == "cpu");
        CHECK(r.ok(1, R"({"t":"project.new","template":"empty"})"));
        r.render(1.1);  // even an empty project's old measurement can be high
        CHECK(st.cpuPermille.load() > 750);
        st.testLoadPermille.store(0);
        CHECK(r.ok(1, R"({"t":"project.new","template":"empty"})"));
        CHECK(st.cpuPermille.load() < 750);
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})"));
    }
    // guard and admission off (QEMU, offline renders): the same load changes nothing
    {
        Rig r;
        r.ses.boot();
        r.ses.clientConnected(1);
        r.ok(1, R"({"t":"hello","proto":1})");
        const double ch = r.ses.project().channels[0].id;
        r.eng.status().testLoadPermille.store(1500);
        r.render(1.1);
        r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":60,"vel":1,"on":true})", ch));
        r.render(0.1);
        CHECK(r.eng.status().voices.load() >= 1);
        CHECK(r.eng.status().cpuGuardDrops.load() == 0);
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.sine"})"));
    }
    // Warm imports retire the stopped project's instruments/effects before building
    // another project. A render-block fence works even with commands still in flight.
    {
        Engine engine({44100, 256});
        unsigned waits = 0;
        float l[256], r[256];
        Session::Config config;
        config.waitForAudio = [&] { ++waits; engine.render(l, r, 256); };
        Session session(engine, config);
        session.boot();
        engine.render(l, r, 256);
        const auto original = session.exportProject();
        const uint32_t firstFence = engine.status().fence.load();
        const unsigned firstWaits = waits;
        std::string error;
        CHECK(session.importProject(original.data(), original.size(), &error));
        CHECK(waits > firstWaits);
        CHECK(engine.status().fence.load() == firstFence + 1);
        Retired retired;
        CHECK(!engine.popRetired(retired));  // reclaimed before new instances were installed
        engine.render(l, r, 256);
        session.tick(20);
        CHECK(session.exportProject() == original);
        CHECK(session.importProject(original.data(), original.size(), &error));
        CHECK(engine.status().fence.load() == firstFence + 2);
        CHECK(!engine.popRetired(retired));
        engine.render(l, r, 256);
        CHECK(engine.status().retireOverflow.load() == 0);
    }
    // Selective SF2 bindings use their requested preset immediately. Choosing an instrument
    // outside the initial subset must reload it on the control thread, rather than playing the
    // subset's first preset. The fixture's upper key split is an octave above program zero.
    {
        Rig r(true, false, 0, 0, true); r.ses.boot(); r.render(.03);
        r.ses.clientConnected(1); CHECK(r.ok(1, R"({"t":"hello","proto":1})"));
        CHECK(r.ok(1, R"({"t":"project.new","template":"empty"})"));
        auto font = subsetFontFixture(); std::string error;
        CHECK(r.ses.importSample("subset.sf2", font.data(), font.size(), &error));
        double id = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})", &id));
        CHECK(r.ok(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"soundfont","sample":"subset.sf2","bank":0,"program":5,"presets":[{"bank":0,"program":5}]})", id)));
        CHECK(r.ses.project().channels[0].inst.resource("soundfont") && r.ses.project().channels[0].inst.missingResources.empty());
        CHECK(r.ok(1, sfmt(R"({"t":"channel.param","channel":%g,"key":"program","v":0})", id)));
        r.render(.05);
        CHECK(r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":84,"vel":1,"on":true})", id)));
        r.render(.02);
        int crossings=0; float previous=0, l[256], rr[256];
        for(int block=0;block<8;block++) { r.eng.render(l,rr,256); for(float x:l){if(previous<0 && x>=0)crossings++;previous=x;} }
        CHECK(crossings>50 && crossings<80);
        CHECK(r.ok(1, sfmt(R"({"t":"note","channel":%g,"key":84,"on":false})", id)));
        const std::string before = r.ses.exportProject();
        CHECK(r.err(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"soundfont","sample":"subset.sf2","presets":"invalid"})", id)) == "bad_arg");
        CHECK(r.ses.exportProject() == before);
        CHECK(r.ok(1, sfmt(R"({"t":"auto.add","target":{"kind":"channelParam","channel":%g,"param":"program"}})", id)));
        CHECK(r.ses.project().automation.back().target.kind == AutomationTarget::ChannelParam);
        r.render(.1);
        CHECK(r.ses.project().channels[0].inst.missingResources.empty());
    }
    // MIDI edits are transactional, ordinary note edits preserve their controller lane, and
    // indexed append patches do not re-send all previous data (out-of-order appends replace it).
    {
        Rig r(true,false,0,0,true); r.ses.boot(); r.render(.03); r.ses.clientConnected(1);
        CHECK(r.ok(1,R"({"t":"hello","proto":1})")); CHECK(r.ok(1,R"({"t":"project.new","template":"empty"})"));
        double channel=0; CHECK(r.ok(1,R"({"t":"channel.add","plugin":"org.smhgzll.sine"})",&channel));
        const double pattern=r.ses.project().patterns[0].id;
        CHECK(r.ok(1,sfmt(R"({"t":"notes.set","pattern":%g,"channel":%g,"notes":[[1,0,960,60,1,0,0,0.5,1,0,4,2]],"midi":[[0,2,192,0]]})",pattern,channel)));
        CHECK(r.ok(1,sfmt(R"({"t":"notes.set","pattern":%g,"channel":%g,"notes":[[1,0,960,64,1]]})",pattern,channel)));
        CHECK(r.ses.project().patterns[0].seqs[0].midi.size()==1);
        auto before=r.ses.exportProject();
        CHECK(r.err(1,sfmt(R"({"t":"notes.set","pattern":%g,"channel":%g,"notes":[[1,0,960,72,1]],"midi":[[0,2,176,7,128]]})",pattern,channel))=="bad_arg");
        CHECK(r.ses.exportProject()==before);
        CHECK(r.ok(1,sfmt(R"({"t":"midi.set","pattern":%g,"channel":%g,"append":true,"events":[[50,2,224,0,96]]})",pattern,channel)));
        const auto lastPatch=[&] { for(auto it=r.out.rbegin();it!=r.out.rend();++it) { auto *j=cJSON_Parse(it->text.c_str()); const auto *path=cJSON_GetObjectItem(j,"path"); if(path && path->valuestring && !strcmp(path->valuestring,"midi"))return j; cJSON_Delete(j); } return (cJSON*)nullptr; };
        auto *patch=lastPatch(); CHECK(patch!=nullptr);
        if(patch) { const auto *v=cJSON_GetObjectItem(patch,"value"); CHECK(cJSON_GetObjectItem(v,"from")->valueint==1); CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(v,"events"))==1); cJSON_Delete(patch); }
        CHECK(r.ok(1,sfmt(R"({"t":"midi.set","pattern":%g,"channel":%g,"append":true,"events":[[10,2,176,1,127]]})",pattern,channel)));
        patch=lastPatch(); CHECK(patch!=nullptr);
        if(patch) { const auto *v=cJSON_GetObjectItem(patch,"value"); CHECK(cJSON_GetObjectItem(v,"from")->valueint==0); CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(v,"events"))==3); cJSON_Delete(patch); }
        before=r.ses.exportProject();
        CHECK(r.err(1,sfmt(R"({"t":"midi.set","pattern":%g,"channel":%g,"events":[[0,2,144,60,100]]})",pattern,channel))=="bad_arg");
        CHECK(r.ses.exportProject()==before);
        CHECK(before.find("\"schema\":2")!=std::string::npos);
    }
    // A shared font plan is not proof that an older channel's compact index has the new
    // program. Rebind only that instance, then verify the MIDI choice also survives a reload.
    {
        Rig r(true,false,0,0,true); r.ses.boot(); r.render(.03); r.ses.clientConnected(1);
        CHECK(r.ok(1,R"({"t":"hello","proto":1})")); CHECK(r.ok(1,R"({"t":"project.new","template":"empty"})"));
        auto font=subsetFontFixture(); std::string error; CHECK(r.ses.importSample("midi.sf2",font.data(),font.size(),&error));
        double a=0,b=0;
        CHECK(r.ok(1,R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})",&a));
        CHECK(r.ok(1,sfmt(R"({"t":"channel.resource","channel":%g,"slot":"soundfont","sample":"midi.sf2","bank":0,"program":5,"presets":[{"bank":0,"program":5}]})",a)));
        CHECK(r.ok(1,R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})",&b));
        CHECK(r.ok(1,sfmt(R"({"t":"channel.resource","channel":%g,"slot":"soundfont","sample":"midi.sf2","bank":0,"program":0,"presets":[{"bank":0,"program":0}]})",b)));
        double pattern=r.ses.project().patterns[0].id;
        CHECK(r.ok(1,sfmt(R"({"t":"notes.set","pattern":%g,"channel":%g,"notes":[[1,0,960,84,1,0,0,0.5,1,0,4,2]],"midi":[[0,2,192,0]]})",pattern,a)));
        const auto checkPitch=[&] {
            r.render(.05); CHECK(r.ok(1,R"({"t":"play"})")); r.render(.03);
            int crossings=0; float previous=0,l[256],rr[256];
            for(int block=0;block<8;++block) { r.eng.render(l,rr,256); for(float x:l) { if(previous<0 && x>=0)++crossings; previous=x; } }
            CHECK(crossings>50 && crossings<80);
            CHECK(r.ok(1,R"({"t":"stop"})")); r.render(.03);
        };
        checkPitch();
        const auto saved=r.ses.exportProject(); CHECK(r.ses.importProject(saved.data(),saved.size(),&error));
        checkPitch(); CHECK(r.ses.exportProject()==saved);
    }
    // A live channel-mode command never silences a different MIDI/DAW channel.
    {
        Rig r; r.ses.boot(); r.ses.clientConnected(1); r.ok(1,R"({"t":"hello","proto":1})");
        r.ok(1,R"({"t":"project.new","template":"empty"})"); double a=0,b=0;
        CHECK(r.ok(1,R"({"t":"channel.add","plugin":"org.smhgzll.sine"})",&a));
        CHECK(r.ok(1,R"({"t":"channel.add","plugin":"org.smhgzll.sine"})",&b));
        CHECK(r.ok(1,sfmt(R"({"t":"midi","channel":%g,"data":[144,60,100]})",a)));
        CHECK(r.ok(1,sfmt(R"({"t":"midi","channel":%g,"data":[145,64,100]})",b)));
        r.render(1.1); CHECK(r.eng.status().voices.load()==2);
        CHECK(r.ok(1,sfmt(R"({"t":"midi","channel":%g,"data":[176,123,0]})",a)));
        r.render(1.1); CHECK(r.eng.status().voices.load()==1);
        CHECK(r.ok(1,sfmt(R"({"t":"midi","channel":%g,"data":[177,120,0]})",b)));
        r.render(1.1); CHECK(r.eng.status().voices.load()==0);
    }
    // Cached older browsers do not send a preset plan. Their bind-then-param sequence keeps
    // the full font available rather than accumulating a different growing subset per channel.
    {
        Rig r(true,false,0,0,true);r.ses.boot();r.render(.03);r.ses.clientConnected(1);
        CHECK(r.ok(1,R"({"t":"hello","proto":1})"));CHECK(r.ok(1,R"({"t":"project.new","template":"empty"})"));
        auto font=subsetFontFixture();std::string err;CHECK(r.ses.importSample("legacy.sf2",font.data(),font.size(),&err));
        double id=0;CHECK(r.ok(1,R"({"t":"channel.add","plugin":"org.smhgzll.soundfont"})",&id));
        CHECK(r.ok(1,sfmt(R"({"t":"channel.resource","channel":%g,"slot":"soundfont","sample":"legacy.sf2"})",id)));
        r.render(.05);r.ses.tick(2000);bool full=false;
        for(const auto &message:r.out){auto *j=cJSON_Parse(message.text.c_str());const auto *n=cJSON_GetObjectItem(j,"fileBufferBytes");full|=n && n->valuedouble>=font.size();cJSON_Delete(j);}
        CHECK(full);
    }
    // Tracker modules: the library takes them, samples.probe reads them through the Tracker
    // plugin, a Tracker channel plays the song, and a start inside the song's note continues it
    // there (note chase). Read-only parameters are not saved with the project.
    {
        Rig r(true, false, 0, 0, true); r.ses.boot(); r.render(.03);
        r.ses.clientConnected(1); CHECK(r.ok(1, R"({"t":"hello","proto":1})"));
        CHECK(r.ok(1, R"({"t":"project.new","template":"empty"})"));
        const auto mod = moduleFixture();
        std::string error;
        CHECK(r.ses.importSample("song.mod", mod.data(), mod.size(), &error));
        auto flt = moduleFixture("FLT4");  // a MOD variant libxmp-lite does not read
        CHECK(!r.ses.importSample("star.mod", flt.data(), flt.size(), &error));
        cJSON *a = r.req(1, R"({"t":"samples.list"})");
        bool listed = false;
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(cJSON_GetObjectItem(a, "library"), "list"))
            listed |= !strcmp(cJSON_GetObjectItem(e, "name")->valuestring, "song.mod") &&
                      !strcmp(cJSON_GetObjectItem(e, "type")->valuestring, "module");
        CHECK(listed);
        cJSON_Delete(a);
        a = r.req(1, R"({"t":"samples.probe","name":"song.mod","plugin":"org.smhgzll.tracker"})");
        const cJSON *v = cJSON_GetObjectItem(a, "values");  // results come in the ack
        CHECK(v != nullptr);
        if (v) {
            CHECK(fabs(cJSON_GetObjectItem(v, "length")->valuedouble - 7.68) < 0.01);
            CHECK(cJSON_GetObjectItem(v, "channels")->valuedouble == 4);
            CHECK(cJSON_GetObjectItem(v, "song_tempo")->valuedouble == 125);
            CHECK(cJSON_GetObjectItem(v, "format")->valuedouble == 1);
        }
        cJSON_Delete(a);
        CHECK(r.err(1, R"({"t":"samples.probe","name":"nothing.mod","plugin":"org.smhgzll.tracker"})") == "not_found");
        CHECK(r.err(1, R"({"t":"samples.probe","name":"song.mod","plugin":"org.smhgzll.sampler"})") == "bad_plugin");
        CHECK(r.err(1, R"({"t":"samples.probe","name":"song.mod","plugin":"org.smhgzll.soundfont"})") == "bad_file");
        double id = 0;
        CHECK(r.ok(1, R"({"t":"channel.add","plugin":"org.smhgzll.tracker"})", &id));
        CHECK(r.ok(1, sfmt(R"({"t":"channel.resource","channel":%g,"slot":"module","sample":"song.mod"})", id)));
        CHECK(r.ses.project().channels[0].inst.missingResources.empty());
        CHECK(r.ok(1, sfmt(R"({"t":"channel.param","channel":%g,"key":"tempo","v":125})", id)));
        CHECK(r.ok(1, R"({"t":"bpm","bpm":125})"));
        const double pat = (double)r.ses.project().patterns[0].id;
        CHECK(r.ok(1, sfmt(R"({"t":"pattern.set","pattern":%g,"length":15360})", pat)));
        CHECK(r.ok(1, sfmt(R"({"t":"notes.set","pattern":%g,"channel":%g,"notes":[[1,0,15360,60,0.78]]})", pat, id)));
        CHECK(r.ok(1, sfmt(R"({"t":"clips.set","clips":[{"kind":"pattern","ref":%g,"track":0,"start":0,"len":15360,"offset":0}]})", pat)));
        CHECK(r.ok(1, R"({"t":"mode","mode":"song"})"));
        CHECK(r.ok(1, R"({"t":"seek","tick":7680})"));  // the middle of the song
        r.render(.05);
        r.energy = 0;
        CHECK(r.ok(1, R"({"t":"play"})"));
        r.render(.5);
        CHECK(r.energy > 1.0);  // the song continues where playback starts
        CHECK(r.ok(1, R"({"t":"stop"})"));
        r.render(.1);
        const std::string doc = r.ses.exportProject();
        CHECK(doc.find("org.smhgzll.tracker") != std::string::npos && doc.find("song.mod") != std::string::npos);
        CHECK(doc.find("\"song_tempo\"") == std::string::npos && doc.find("\"memory\"") == std::string::npos);
        CHECK(doc.find("\"tempo\"") != std::string::npos);
    }
    // Module opening retains only the selected file, even with ample space or saved references.
    // Ordinary uploads preserve other files; whole / streamed opens and cached opens clear them.
    for (bool streamed : {false, true}) {
        Rig r(true, false, 0, 0, true);
        std::string err;
        auto mod = moduleFixture(); mod.resize(16 * 4096 - 64);
        for (const char *name : {"old.mod", "saved.mod", "autosaved.mod", "current.mod"})
            CHECK(r.samples.put(name, mod.data(), mod.size(), &err));
        CHECK(r.samples.putRaw("font.sf2", SampleType::SoundFont, mod.data(), mod.size(), &err));
        CHECK(r.samples.putRaw("tone.wav", SampleType::Wav, mod.data(), mod.size(), &err));
        CHECK(r.samples.put("unused.mod", mod.data(), mod.size(), &err));
        auto project = [](const char *file) {
            Project p = emptyProject(); Channel c;
            c.id = p.newId(); c.inst.pluginId = "org.smhgzll.tracker";
            c.inst.resources.emplace_back("module", file); p.channels.push_back(c);
            return p;
        };
        CHECK(r.store.save(1, "saved", projectToText(project("saved.mod")), 0, &err));
        CHECK(r.store.save(0, "autosaved", projectToText(project("autosaved.mod")), 0, &err));
        r.ses.start(project("current.mod")); r.render(.03);
        auto incoming = mod; incoming.resize(32 * 4096 - 64);
        CHECK(!r.ses.importSample("new.mod", incoming.data(), incoming.size(), &err));
        CHECK(r.samples.list().size() == 7);  // a normal full-library upload does not clear anything
        // Cached project documents, even corrupt ones, no longer pin disposable module files.
        CHECK(r.store.save(2, "corrupt", projectToText(project("old.mod")), 0, &err));
        r.flash.data()[2 * 2 * 128 * 1024 + 64] ^= 1;
        if (streamed) {
            CHECK(r.ses.importSampleBegin("new.mod", incoming.size(), incoming.data(), 4096, &err, true));
            CHECK(r.ses.importSampleAppend(incoming.data() + 4096, incoming.size() - 4096, &err));
            CHECK(r.ses.importSampleFinish(&err));
        } else {
            CHECK(r.ses.importSample("new.mod", incoming.data(), incoming.size(), &err, true));
        }
        CHECK(r.samples.list().size() == 3);
        for (const char *name : {"font.sf2", "tone.wav", "new.mod"}) CHECK(r.samples.verify(name, &err));
        CHECK(r.samples.freeBytes() == 64 * 4096);
        CHECK(r.store.info(1).valid && r.store.info(0).valid);  // project documents themselves remain
        r.ses.clientConnected(1); CHECK(r.ok(1, R"({"t":"hello","proto":1})"));
        CHECK(r.ses.importSample("other.mod", mod.data(), mod.size(), &err));  // normal upload, lots of room
        CHECK(r.samples.find("new.mod") && r.samples.find("other.mod"));
        const uint32_t seq = r.samples.find("new.mod")->seq;
        CHECK(r.err(1, R"({"t":"samples.keepModule","name":"missing.mod"})") == "not_found");
        CHECK(r.err(1, R"({"t":"samples.keepModule","name":"tone.wav"})") == "not_found");
        CHECK(r.samples.list().size() == 4);
        r.sampleFlash.failAfter(0);
        CHECK(r.err(1, R"({"t":"samples.keepModule","name":"new.mod"})") == "storage");
        r.sampleFlash.failAfter(-1);
        CHECK(r.samples.list().size() == 4);
        CHECK(r.ok(1, R"({"t":"samples.keepModule","name":"new.mod"})"));
        CHECK(r.samples.list().size() == 3 && r.samples.find("new.mod")->seq == seq);
        // Replacing a changed same-name module can discard its old copy when staging cannot fit.
        auto large = incoming; large.resize(64 * 4096 - 64);
        CHECK(r.ses.importSample("new.mod", large.data(), large.size(), &err, true));
        CHECK(r.samples.list().size() == 3 && r.samples.verify("new.mod", &err));
        CHECK(r.samples.verify("font.sf2", &err) && r.samples.verify("tone.wav", &err));
        // An in-flight streamed upload must not have its file reservations / cache cleared.
        CHECK(r.ses.importSampleBegin("pending.mod", mod.size(), mod.data(), 4096, &err));
        CHECK(r.err(1, R"({"t":"samples.keepModule","name":"new.mod"})") == "storage");
        r.ses.importSampleAbort();
    }
    TEST_MAIN_END("test_session");
}
