// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "probe.hpp"
#include "daw/project_json.hpp"
#include "cJSON.h"
#include <algorithm>
#include <stdlib.h>
using namespace daw;
struct Seen { uint64_t at; int slot; daw_event_t ev; std::vector<uint8_t> bytes; };
static std::vector<Seen> seen;
static void capture(int slot, const daw_process_t *c) {
    for (uint32_t i = 0; i < c->event_count; ++i) {
        Seen s{c->sample_pos + c->events[i].offset, slot, c->events[i], {}};
        if (s.ev.type == DAW_EV_MIDI_BYTES) s.bytes.assign(s.ev.u.bytes.data, s.ev.u.bytes.data + s.ev.u.bytes.size);
        seen.push_back(std::move(s));
    }
}
static void testRoundTripAndValidation() {
    Project p = testProject(1, 120);
    auto &q = p.patterns[0].get(p.channels[0].id);
    q.pianoRoll = true;
    Note note; note.id = 1; note.midiOnOrder = 4; note.midiOffOrder = 10; q.notes.push_back(note);
    q.midi = {{0, 2, {0xc0, 40}}, {30, 4, {0xb0, 101, 0}}, {50, 2, {0xe0, 1, 127}},
              {60, 2, {0xa0, 60, 99}}, {70, 2, {0xd0, 89}}, {80, 2, {0xf0, 0x7d, 1, 2, 0xf7}}, {90, 2, {0xff, 5, 65}}};
    p.normalise();
    std::string text = projectToText(p), err; Project restored;
    CHECK(projectFromText(text.data(), text.size(), restored, &err));
    CHECK(projectToText(restored) == text);
    CHECK(text.find("\"schema\":2") != std::string::npos);
    CHECK(projectToText(testProject(1)).find("\"schema\":1") != std::string::npos);
    const auto &s = restored.patterns[0].seqs[0];
    CHECK(s.notes[0].midiOnOrder == 4 && s.notes[0].midiOffOrder == 10);
    CHECK(s.midi.size() == q.midi.size() && s.midi.back().data == q.midi.back().data);
    for (const char *bad : {"[[0,2,144,60,100]]", "[[-1,2,176,7,100]]", "[[0,2,176,7,128]]", "[[0,2,192,1,2]]", "[[0,2,224,0]]", "[[0,2,112]]", "[[0,2,241,1,2]]", "[[0,2,176,7,1.5]]"}) {
        cJSON *j = cJSON_Parse(bad); auto before = s.midi; std::vector<MidiEvent> out = before;
        CHECK(!midiFromJson(j, out, &err)); CHECK(out.size() == before.size()); cJSON_Delete(j);
    }
}
static void testReadAllocationFailure() {
    Project previous = defaultProject();
    const auto name = previous.name; const auto count = previous.channels.size();
    cJSON *input = projectToJson(previous);
    cJSON_Hooks fail{[](size_t) -> void * { return nullptr; }, ::free};
    cJSON_InitHooks(&fail);
    std::string error;
    const bool ok = projectFromJson(input, previous, &error);
    cJSON_InitHooks(nullptr); cJSON_Delete(input);
    CHECK(!ok); CHECK(!error.empty()); CHECK(previous.name == name && previous.channels.size() == count);
}
static void testTimingOrderingAndPayloads() {
    Project p = testProject(1, 120); auto &q = p.patterns[0].get(p.channels[0].id); q.pianoRoll = true;
    Note n; n.id = 1; n.len = 960; n.midiOnOrder = 4; n.midiOffOrder = 2; q.notes.push_back(n);
    q.midi = {{0, 2, {0xc0, 40}}, {0, 6, {0xb0, 7, 90}}, {480, 2, {0xe0, 0, 96}}, {960, 4, {0xd0, 80}},
              {1000, 2, {0xa0, 60, 99}}, {1100, 2, {0xf0, 0x7d, 1, 2, 0xf7}}, {1200, 2, {0xff, 5, 65}}};
    Harness h(p); seen.clear(); g_probeHook = capture; h.cmd(EngineCmd::Play); h.run(.8); g_probeHook = nullptr;
    std::vector<Seen> first;
    for (const auto &e : seen) if (e.at == 0 && e.ev.type != DAW_EV_MIDI_RESET && e.ev.type != DAW_EV_ALL_SOUND_OFF) first.push_back(e);
    CHECK(first.size() == 3);
    if (first.size() == 3) CHECK(first[0].ev.type == DAW_EV_PROGRAM && first[1].ev.type == DAW_EV_NOTE_ON && first[2].ev.type == DAW_EV_CC);
    bool bend = false, pressure = false, poly = false, packet = false;
    for (const auto &e : seen) {
        if (e.ev.type == DAW_EV_PITCH_BEND) { bend = true; CHECK_NEAR(e.at, 11025, 1); CHECK_NEAR(e.ev.u.midi.value, .5, 1e-6); }
        if (e.ev.type == DAW_EV_CHANNEL_PRESSURE) { pressure = true; CHECK_NEAR(e.at, 22050, 1); }
        if (e.ev.type == DAW_EV_POLY_PRESSURE) { poly = true; CHECK(e.ev.u.midi.number == 60); }
        if (e.ev.type == DAW_EV_MIDI_BYTES) { packet = true; CHECK(e.bytes == std::vector<uint8_t>({0xf0, 0x7d, 1, 2, 0xf7})); }
    }
    CHECK(bend && pressure && poly && packet); CHECK(h.eng.status().eventDrops.load() == 0);
}
static void testControllerLaneLengthAndSeek() {
    Project p = testProject(1, 120); p.patterns[0].lengthTicks = kPPQ * 16;
    auto &q = p.patterns[0].get(p.channels[0].id); // default 16-step row, no notes
    q.midi = {{0, 2, {0xb0, 101, 0}}, {1, 2, {0xb0, 100, 0}}, {2, 2, {0xb0, 6, 12}}, {5000, 2, {0xc0, 40}}};
    Harness h(p); seen.clear(); g_probeHook = capture; h.cmd(EngineCmd::Play); h.run(3.0);
    int initialCC = 0, program = 0;
    for (const auto &e : seen) { if (e.ev.type == DAW_EV_CC) ++initialCC; if (e.ev.type == DAW_EV_PROGRAM) ++program; }
    CHECK(initialCC == 3 && program == 1); // MIDI does not repeat every default step row
    h.cmd(EngineCmd::Stop); h.run(.01); seen.clear();
    EngineCmd seek{EngineCmd::Seek}; seek.i[0] = 6000; h.post(seek); h.cmd(EngineCmd::Play); h.run(.02);
    std::vector<int> ccs; bool preset = false;
    for (const auto &e : seen) { if (e.ev.type == DAW_EV_CC) ccs.push_back(e.ev.u.midi.number); preset |= e.ev.type == DAW_EV_PROGRAM; }
    CHECK(ccs == std::vector<int>({101, 100, 6})); CHECK(preset); CHECK(h.eng.status().eventDrops.load() == 0);
    g_probeHook = nullptr;
}
static void testBoundedSeek() {
    Project p = testProject(1, 120); auto &q = p.patterns[0].get(p.channels[0].id);
    q.pianoRoll = true; p.patterns[0].lengthTicks = 16000;
    for (int i = 0; i < 2000; ++i) q.midi.push_back({i, 2, {0xb0, uint8_t(i % 2 ? 100 : 101), uint8_t(i & 127)}});
    Harness h(p); EngineCmd seek{EngineCmd::Seek}; seek.i[0] = 10000; h.post(seek); h.cmd(EngineCmd::Play); h.run(.4);
    CHECK(h.eng.status().eventDrops.load() == 0); CHECK(h.eng.status().tick.load() > 10000);
}
int main() { testRoundTripAndValidation(); testReadAllocationFailure(); testTimingOrderingAndPayloads(); testControllerLaneLengthAndSeek(); testBoundedSeek(); TEST_MAIN_END("MIDI event persistence and sequencing"); }
