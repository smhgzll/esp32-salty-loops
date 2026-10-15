// SPDX-License-Identifier: GPL-3.0-or-later
// Transport and sequencer timing against the sample clock.
#include "check.hpp"
#include "probe.hpp"

using namespace daw;

static Pattern &pat(Project &p) { return p.patterns[0]; }

static void stepsOn(Project &p, int ch, std::initializer_list<int> on, int count = 16)
{
    ChannelSeq &s = pat(p).get(p.channels[(size_t)ch].id);
    s.resizeSteps(count);
    for (int i : on)
        s.steps[(size_t)i].on = true;
}

// every quarter note at several tempos: each note-on within one sample of the ideal position
static void testTempoAccuracy()
{
    for (double bpm : {20.0, 60.0, 120.0, 133.0, 174.0, 300.0}) {
        Project p = testProject(1, bpm);
        stepsOn(p, 0, {0, 4, 8, 12});
        Harness h(p);
        h.cmd(EngineCmd::Play);
        const double seconds = bpm < 50 ? 30.0 : 8.0;
        h.run(seconds);
        auto ons = h.ons(0);
        const double spb = 44100.0 * 60.0 / bpm;  // samples per beat
        CHECK(ons.size() >= (size_t)(seconds / (60.0 / bpm)) - 1);
        double worst = 0;
        for (size_t i = 0; i < ons.size(); i++)
            worst = fmax(worst, fabs((double)ons[i].at - i * spb));
        if (worst > 1.0)
            fprintf(stderr, "bpm %.0f: worst note-on deviation %.2f samples\n", bpm, worst);
        CHECK(worst <= 1.0);
    }
}

// a one-bar pattern loops seamlessly: no duplicate and no missing notes at the wrap
static void testLoopWrap()
{
    Project p = testProject(1, 128);
    stepsOn(p, 0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    Harness h(p, 128);
    h.cmd(EngineCmd::Play);
    h.run(10.0);
    auto ons = h.ons(0);
    const double sp16 = 44100.0 * 60.0 / 128.0 / 4.0;
    CHECK(ons.size() == (size_t)(10.0 / (sp16 / 44100.0)) || ons.size() == (size_t)(10.0 / (sp16 / 44100.0)) + 1);
    double worst = 0;
    for (size_t i = 1; i < ons.size(); i++)
        worst = fmax(worst, fabs((double)(ons[i].at - ons[i - 1].at) - sp16));
    CHECK(worst <= 1.0);  // loop boundaries align to the next sample
    // every note ends before the next starts (16th note length)
    CHECK(h.offs(0).size() + 1 >= ons.size());
}

// swing delays the odd steps by swing * half a step
static void testSwing()
{
    Project p = testProject(1, 120);
    stepsOn(p, 0, {0, 1, 2, 3});
    pat(p).swing = 0.5f;
    Harness h(p);
    h.cmd(EngineCmd::Play);
    h.run(1.0);
    auto ons = h.ons(0);
    const double sp16 = 44100.0 * 60.0 / 120.0 / 4.0;  // 5512.5
    CHECK(ons.size() >= 4);
    CHECK_NEAR((double)ons[0].at, 0, 1);
    CHECK_NEAR((double)ons[1].at, sp16 + 0.25 * sp16, 1);
    CHECK_NEAR((double)ons[2].at, 2 * sp16, 1);
    CHECK_NEAR((double)ons[3].at, 3 * sp16 + 0.25 * sp16, 1);
}

// independent step counts: a 3-step row against a 16-step row (polymeter)
static void testPolymeter()
{
    Project p = testProject(2, 120);
    stepsOn(p, 0, {0}, 3);
    stepsOn(p, 1, {0}, 16);
    Harness h(p);
    h.cmd(EngineCmd::Play);
    h.run(2.0);  // one bar at 120 BPM (+ the rest of the last block)
    auto firstBar = [](std::vector<ProbeEvent> v) {
        std::vector<ProbeEvent> o;
        for (auto &e : v)
            if (e.at < 88200)
                o.push_back(e);
        return o;
    };
    auto a = firstBar(h.ons(0)), b = firstBar(h.ons(1));
    CHECK(b.size() == 1);
    CHECK(a.size() == 6);  // 16 / 3 -> steps 0, 3, 6, 9, 12, 15
    const double sp16 = 5512.5;
    for (size_t i = 0; i < a.size(); i++)
        CHECK_NEAR((double)a[i].at, i * 3 * sp16, 1);
}

// piano roll notes: positions, lengths (note-off), velocity, key; a slide note-on precedes the
// previous note's note-off at the same sample
static void testPianoRollAndSlide()
{
    Project p = testProject(1, 120);
    ChannelSeq &s = pat(p).get(p.channels[0].id);
    s.pianoRoll = true;
    Note a;
    a.id = 1;
    a.start = 0;
    a.len = kPPQ;  // a quarter
    a.key = 48;
    a.vel = 0.5f;
    Note b = a;
    b.id = 2;
    b.start = kPPQ;
    b.key = 55;
    b.slide = true;
    s.notes = {a, b};
    Harness h(p);
    h.cmd(EngineCmd::Play);
    h.run(1.2);
    auto ons = h.ons(0);
    auto offs = h.offs(0);
    CHECK(ons.size() == 2);
    CHECK(ons[0].key == 48 && fabsf(ons[0].vel - 0.5f) < 1e-6f);
    CHECK_NEAR((double)ons[1].at, 22050, 1);
    CHECK(ons[1].flags & DAW_NOTE_FLAG_SLIDE);
    // the first note's off is at 22050 too, and comes after the slide note-on in the event stream
    size_t iSlide = 0, iOff = 0;
    for (size_t i = 0; i < g_probe.size(); i++) {
        if (g_probe[i].type == DAW_EV_NOTE_ON && g_probe[i].key == 55)
            iSlide = i;
        if (g_probe[i].type == DAW_EV_NOTE_OFF && g_probe[i].key == 48)
            iOff = i;
    }
    CHECK(offs.size() >= 1);
    CHECK_NEAR((double)offs[0].at, 22050, 1);
    CHECK(iSlide < iOff);
}

// a queued pattern starts exactly at the end of the current one
static void testPatternQueue()
{
    Project p = testProject(1, 120);
    stepsOn(p, 0, {0});
    Pattern p2;
    p2.id = p.newId();
    p2.name = "P2";
    ChannelSeq &s2 = p2.get(p.channels[0].id);
    s2.steps[8].on = true;
    s2.steps[8].key = 72;
    p.patterns.push_back(p2);
    Harness h(p);
    h.cmd(EngineCmd::Play);
    h.run(0.5);
    h.cmd(EngineCmd::QueuePattern, 0, 1);
    h.run(3.0);
    auto ons = h.ons(0);
    CHECK(ons.size() >= 2);
    CHECK(ons[0].key == 60 && ons[0].at == 0);
    // pattern 1 is one bar (88200 samples); pattern 2's step 8 is half a bar later
    CHECK(ons[1].key == 72);
    CHECK_NEAR((double)ons[1].at, 88200 + 44100, 1);
}

// stop releases everything; pause / continue resumes where it was; count-in delays the start
static void testTransport()
{
    {
        Project p = testProject(1, 120);
        ChannelSeq &s = pat(p).get(p.channels[0].id);
        s.pianoRoll = true;
        Note n;
        n.id = 1;
        n.start = 0;
        n.len = kPPQ * 8;  // longer than the run
        s.notes = {n};
        Harness h(p);
        h.cmd(EngineCmd::Play);
        h.run(0.5);
        CHECK(h.offs(0).empty());
        h.cmd(EngineCmd::Stop);
        h.run(0.1);
        CHECK(h.offs(0).size() == 1);
        bool allOff = false;
        for (auto &e : g_probe)
            allOff |= e.type == DAW_EV_ALL_NOTES_OFF;
        CHECK(allOff);
        CHECK(h.eng.status().playing.load() == 0);
        CHECK(h.eng.status().tick.load() == 0);
    }
    {
        Project p = testProject(1, 120);
        stepsOn(p, 0, {0, 8});
        Harness h(p);
        h.cmd(EngineCmd::Play);
        h.run(0.5);
        h.cmd(EngineCmd::Pause);
        h.run(0.1);
        const int32_t t = h.eng.status().tick.load();
        h.run(1.0);
        CHECK(h.eng.status().tick.load() == t);  // frozen while paused
        CHECK(h.eng.status().playing.load() == 2);
        const size_t before = h.ons(0).size();
        h.cmd(EngineCmd::Continue);
        h.run(1.0);
        auto ons = h.ons(0);
        CHECK(ons.size() == before + 1);  // step 8 after the resume
    }
    {
        Project p = testProject(1, 120);
        stepsOn(p, 0, {0});
        Harness h(p);
        h.cmd(EngineCmd::SetCountIn, 1);
        daw::EngineCmd m{EngineCmd::SetMetronome, 1};
        m.f[0] = 1.0f;
        h.post(m);
        h.cmd(EngineCmd::Play);
        h.run(2.5);
        auto ons = h.ons(0);
        CHECK(ons.size() == 1);
        CHECK_NEAR((double)ons[0].at, 88200, 1);  // after one bar of count-in
        // the count-in clicks are audible at the beats (the probe itself is silent except its marker)
        CHECK(fabsf(h.L[10]) > 0.05f);
        CHECK(fabsf(h.L[22050 + 10]) > 0.05f);
    }
}

// tempo change mid-pattern: notes follow the new tempo from the change on; already rendered
// output is not touched
static void testTempoChange()
{
    Project p = testProject(1, 120);
    stepsOn(p, 0, {0, 4, 8, 12});
    Harness h(p);
    h.cmd(EngineCmd::Play);
    h.run(1.0);  // beats at 0, 22050 (120 BPM)
    daw::EngineCmd b{EngineCmd::SetTempo};
    b.d = 60.0;
    h.post(b);
    h.run(3.0);
    auto ons = h.ons(0);
    CHECK(ons.size() >= 4);
    CHECK_NEAR((double)ons[1].at, 22050, 1);
    CHECK_NEAR((double)ons[2].at, 44100, 0.5);  // before the change (1.0 s = 173 blocks = 44288)
    // the change takes effect at the block boundary: beat 3 is reached at 60 BPM from there
    const double changeAt = 44288.0;
    const double ticksAtChange = changeAt * 120.0 * kPPQ / 60.0 / 44100.0;
    const double expect3 = changeAt + (3 * kPPQ - ticksAtChange) * 44100.0 / kPPQ;  // 60 BPM: kPPQ ticks per second
    CHECK_NEAR((double)ons[3].at, expect3, 1.0);
}

// song mode: clips with offsets, repetition of a short pattern inside a long clip, truncation of
// notes at the clip end, muted tracks
static void testSong()
{
    Project p = testProject(1, 120);
    ChannelSeq &s = pat(p).get(p.channels[0].id);
    s.pianoRoll = true;
    Note n;
    n.id = 1;
    n.start = 0;
    n.len = kPPQ * 3;
    n.key = 60;
    Note m = n;
    m.id = 2;
    m.start = kPPQ * 2;
    m.key = 64;
    m.len = kPPQ;
    s.notes = {n, m};
    pat(p).lengthTicks = kPPQ * 4;
    const int32_t bar = kPPQ * 4;
    p.tracks.resize(8);
    auto clip = [&](int track, int32_t start, int32_t len, int32_t off) {
        Clip c;
        c.id = p.newId();
        c.track = track;
        c.start = start;
        c.len = len;
        c.offset = off;
        c.ref = pat(p).id;
        p.clips.push_back(c);
    };
    clip(0, 0, 2 * bar, 0);            // two repetitions
    clip(1, 2 * bar, bar / 2, kPPQ * 2); // starts inside the pattern at beat 3: only the E, truncated? (len 1 beat fits)
    clip(2, 3 * bar, bar, 0);
    p.tracks[2].mute = true;            // muted track: nothing
    Harness h(p);
    h.cmd(EngineCmd::SetMode, 1);
    h.cmd(EngineCmd::Play);
    h.run(8.0 - 0.01);  // 4 bars
    auto ons = h.ons(0);
    // bar 1: C at 0, E at beat 2; bar 2: C, E; bar 3 (clip 2): E at its start
    CHECK(ons.size() == 5);
    const double spb = 22050;
    CHECK_NEAR((double)ons[0].at, 0, 1);
    CHECK_NEAR((double)ons[1].at, 2 * spb, 1);
    CHECK_NEAR((double)ons[2].at, 4 * spb, 1);
    CHECK_NEAR((double)ons[3].at, 6 * spb, 1);
    CHECK(ons[4].key == 64);
    CHECK_NEAR((double)ons[4].at, 8 * spb, 1);
    // the C of the second repetition (3 beats long) is cut at the clip end (bar 2 end = beat 8)
    for (auto &o : h.offs(0))
        CHECK(o.at <= (uint64_t)(8 * spb + 1) || o.key == 64);
}

// automation clip drives a plugin parameter (events) and the tempo
static void testAutomation()
{
    Project p = testProject(1, 120);
    stepsOn(p, 0, {0});
    Automation a;
    a.id = p.newId();
    a.target.kind = AutomationTarget::ChannelParam;
    a.target.channel = p.channels[0].id;
    a.target.param = "level";
    a.points = {{0, 0.0f, 0}, {kPPQ * 4, 1.0f, 0}};
    p.automation.push_back(a);
    Clip c;
    c.id = p.newId();
    c.kind = ClipKind::Automation;
    c.track = 1;
    c.start = 0;
    c.len = kPPQ * 4;
    c.ref = a.id;
    p.clips.push_back(c);
    Clip pc;
    pc.id = p.newId();
    pc.track = 0;
    pc.start = 0;
    pc.len = kPPQ * 4;
    pc.ref = pat(p).id;
    p.clips.push_back(pc);
    Harness h(p);
    h.cmd(EngineCmd::SetMode, 1);
    h.cmd(EngineCmd::Play);
    h.run(1.0);  // half the ramp
    std::vector<float> vals;
    for (auto &e : g_probe)
        if (e.type == DAW_EV_PARAM)
            vals.push_back(e.value);
    CHECK(vals.size() > 50);
    bool monotonic = true;
    for (size_t i = 1; i < vals.size(); i++)
        monotonic &= vals[i] >= vals[i - 1];
    CHECK(monotonic);
    CHECK(vals.front() < 0.01f);
    CHECK_NEAR(vals.back(), 0.5, 0.02);
}

// note probability: deterministic, and roughly the requested share
static void testProbability()
{
    auto runOnce = [] {
        Project p = testProject(1, 300);
        ChannelSeq &s = pat(p).get(p.channels[0].id);
        s.resizeSteps(16);
        for (auto &st : s.steps) {
            st.on = true;
            st.prob = 0.5f;
        }
        Harness h(p);
        h.cmd(EngineCmd::Play);
        h.run(16.0);
        std::vector<uint64_t> at;
        for (auto &e : h.ons(0))
            at.push_back(e.at);
        return at;
    };
    auto a = runOnce(), b = runOnce();
    CHECK(a == b);
    const double total = 16.0 / (60.0 / 300.0 / 4.0);  // 320 steps
    CHECK(a.size() > total * 0.4 && a.size() < total * 0.6);
}

// the active-note table and the event pool overflow gracefully (counted, no crash)
static void testOverload()
{
    Project p = testProject(2, 300);
    ChannelSeq &s = pat(p).get(p.channels[0].id);
    s.pianoRoll = true;
    for (int i = 0; i < 600; i++) {
        Note n;
        n.id = (Id)i + 1;
        n.start = (i % 16) * 10;
        n.len = kPPQ * 16;
        n.key = (uint8_t)(i % 128);
        s.notes.push_back(n);
    }
    s.sortNotes();
    Harness h(p);
    h.cmd(EngineCmd::Play);
    h.run(2.0);
    CHECK(h.eng.status().noteDrops.load() + h.eng.status().eventDrops.load() > 0);
    CHECK(h.eng.status().playing.load() == 1);
}

// ABI 1.2 note chase: a plugin asking for it hears the notes the position lands inside
static void testChase()
{
    const int32_t bar = kPPQ * 4;
    const double spb = 22050;  // samples per beat at 120 BPM
    auto project = [&](int32_t patLen, std::vector<Note> notes) {
        Project p = testProject(2, 120);
        p.channels[0].inst.pluginId = "test.chase";
        for (int c = 0; c < 2; c++) {
            ChannelSeq &s = pat(p).get(p.channels[c].id);
            s.pianoRoll = true;
            s.notes = notes;
        }
        pat(p).lengthTicks = patLen;
        p.tracks.resize(2);
        return p;
    };
    auto note = [](uint32_t id, int32_t start, int32_t len) {
        Note n;
        n.id = id;
        n.start = start;
        n.len = len;
        n.key = 60;
        return n;
    };
    auto addClip = [](Project &p, int track, int32_t start, int32_t len, int32_t off) {
        Clip c;
        c.id = p.newId();
        c.track = track;
        c.start = start;
        c.len = len;
        c.offset = off;
        c.ref = p.patterns[0].id;
        p.clips.push_back(c);
    };
    auto seek = [](Harness &h, int32_t tick) {
        EngineCmd c{EngineCmd::Seek};
        c.i[0] = tick;
        h.post(c);
    };
    {  // starting inside a long note: only the chasing plugin hears it, with the elapsed part
        // (one clip plays the pattern's notes of both channels)
        Project p = project(4 * bar, {note(1, 0, 4 * bar)});
        addClip(p, 0, 0, 4 * bar, 0);
        Harness h(p);
        h.cmd(EngineCmd::SetMode, 1);
        seek(h, 2 * bar);
        h.cmd(EngineCmd::Play);
        h.run(4.05);  // to just after the song's end, where it starts again (plain note-ons)
        auto on0 = h.ons(0), on1 = h.ons(1);
        CHECK(on0.size() == 2 && on1.size() == 1);
        CHECK(on1[0].at == 176400 && !(on1[0].flags & DAW_NOTE_FLAG_CHASE));
        if (on0.size() == 1) {
            CHECK(on0[0].at == 0);
            CHECK(on0[0].flags & DAW_NOTE_FLAG_CHASE);
            CHECK(on0[0].mbeats == 8000);
        }
        auto off0 = h.offs(0);
        CHECK(off0.size() == 1 && off0[0].id == on0[0].id);
        if (!off0.empty())
            CHECK_NEAR((double)off0[0].at, 8 * spb, 1);
        if (on0.size() == 2)
            CHECK(on0[1].at == 176400 && !(on0[1].flags & DAW_NOTE_FLAG_CHASE));
    }
    {  // a loop region inside the note: chased at the start and again at every wrap, after a note-off
        Project p = project(4 * bar, {note(1, 0, 4 * bar)});
        addClip(p, 0, 0, 4 * bar, 0);
        Harness h(p);
        h.cmd(EngineCmd::SetMode, 1);
        EngineCmd lp{EngineCmd::SetLoop, 1};
        lp.i[0] = bar;
        lp.i[1] = 2 * bar;
        h.post(lp);
        seek(h, bar);
        h.cmd(EngineCmd::Play);
        h.run(4.5);  // the 2 s region twice and a bit
        auto on0 = h.ons(0);
        CHECK(on0.size() == 3);
        for (size_t i = 0; i < on0.size(); i++) {
            CHECK(on0[i].mbeats == 4000);
            CHECK_NEAR((double)on0[i].at, i * 4 * spb, 1);
        }
        auto off0 = h.offs(0);
        CHECK(off0.size() == 2);
        for (size_t i = 0; i < off0.size() && i + 1 < on0.size(); i++) {
            CHECK(off0[i].id == on0[i].id);
            CHECK(off0[i].at == on0[i + 1].at);
        }
        CHECK(h.ons(1).empty());
    }
    {  // a note that begins before its clip does never sounds, chased or not
        Project p = project(4 * bar, {note(1, 0, 4 * bar)});
        addClip(p, 0, bar, 2 * bar, 2 * bar);
        Harness h(p);
        h.cmd(EngineCmd::SetMode, 1);
        seek(h, 2 * bar);
        h.cmd(EngineCmd::Play);
        h.run(1.0);
        CHECK(h.ons(0).empty());
    }
    {  // notes of the previous repetition reach into this one; both are chased
        Project p = project(bar, {note(1, 0, 2 * bar)});
        addClip(p, 0, 0, 4 * bar, 0);
        Harness h(p);
        h.cmd(EngineCmd::SetMode, 1);
        seek(h, bar + bar / 2);
        h.cmd(EngineCmd::Play);
        h.run(0.2);
        auto on0 = h.ons(0);
        CHECK(on0.size() == 2);
        if (on0.size() == 2)
            CHECK((on0[0].mbeats == 2000 && on0[1].mbeats == 6000) || (on0[0].mbeats == 6000 && on0[1].mbeats == 2000));
    }
    {  // pattern mode; pause and continue chase too; a note starting at the position is not doubled
        Project p = project(4 * bar, {note(1, 0, 4 * bar), note(2, bar, kPPQ)});
        Harness h(p);
        seek(h, bar);
        h.cmd(EngineCmd::Play);
        h.run(0.5);
        auto on0 = h.ons(0);
        CHECK(on0.size() == 2);  // the long note chased, the short one at its own start
        if (on0.size() == 2) {
            CHECK(on0[0].mbeats == 4000 && (on0[0].flags & DAW_NOTE_FLAG_CHASE));
            CHECK(!(on0[1].flags & DAW_NOTE_FLAG_CHASE));
        }
        h.cmd(EngineCmd::Pause);
        h.run(0.1);
        h.cmd(EngineCmd::Continue);
        h.run(0.1);
        on0 = h.ons(0);
        CHECK(on0.size() == 3);
        if (on0.size() == 3)
            CHECK((on0[2].flags & DAW_NOTE_FLAG_CHASE) && on0[2].mbeats > 4000 && on0[2].mbeats < 6000);
        CHECK(h.ons(1).size() == 1);  // the plain probe: only the short note
    }
}

int main()
{
    testTempoAccuracy();
    testLoopWrap();
    testSwing();
    testPolymeter();
    testPianoRollAndSlide();
    testPatternQueue();
    testTransport();
    testTempoChange();
    testSong();
    testAutomation();
    testProbability();
    testOverload();
    testChase();
    TEST_MAIN_END("test_sequencer");
}
