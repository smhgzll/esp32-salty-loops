// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/model.hpp"

#include <math.h>

#include <algorithm>

namespace daw {

const std::string *PluginState::resource(const std::string &slot) const
{
    for (const auto &r : resources)
        if (r.first == slot)
            return &r.second;
    return nullptr;
}

float *PluginState::param(const std::string &key)
{
    for (auto &p : params)
        if (p.first == key)
            return &p.second;
    return nullptr;
}

void PluginState::setParam(const std::string &key, float v)
{
    if (float *p = param(key))
        *p = v;
    else
        params.emplace_back(key, v);
}

void ChannelSeq::resizeSteps(int n)
{
    n = std::clamp(n, 1, kMaxSteps);
    stepCount = (uint16_t)n;
    steps.resize((size_t)n);
}

void ChannelSeq::sortNotes()
{
    std::stable_sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) {
        return a.start != b.start ? a.start < b.start : a.key < b.key;
    });
}

ChannelSeq *Pattern::find(Id channel)
{
    for (auto &s : seqs)
        if (s.channel == channel)
            return &s;
    return nullptr;
}

const ChannelSeq *Pattern::find(Id channel) const
{
    for (auto &s : seqs)
        if (s.channel == channel)
            return &s;
    return nullptr;
}

ChannelSeq &Pattern::get(Id channel)
{
    if (ChannelSeq *s = find(channel))
        return *s;
    ChannelSeq s;
    s.channel = channel;
    s.resizeSteps(16);
    seqs.push_back(std::move(s));
    return seqs.back();
}

float Automation::valueAt(int32_t tick) const
{
    if (points.empty())
        return 0.5f;
    if (tick <= points.front().tick)
        return points.front().value;
    if (tick >= points.back().tick)
        return points.back().value;
    auto it = std::upper_bound(points.begin(), points.end(), tick, [](int32_t t, const AutoPoint &p) { return t < p.tick; });
    const AutoPoint &b = *it, &a = *(it - 1);
    float t = b.tick > a.tick ? (float)(tick - a.tick) / (float)(b.tick - a.tick) : 1.0f;
    if (a.tension != 0.0f)
        t = powf(t, exp2f(a.tension * 3.0f));
    return a.value + (b.value - a.value) * t;
}

Channel *Project::channel(Id id)
{
    for (auto &c : channels)
        if (c.id == id)
            return &c;
    return nullptr;
}

Pattern *Project::pattern(Id id)
{
    for (auto &p : patterns)
        if (p.id == id)
            return &p;
    return nullptr;
}

const Pattern *Project::pattern(Id id) const
{
    for (auto &p : patterns)
        if (p.id == id)
            return &p;
    return nullptr;
}

Automation *Project::automationById(Id id)
{
    for (auto &a : automation)
        if (a.id == id)
            return &a;
    return nullptr;
}

Clip *Project::clip(Id id)
{
    for (auto &c : clips)
        if (c.id == id)
            return &c;
    return nullptr;
}

FxSlot *Project::fx(Id id, int *trackOut)
{
    for (auto &t : mixer)
        for (auto &f : t.fx)
            if (f.id == id) {
                if (trackOut)
                    *trackOut = t.index;
                return &f;
            }
    return nullptr;
}

int Project::channelIndex(Id id) const
{
    for (size_t i = 0; i < channels.size(); i++)
        if (channels[i].id == id)
            return (int)i;
    return -1;
}

int Project::patternIndex(Id id) const
{
    for (size_t i = 0; i < patterns.size(); i++)
        if (patterns[i].id == id)
            return (int)i;
    return -1;
}

int32_t Project::patternLength(const Pattern &p) const
{
    if (p.lengthTicks > 0)
        return p.lengthTicks;
    int32_t end = 0;
    for (const auto &s : p.seqs) {
        if (s.pianoRoll) {
            for (const auto &n : s.notes)
                end = std::max(end, n.start + n.len);
        } else {
            bool any = false;
            for (const auto &st : s.steps)
                any |= st.on;
            if (any)
                end = std::max(end, (int32_t)s.stepCount * kStepTicks);
        }
    }
    for (const auto &s : p.seqs)
        for (const auto &e : s.midi) end = std::max(end, e.tick + 1);
    const int32_t bar = ts.ticksPerBar();
    return std::max(bar, roundUpTo(end, bar));
}

int32_t Project::songLength() const
{
    int32_t end = 0;
    for (const auto &c : clips)
        end = std::max(end, c.start + c.len);
    return std::max(ts.ticksPerBar(), roundUpTo(end, ts.ticksPerBar()));
}

void Project::ensureMixer()
{
    while ((int)mixer.size() < kMaxMixerTracks) {
        MixerTrack t;
        t.index = (int)mixer.size();
        t.name = t.index == 0 ? "Master" : "Insert " + std::to_string(t.index);
        t.color = t.index == 0 ? 0xff9a3c : 0x8a8f98;
        mixer.push_back(std::move(t));
    }
    if ((int)mixer.size() > kMaxMixerTracks)
        mixer.resize(kMaxMixerTracks);
}

static float clampf(float v, float lo, float hi) { return v == v ? std::clamp(v, lo, hi) : lo; }

void Project::normalise()
{
    ensureMixer();
    bpm = std::clamp(bpm == bpm ? bpm : 120.0, kMinBpm, kMaxBpm);
    if (ts.num < 1 || ts.num > 16)
        ts.num = 4;
    if (ts.den != 2 && ts.den != 4 && ts.den != 8 && ts.den != 16)
        ts.den = 4;
    if (channels.size() > (size_t)kMaxChannels)
        channels.resize(kMaxChannels);
    if (patterns.size() > (size_t)kMaxPatterns)
        patterns.resize(kMaxPatterns);
    if (clips.size() > (size_t)kMaxClips)
        clips.resize(kMaxClips);
    if (tracks.size() > (size_t)kMaxPlaylistTracks)
        tracks.resize(kMaxPlaylistTracks);
    while (tracks.size() < 8) {
        PlaylistTrack t;
        t.name = "Track " + std::to_string(tracks.size() + 1);
        tracks.push_back(t);
    }
    Id maxId = 0;
    for (auto &c : channels) {
        maxId = std::max(maxId, c.id);
        c.vol = clampf(c.vol, 0, 1.25f);
        c.pan = clampf(c.pan, -1, 1);
        if (c.track < 0 || c.track >= kMaxMixerTracks)
            c.track = 0;
        if (c.root > 127)
            c.root = 60;
    }
    for (auto &p : patterns) {
        maxId = std::max(maxId, p.id);
        p.swing = clampf(p.swing, 0, 1);
        if (p.lengthTicks < 0)
            p.lengthTicks = 0;
        // drop sequences of channels that no longer exist
        p.seqs.erase(std::remove_if(p.seqs.begin(), p.seqs.end(), [&](const ChannelSeq &s) { return !channel(s.channel); }),
                     p.seqs.end());
        for (auto &s : p.seqs) {
            s.resizeSteps(s.stepCount);
            if (s.notes.size() > (size_t)kMaxNotesPerSeq)
                s.notes.resize(kMaxNotesPerSeq);
            Id maxNote = 0;
            for (auto &n : s.notes) {
                n.start = std::max<int32_t>(0, n.start);
                n.len = std::clamp<int32_t>(n.len, 1, kMaxSpanTicks);
                n.key = std::min<uint8_t>(n.key, 127);
                n.vel = clampf(n.vel, 0, 1);
                n.pan = clampf(n.pan, -1, 1);
                n.pitch = clampf(n.pitch, -1, 1);
                n.rel = clampf(n.rel, 0, 1);
                n.prob = clampf(n.prob, 0, 1);
                maxNote = std::max(maxNote, n.id);
            }
            if (s.nextNoteId <= maxNote)
                s.nextNoteId = maxNote + 1;
            s.sortNotes();
        }
    }
    for (auto &t : mixer) {
        t.vol = clampf(t.vol, 0, 1.25f);
        t.pan = clampf(t.pan, -1, 1);
        if (t.index == 0 || t.route < 0 || t.route >= kMaxMixerTracks || t.route == t.index)
            t.route = 0;
        if (t.fx.size() > (size_t)kMaxFxPerTrack)
            t.fx.resize(kMaxFxPerTrack);
        if (t.sends.size() > (size_t)kMaxSendsPerTrack)
            t.sends.resize(kMaxSendsPerTrack);
        for (auto &f : t.fx) {
            maxId = std::max(maxId, f.id);
            f.mix = clampf(f.mix, 0, 1);
        }
        for (auto &s : t.sends) {
            s.level = clampf(s.level, 0, 1.25f);
            if (s.target < 0 || s.target >= kMaxMixerTracks)
                s.target = 0;
        }
    }
    for (auto &c : clips) {
        maxId = std::max(maxId, c.id);
        c.start = std::max<int32_t>(0, c.start);
        c.len = std::max<int32_t>(1, c.len);
        c.offset = std::max<int32_t>(0, c.offset);
        c.track = std::clamp(c.track, 0, (int)tracks.size() - 1);
    }
    clips.erase(std::remove_if(clips.begin(), clips.end(),
                               [&](const Clip &c) {
                                   if (c.kind == ClipKind::Pattern)
                                       return pattern(c.ref) == nullptr;
                                   if (c.kind == ClipKind::Automation)
                                       return automationById(c.ref) == nullptr;
                                   return false;
                               }),
                clips.end());
    for (auto &a : automation) {
        maxId = std::max(maxId, a.id);
        if (a.points.size() > (size_t)kMaxAutoPoints)
            a.points.resize(kMaxAutoPoints);
        for (auto &pt : a.points) {
            pt.value = clampf(pt.value, 0, 1);
            pt.tension = clampf(pt.tension, -1, 1);
            pt.tick = std::max<int32_t>(0, pt.tick);
        }
        std::stable_sort(a.points.begin(), a.points.end(), [](const AutoPoint &x, const AutoPoint &y) { return x.tick < y.tick; });
    }
    for (auto &m : markers)
        maxId = std::max(maxId, m.id);
    if (loopEnd <= loopStart)
        loopOn = false;
    countInBars = std::clamp(countInBars, 0, 2);
    if (patterns.empty()) {
        Pattern p;
        p.id = ++maxId;
        p.name = "Pattern 1";
        patterns.push_back(p);
    }
    if (!pattern(currentPattern))
        currentPattern = patterns.front().id;
    if (nextId <= maxId)
        nextId = maxId + 1;
}

void stepsToNotes(const ChannelSeq &seq, uint8_t root, float swing, std::vector<Note> &out)
{
    out.clear();
    const int32_t swingTicks = (int32_t)lrintf(swing * kStepTicks * 0.5f);
    for (int i = 0; i < seq.stepCount && i < (int)seq.steps.size(); i++) {
        const Step &st = seq.steps[(size_t)i];
        if (!st.on)
            continue;
        Note n;
        n.id = 0x80000000u | (uint32_t)i;  // step notes: ids above the piano roll's range
        n.start = i * kStepTicks + ((i & 1) ? swingTicks : 0);
        n.len = kStepTicks;
        n.key = st.key ? st.key : root;
        n.vel = st.accent ? std::max(st.vel, 0.95f) : st.vel;
        n.pan = st.pan;
        n.prob = st.prob;
        n.accent = st.accent;
        out.push_back(n);
    }
}

uint32_t crc32(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

Project emptyProject()
{
    Project p;
    p.name = "Untitled";
    p.ensureMixer();
    p.normalise();
    return p;
}

Project defaultProject()
{
    Project p;
    p.name = "Untitled";
    p.bpm = 128.0;
    p.ensureMixer();
    struct Ch {
        const char *name, *plugin, *icon;
        uint32_t color;
        int mode;  // DrumLab single-piece mode, -1 = not DrumLab
        const char *preset;
    };
    const Ch chans[] = {
        {"Kick", "org.smhgzll.drumlab", "drum", 0xff6b4a, 1, nullptr},
        {"Clap", "org.smhgzll.drumlab", "drum", 0xffb03a, 7, nullptr},
        {"Hat", "org.smhgzll.drumlab", "drum", 0xf5d547, 5, nullptr},
        {"Open Hat", "org.smhgzll.drumlab", "drum", 0xc6e04a, 6, nullptr},
        {"Bass", "org.smhgzll.3xosc", "wave-saw", 0x4cd9a0, -1, "Fat Bass"},
        {"Lead", "org.smhgzll.3xosc", "wave-saw", 0x5fb3ff, -1, "Pluck"},
        {"Pad", "org.smhgzll.3xosc", "wave-saw", 0x9a7bff, -1, "Warm Pad"},
        {"Sine", "org.smhgzll.sine", "wave-sine", 0xff7ad9, -1, nullptr},
    };
    int track = 1;
    for (const auto &c : chans) {
        Channel ch;
        ch.id = p.newId();
        ch.name = c.name;
        ch.color = c.color;
        ch.icon = c.icon;
        ch.inst.pluginId = c.plugin;
        if (c.mode >= 0)
            ch.inst.setParam("mode", (float)c.mode);
        if (c.preset)
            ch.inst.preset = c.preset;  // applied by the plugin host on instantiation
        ch.track = track;
        p.mixer[(size_t)track].name = c.name;
        p.mixer[(size_t)track].color = c.color;
        track++;
        p.channels.push_back(ch);
    }
    // master: safety limiter; lead: delay
    {
        FxSlot lim;
        lim.id = p.newId();
        lim.inst.pluginId = "org.smhgzll.fx.limiter";
        p.mixer[0].fx.push_back(lim);
        FxSlot dly;
        dly.id = p.newId();
        dly.inst.pluginId = "org.smhgzll.fx.delay";
        dly.inst.setParam("mix", 25.0f);
        p.mixer[6].fx.push_back(dly);
    }
    Pattern beat;
    beat.id = p.newId();
    beat.name = "Beat";
    beat.color = 0xff6b4a;
    auto steps = [&](int ch, std::initializer_list<int> on, float vel = 0.78f) {
        ChannelSeq &s = beat.get(p.channels[(size_t)ch].id);
        for (int i : on) {
            s.steps[(size_t)i].on = true;
            s.steps[(size_t)i].vel = vel;
        }
    };
    steps(0, {0, 4, 8, 12}, 0.9f);
    steps(1, {4, 12});
    steps(2, {2, 6, 10, 14}, 0.6f);
    steps(3, {14}, 0.5f);
    Pattern bass;
    bass.id = p.newId();
    bass.name = "Bassline";
    bass.color = 0x4cd9a0;
    {
        ChannelSeq &s = bass.get(p.channels[4].id);
        s.pianoRoll = true;
        const int keys[] = {36, 36, 48, 36, 39, 36, 43, 41};
        for (int i = 0; i < 8; i++) {
            Note n;
            n.id = s.nextNoteId++;
            n.start = i * kPPQ / 2;
            n.len = kPPQ / 2 - kStepTicks / 2;
            n.key = (uint8_t)keys[i];
            n.vel = i % 2 ? 0.7f : 0.9f;
            s.notes.push_back(n);
        }
        // a slide into the last note
        s.notes[7].slide = true;
        s.notes[6].len = kPPQ / 2;
    }
    Pattern chords;
    chords.id = p.newId();
    chords.name = "Chords";
    chords.color = 0x9a7bff;
    {
        ChannelSeq &s = chords.get(p.channels[6].id);
        s.pianoRoll = true;
        const int prog[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 3; k++) {
                Note n;
                n.id = s.nextNoteId++;
                n.start = c * kPPQ * 4;
                n.len = kPPQ * 4 - kStepTicks;
                n.key = (uint8_t)prog[c][k];
                n.vel = 0.6f;
                s.notes.push_back(n);
            }
    }
    p.patterns = {beat, bass, chords};
    p.currentPattern = beat.id;
    for (int i = 0; i < 8; i++) {
        PlaylistTrack t;
        t.name = "Track " + std::to_string(i + 1);
        p.tracks.push_back(t);
    }
    const int32_t bar = p.ts.ticksPerBar();
    auto clip = [&](int track, Id ref, int32_t startBar, int32_t bars) {
        Clip c;
        c.id = p.newId();
        c.kind = ClipKind::Pattern;
        c.track = track;
        c.ref = ref;
        c.start = startBar * bar;
        c.len = bars * bar;
        p.clips.push_back(c);
    };
    clip(0, beat.id, 0, 8);
    clip(1, bass.id, 2, 6);
    clip(2, chords.id, 4, 4);
    p.normalise();
    return p;
}

}  // namespace daw
