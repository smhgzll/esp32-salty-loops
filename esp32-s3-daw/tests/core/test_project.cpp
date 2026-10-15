// SPDX-License-Identifier: GPL-3.0-or-later
// Project file format: round trips, missing plugins, corrupt input, limits, compile / routing.
#include <string.h>

#include <string>

#include "cJSON.h"
#include "check.hpp"
#include "daw/compile.hpp"
#include "daw/model.hpp"
#include "daw/plugin_host.hpp"
#include "daw/project_json.hpp"

using namespace daw;

static bool known(const std::string &id) { return PluginRegistry::find(id) != nullptr; }

int main()
{
    setPluginKnownCheck(known);
    // the demo project survives a text round trip unchanged
    {
        Project p = defaultProject();
        std::string a = projectToText(p);
        Project q;
        std::string err;
        CHECK(projectFromText(a.data(), a.size(), q, &err));
        std::string b = projectToText(q);
        CHECK(a == b);
        CHECK(q.channels.size() == 8 && q.patterns.size() == 3 && q.clips.size() == 3);
        CHECK(q.mixer.size() == (size_t)kMaxMixerTracks && q.mixer[0].fx.size() == 1);
        CHECK(q.patterns[1].seqs[0].notes[7].slide);
        CHECK(a.size() < 16000);
    }
    // Runtime load errors are visible, but are not restored as persistent plugin state.
    {
        Project p = defaultProject();
        p.channels[0].inst.loadError = "not enough internal RAM";
        const std::string text = projectToText(p);
        CHECK(text.find("loadError") != std::string::npos);
        Project q;
        CHECK(projectFromText(text.data(), text.size(), q, nullptr));
        // A saved runtime warning must not prevent a later successful load.
        CHECK(q.channels[0].inst.loadError.empty());
    }
    // an empty project
    {
        Project p = emptyProject();
        std::string a = projectToText(p), err;
        Project q;
        CHECK(projectFromText(a.data(), a.size(), q, &err));
        CHECK(q.channels.empty() && q.patterns.size() == 1);
    }
    // an unknown plugin is kept verbatim, flagged missing, and written back unchanged
    {
        Project p = defaultProject();
        p.channels[0].inst.pluginId = "com.example.not-here";
        p.channels[0].inst.params = {{"secret", 0.25f}};
        p.channels[0].inst.blob = "AAEC";
        std::string a = projectToText(p), err;
        Project q;
        CHECK(projectFromText(a.data(), a.size(), q, &err));
        CHECK(q.channels[0].inst.missing);
        CHECK(!q.channels[1].inst.missing);
        std::string b = projectToText(q);
        CHECK(b.find("\"com.example.not-here\"") != std::string::npos);
        CHECK(b.find("\"blob\":\"AAEC\"") != std::string::npos);
        CHECK(b.find("\"secret\":0.25") != std::string::npos);
    }
    // corrupt / foreign / future input is refused with a reason, never half-loaded
    {
        Project q = defaultProject();
        std::string err;
        const char *bad[] = {"", "{", "[]", "{\"format\":\"other\"}", "{\"format\":\"esp32-daw-project\",\"schema\":99}",
                             "{\"format\":\"esp32-daw-project\",\"schema\":0}"};
        for (const char *b : bad) {
            err.clear();
            CHECK(!projectFromText(b, strlen(b), q, &err));
            CHECK(!err.empty());
        }
        CHECK(q.channels.size() == 8);  // untouched
    }
    // hostile values are clamped: NaN, out-of-range, huge counts, bad references
    {
        const char *t = R"({"format":"esp32-daw-project","schema":1,"meta":{"bpm":99999,"timeSig":[0,3]},
          "channels":[{"id":5,"vol":-3,"pan":7,"track":999,"plugin":{"id":"org.smhgzll.sine","params":{"level":1e30}}}],
          "patterns":[{"id":9,"swing":5,"seqs":[{"channel":5,"stepCount":100000,"notes":[[1,-50,0,300,9,9,9,9,9,1]]},
                                             {"channel":77,"stepCount":4}]}],
          "playlist":{"clips":[{"id":3,"ref":12345,"start":0,"len":100},{"id":4,"ref":9,"start":-5,"len":-1,"track":99}]}})";
        Project q;
        std::string err;
        CHECK(projectFromText(t, strlen(t), q, &err));
        CHECK(q.bpm == kMaxBpm);
        CHECK(q.ts.num == 4 && q.ts.den == 4);
        CHECK(q.channels[0].vol == 0.0f && q.channels[0].pan == 1.0f && q.channels[0].track == 0);
        CHECK(q.patterns[0].swing == 1.0f);
        CHECK(q.patterns[0].seqs.size() == 1);  // the sequence of a channel that does not exist is dropped
        const ChannelSeq &s = q.patterns[0].seqs[0];
        CHECK(s.stepCount == kMaxSteps && s.steps.size() == (size_t)kMaxSteps);
        CHECK(s.notes[0].start == 0 && s.notes[0].len >= 1 && s.notes[0].key == 127 && s.notes[0].vel == 1.0f);
        CHECK(q.clips.size() == 1);  // the clip pointing at no pattern is dropped
        CHECK(q.clips[0].start == 0 && q.clips[0].len == 1 && q.clips[0].track < (int)q.tracks.size());
        CHECK(q.nextId > 9);
    }
    // mixer routing: chains compile in dependency order; cycles are refused
    {
        Project p = defaultProject();
        p.mixer[1].route = 5;
        p.mixer[5].route = 9;
        p.mixer[2].sends = {{9, 0.5f}};
        std::string err;
        MixModel *m = compileMix(p, &err);
        CHECK(m != nullptr);
        if (m) {
            int pos[kEngineTracks];
            for (int i = 0; i < m->count; i++)
                pos[m->order[i]] = i;
            CHECK(m->count == kEngineTracks && m->order[m->count - 1] == 0);
            CHECK(pos[1] < pos[5] && pos[5] < pos[9] && pos[2] < pos[9]);
            freeMix(m);
        }
        p.mixer[9].route = 1;  // 1 -> 5 -> 9 -> 1
        CHECK(compileMix(p, &err) == nullptr);
        CHECK(err.find("cycle") != std::string::npos);
        p.mixer[9].route = 0;
        p.mixer[9].sends = {{2, 0.3f}};  // 2 -send-> 9 -send-> 2
        CHECK(compileMix(p, &err) == nullptr);
    }
    // the play model: pattern lengths, step conversion, clip order, slots
    {
        Project p = defaultProject();
        auto slotOf = [&](Id ch) { return p.channelIndex(ch); };
        auto resolve = [](const AutomationTarget &, PlayAuto &) { return false; };
        PlayModel *m = compilePlay(p, slotOf, resolve, 7);
        CHECK(m && m->version == 7 && m->patternCount == 3);
        CHECK(m->patterns[0].length == kPPQ * 4);
        CHECK(m->patterns[0].seqCount == 4);
        CHECK(m->patterns[0].seqs[0].count == 4 && m->patterns[0].seqs[0].loopLen == 16 * kStepTicks);
        CHECK(m->patterns[2].length == kPPQ * 16);
        CHECK(m->clipCount == 3 && m->clips[0].start <= m->clips[1].start);
        CHECK(m->songLength == kPPQ * 4 * 8);
        freePlay(m);
    }
    // the plugin registry: every imported plugin passes the DAW's ABI check
    {
        CHECK(PluginRegistry::all().size() >= 7);
        CHECK(PluginRegistry::find("org.smhgzll.3xosc") != nullptr);
        cJSON *cat = pluginCatalogJson();
        CHECK(cJSON_GetArraySize(cat) == (int)PluginRegistry::all().size());
        cJSON_Delete(cat);
    }
    TEST_MAIN_END("test_project");
}
