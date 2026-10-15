// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/compile.hpp"

#include <math.h>
#include <string.h>

#include <algorithm>

#include "daw/platform.hpp"
#include "daw_plugin_api.h"

namespace daw {

namespace {

size_t align8(size_t n) { return (n + 7) & ~(size_t)7; }

struct Arena {
    uint8_t *base = nullptr;
    size_t used = 0, cap = 0;
    template <class T>
    T *take(size_t n)
    {
        T *p = reinterpret_cast<T *>(base + used);
        used += align8(sizeof(T) * n);
        return n ? p : nullptr;
    }
};

uint8_t probByte(float p) { return p >= 0.999f ? 255 : (uint8_t)lrintf(std::clamp(p, 0.0f, 1.0f) * 254.0f); }

}  // namespace

PlayModel *compilePlay(const Project &p, const SlotOf &slotOf, const ResolveTarget &resolve, uint32_t version)
{
    // per pattern / sequence: the notes that play (steps converted)
    struct SeqSrc {
        int slot;
        int32_t loopLen;
        std::vector<Note> notes;
        const std::vector<MidiEvent> *midi;
    };
    std::vector<std::vector<SeqSrc>> pats(p.patterns.size());
    size_t nSeq = 0, nNotes = 0, nMidi = 0, nMidiBytes = 0;
    uint32_t midiSlots = 0;
    for (size_t pi = 0; pi < p.patterns.size(); pi++) {
        const Pattern &pat = p.patterns[pi];
        for (const ChannelSeq &s : pat.seqs) {
            const int slot = slotOf(s.channel);
            if (slot < 0)
                continue;
            SeqSrc src;
            src.slot = slot;
            src.midi = &s.midi;
            if (s.pianoRoll) {
                src.loopLen = 0;
                src.notes = s.notes;
            } else {
                const Channel *ch = const_cast<Project &>(p).channel(s.channel);
                stepsToNotes(s, ch ? ch->root : 60, pat.swing, src.notes);
                src.loopLen = (int32_t)s.stepCount * kStepTicks;
            }
            if (src.notes.empty() && s.midi.empty())
                continue;
            std::stable_sort(src.notes.begin(), src.notes.end(), [](const Note &a, const Note &b) { return a.start < b.start; });
            nSeq++;
            nNotes += src.notes.size();
            nMidi += s.midi.size();
            if (!s.midi.empty() || std::any_of(src.notes.begin(), src.notes.end(), [](const Note &n) { return n.midiOnOrder != 0; })) midiSlots |= 1u << slot;
            for (const auto &e : s.midi) nMidiBytes += e.data.size();
            pats[pi].push_back(std::move(src));
        }
    }
    // clips that can sound: not muted, on a playlist track that is audible (mute / solo)
    bool anySolo = false;
    for (const auto &t : p.tracks)
        anySolo |= t.solo;
    std::vector<const Clip *> clips;
    for (const Clip &c : p.clips) {
        if (c.muted || c.track < 0 || c.track >= (int)p.tracks.size())
            continue;
        const PlaylistTrack &t = p.tracks[(size_t)c.track];
        if (t.mute || (anySolo && !t.solo))
            continue;
        if (c.kind == ClipKind::Audio)
            continue;  // audio clips: data model only (no sample playback engine yet)
        clips.push_back(&c);
    }
    std::stable_sort(clips.begin(), clips.end(), [](const Clip *a, const Clip *b) { return a->start < b->start; });
    size_t nPoints = 0;
    for (const auto &a : p.automation)
        nPoints += a.points.size();

    const size_t total = align8(sizeof(PlayModel)) + align8(sizeof(PlayPattern) * p.patterns.size()) + align8(sizeof(PlaySeq) * nSeq) +
                         align8(sizeof(PlayNote) * nNotes) + align8(sizeof(PlayClip) * clips.size()) +
                         align8(sizeof(PlayAuto) * p.automation.size()) + align8(sizeof(PlayPoint) * nPoints) +
                         align8(sizeof(PlayMidi) * nMidi) + align8(nMidiBytes) + 64;
    Arena ar;
    ar.base = static_cast<uint8_t *>(platform::alloc(total, 8, platform::Mem::Large));
    if (!ar.base)
        return nullptr;
    ar.cap = total;
    memset(ar.base, 0, total);
    PlayModel *m = ar.take<PlayModel>(1);
    m->version = version;
    m->arena = ar.base;
    PlayPattern *pp = ar.take<PlayPattern>(p.patterns.size());
    PlaySeq *seqs = ar.take<PlaySeq>(nSeq);
    PlayNote *notes = ar.take<PlayNote>(nNotes);
    PlayClip *pc = ar.take<PlayClip>(clips.size());
    PlayAuto *pa = ar.take<PlayAuto>(p.automation.size());
    PlayPoint *pts = ar.take<PlayPoint>(nPoints);

    PlayMidi *midis = ar.take<PlayMidi>(nMidi);
    uint8_t *midiData = ar.take<uint8_t>(nMidiBytes);
    size_t mi = 0, mbi = 0;
    m->midiSlots = midiSlots;
    size_t si = 0, ni = 0;
    for (size_t pi = 0; pi < p.patterns.size(); pi++) {
        PlayPattern &out = pp[pi];
        out.id = p.patterns[pi].id;
        out.length = p.patternLength(p.patterns[pi]);
        out.seqs = seqs ? seqs + si : nullptr;
        out.seqCount = (uint32_t)pats[pi].size();
        for (const SeqSrc &src : pats[pi]) {
            PlaySeq &q = seqs[si++];
            q.slot = (uint8_t)src.slot;
            q.loopLen = src.loopLen;
            q.count = (uint32_t)src.notes.size();
            q.notes = notes ? notes + ni : nullptr;
            q.midiCount = (uint32_t)src.midi->size();
            q.midi = midis ? midis + mi : nullptr;
            // Replaceable state can be skipped during a seek; bank / RPN / NRPN / data-entry /
            // reset messages keep their full order, because they affect later commands.
            uint32_t last[384];
            std::fill(std::begin(last), std::end(last), UINT32_MAX);
            for (size_t i = 0; i < src.midi->size(); ++i) {
                const auto &e = (*src.midi)[i];
                PlayMidi &x = midis[mi++];
                x = {e.tick, e.order, (uint32_t)e.data.size(), UINT32_MAX, midiData + mbi};
                memcpy(midiData + mbi, e.data.data(), e.data.size()); mbi += e.data.size();
                int key = -1;
                const auto st = e.data.empty() ? 0 : e.data[0] & 0xf0;
                if (st == 0xe0) key = 128;
                else if (st == 0xd0) key = 129;
                else if (st == 0xa0 && e.data.size() == 3) key = 130 + e.data[1];
                else if (st == 0xb0 && e.data.size() == 3) {
                    const int cc = e.data[1];
                    if (cc > 0 && cc != 6 && cc != 32 && cc != 38 && (cc < 96 || cc > 101) && cc < 120) key = cc;
                    if (cc == 121) std::fill(std::begin(last), std::end(last), UINT32_MAX);
                } else if (e.data[0] >= 0xf0) std::fill(std::begin(last), std::end(last), UINT32_MAX);
                if (key >= 0) {
                    if (last[key] != UINT32_MAX) midis[mi - 1 - i + last[key]].nextState = (uint32_t)i;
                    last[key] = (uint32_t)i;
                }
            }
            for (const Note &n : src.notes) {
                PlayNote &x = notes[ni++];
                x.start = n.start;
                x.len = n.len;
                x.id = n.id;
                x.key = n.key;
                x.flags = (uint8_t)((n.slide ? DAW_NOTE_FLAG_SLIDE : 0) | (n.accent ? DAW_NOTE_FLAG_ACCENT : 0));
                x.prob = probByte(n.prob);
                x.vel = n.vel;
                x.pan = n.pan;
                x.pitch = n.pitch;
                x.rel = n.rel;
                x.midiOnOrder = n.midiOnOrder; x.midiOffOrder = n.midiOffOrder;
            }
        }
    }
    m->patterns = pp;
    m->patternCount = (uint32_t)p.patterns.size();

    for (size_t i = 0; i < clips.size(); i++) {
        const Clip &c = *clips[i];
        PlayClip &x = pc[i];
        x.start = c.start;
        x.end = c.start + c.len;
        x.offset = c.offset;
        x.kind = (uint8_t)c.kind;
        x.track = (uint8_t)std::min(c.track, 255);
        int idx = -1;
        if (c.kind == ClipKind::Pattern)
            idx = p.patternIndex(c.ref);
        else
            for (size_t a = 0; a < p.automation.size(); a++)
                if (p.automation[a].id == c.ref)
                    idx = (int)a;
        x.index = idx < 0 ? 0xFFFF : (uint16_t)idx;
    }
    m->clips = pc;
    m->clipCount = (uint32_t)clips.size();

    size_t pti = 0;
    for (size_t i = 0; i < p.automation.size(); i++) {
        const Automation &a = p.automation[i];
        PlayAuto &x = pa[i];
        x.kind = AT_NONE;
        if (!resolve(a.target, x))
            x.kind = AT_NONE;
        x.points = pts ? pts + pti : nullptr;
        x.pointCount = (uint32_t)a.points.size();
        for (const AutoPoint &pt : a.points)
            pts[pti++] = PlayPoint{pt.tick, pt.value, pt.tension};
        if (x.kind == AT_NONE)
            x.pointCount = 0;
    }
    m->autos = pa;
    m->autoCount = (uint32_t)p.automation.size();
    m->songLength = p.songLength();
    return m;
}

void freePlay(const PlayModel *m)
{
    if (m)
        platform::free(m->arena);
}

MixModel *compileMix(const Project &p, std::string *err)
{
    auto *m = static_cast<MixModel *>(platform::alloc(sizeof(MixModel), 8, platform::Mem::Large));
    if (!m)
        return nullptr;
    memset(m, 0, sizeof *m);
    // edges: insert i -> its route and its send targets (all must come later in the order)
    bool edge[kEngineTracks][kEngineTracks] = {};
    for (int i = 0; i < kEngineTracks; i++) {
        m->route[i] = -1;
        for (int s = 0; s < kMaxSendsPerTrack; s++)
            m->sendTarget[i][s] = -1;
    }
    for (const MixerTrack &t : p.mixer) {
        if (t.index <= 0 || t.index >= kEngineTracks)
            continue;
        const int r = (t.route > 0 && t.route < kEngineTracks && t.route != t.index) ? t.route : 0;
        m->route[t.index] = (int8_t)r;
        if (r > 0)
            edge[t.index][r] = true;
        for (size_t s = 0; s < t.sends.size() && s < (size_t)kMaxSendsPerTrack; s++) {
            const int tg = t.sends[s].target;
            if (tg < 0 || tg >= kEngineTracks || tg == t.index)
                continue;
            m->sendTarget[t.index][s] = (int8_t)tg;
            if (tg > 0)
                edge[t.index][tg] = true;
        }
    }
    // Kahn's algorithm over the inserts
    int indeg[kEngineTracks] = {};
    for (int a = 1; a < kEngineTracks; a++)
        for (int b = 1; b < kEngineTracks; b++)
            indeg[b] += edge[a][b];
    bool done[kEngineTracks] = {};
    uint8_t n = 0;
    for (int round = 1; round < kEngineTracks; round++) {
        int pick = -1;
        for (int i = 1; i < kEngineTracks; i++)
            if (!done[i] && indeg[i] == 0) {
                pick = i;
                break;
            }
        if (pick < 0) {
            if (err)
                *err = "mixer routing has a cycle";
            platform::free(m);
            return nullptr;
        }
        done[pick] = true;
        m->order[n++] = (uint8_t)pick;
        for (int b = 1; b < kEngineTracks; b++)
            if (edge[pick][b])
                indeg[b]--;
    }
    m->order[n++] = 0;
    m->count = n;
    return m;
}

void freeMix(const MixModel *m) { platform::free((void *)m); }

}  // namespace daw
