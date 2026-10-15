// SPDX-License-Identifier: GPL-3.0-or-later
// Real-time engine (see engine.hpp). Everything in this file that runs from render() is bounded,
// allocation-free; the optional platform worker uses task notifications to start/join.
#include "daw/engine.hpp"

#include <math.h>
#include <string.h>

#include <new>

#include "daw/plugin_host.hpp"

namespace daw {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// sin(2 pi x), x in turns; degree-9 polynomial after folding (|err| < 4e-6)
inline float sinTurns(float x)
{
    x -= (float)(int32_t)x;
    if (x < 0)
        x += 1.0f;
    if (x > 0.75f)
        x -= 1.0f;
    else if (x > 0.25f)
        x = 0.5f - x;
    float t = x * kTwoPi, t2 = t * t;
    return t * (1.0f + t2 * (-0.16666667f + t2 * (0.0083333310f + t2 * (-0.00019840874f + t2 * 2.7525562e-6f))));
}

// sample offset of an event `rel` samples after a segment starting at `segPos` (fractional)
inline uint32_t offsetAt(double segPos, double rel)
{
    double x = segPos + (rel > 0 ? rel : 0.0);
    return (uint32_t)floor(x + 1e-6);
}

inline int32_t ceilTick(double t)
{
    double c = ceil(t - 1e-9);
    return (int32_t)c;
}

inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline void balance(float pan, float &l, float &r)
{
    pan = pan < -1 ? -1 : (pan > 1 ? 1 : pan);
    l = pan > 0 ? 1.0f - pan : 1.0f;
    r = pan < 0 ? 1.0f + pan : 1.0f;
}

// every sample finite: NaN / Inf have an all-ones exponent. An integer maximum in four
// independent lanes instead of a dependent float sum (runs on every plugin output, every block).
// the float bits as integers: a plain load (memcpy is an out-of-line call in the ESP-IDF build)
typedef uint32_t __attribute__((may_alias)) FloatBits;
inline bool finiteSum(const float *x, uint32_t n)
{
    const FloatBits *u = reinterpret_cast<const FloatBits *>(x);
    uint32_t m0 = 0, m1 = 0, m2 = 0, m3 = 0, i = 0;
    for (; i + 4 <= n; i += 4) {
        const uint32_t a = u[i] & 0x7f800000u, b = u[i + 1] & 0x7f800000u;
        const uint32_t c = u[i + 2] & 0x7f800000u, d = u[i + 3] & 0x7f800000u;
        m0 = a > m0 ? a : m0;
        m1 = b > m1 ? b : m1;
        m2 = c > m2 ? c : m2;
        m3 = d > m3 ? d : m3;
    }
    for (; i < n; i++) {
        const uint32_t a = u[i] & 0x7f800000u;
        m0 = a > m0 ? a : m0;
    }
    m0 = m1 > m0 ? m1 : m0;
    m2 = m3 > m2 ? m3 : m2;
    return (m2 > m0 ? m2 : m0) != 0x7f800000u;
}

inline bool hasSignal(const float *l, const float *r, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (l[i] != 0.0f || r[i] != 0.0f) return true;
    return false;
}

// Preserve the sample-by-sample gain state while a track is silent. Once float rounding
// prevents another change, the remaining iterations cannot change it either.
inline float advanceSilentGain(float value, float target, float k, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        const float next = value + (target - value) * k;
        if (next == value)
            break;
        value = next;
    }
    return value;
}

// between polls of a flag the other core sets: keeps a waiting core off the shared internal-RAM
// bus the working core needs (both cores' buffers and the flags live there)
inline void spinPause()
{
#if defined(__XTENSA__)
    for (int i = 0; i < 96; ++i)
        __asm__ __volatile__("nop");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

}  // namespace

Engine::Engine(const Config &cfg) : cfg_(cfg)
{
    if (cfg_.blockFrames == 0 || cfg_.blockFrames > kEngineMaxBlock)
        cfg_.blockFrames = kEngineMaxBlock;
    bufs_ = static_cast<Buffers *>(platform::alloc(sizeof(Buffers), 16, platform::Mem::Fast));
    if (bufs_)
        memset(bufs_, 0, sizeof(Buffers));
    if (cfg_.worker) {
        // one output in internal RAM (also the effect scratch), the others in PSRAM
        workerOut_ = static_cast<float (*)[2][kEngineMaxBlock]>(platform::alloc(sizeof(*workerOut_), 16, platform::Mem::Fast));
        workerOutLarge_ = static_cast<float (*)[2][kEngineMaxBlock]>(
            platform::alloc((kWorkerBufs - 1) * sizeof(*workerOutLarge_), 16, platform::Mem::Large));
        if (void *p = platform::alloc(sizeof(Parallel), alignof(Parallel), platform::Mem::Fast))
            par_ = new (p) Parallel();
        if (!workerOut_ || !workerOutLarge_ || !par_) {  // retain the serial renderer on allocation failure
            cfg_.worker = nullptr;
            platform::free(workerOut_);
            platform::free(workerOutLarge_);
            if (par_) {
                par_->~Parallel();
                platform::free(par_);
            }
            workerOut_ = workerOutLarge_ = nullptr;
            par_ = nullptr;
        }
    }
    eventBufs_ = static_cast<EventBuffers *>(platform::alloc(sizeof(EventBuffers), 16, platform::Mem::Large));
    for (auto &a : active_)
        a.used = false;
    for (auto &v : autoLast_)
        v = -1.0f;
    st_.bpm.store((float)bpm_);
    st_.budgetUs.store(cfg_.blockFrames * 1000000u / cfg_.sampleRate);
    clickInc_ = 1500.0f / (float)cfg_.sampleRate;
}

Engine::~Engine()
{
    // only after the audio thread stopped: apply what is still queued, then free everything the
    // engine holds (instances, snapshots) and everything waiting in the retire queue
    auto destroyRetired = [](const Retired &r) {
        switch (r.kind) {
        case Retired::Instance: PluginInstance::destroy(static_cast<PluginInstance *>(r.p)); break;
        case Retired::Play: platform::free(static_cast<const PlayModel *>(r.p)->arena); break;
        case Retired::Mix: platform::free(r.p); break;
        case Retired::Chain: delete static_cast<FxChainMsg *>(r.p); break;
        }
    };
    Retired r;
    if (ok()) {
        EngineCmd c;
        while (cmds_.pop(c)) {
            handle(c);
            while (retired_.pop(r))
                destroyRetired(r);
        }
    }
    while (retired_.pop(r))
        destroyRetired(r);
    for (auto &s : slots_) {
        PluginInstance::destroy(s.inst);
        if (s.pending != kRemoveMarker())
            PluginInstance::destroy(s.pending);
        s.inst = s.pending = nullptr;
    }
    for (auto &t : tracks_)
        for (auto &f : t.fx) {
            PluginInstance::destroy(f.inst);
            f.inst = nullptr;
        }
    if (play_)
        platform::free(play_->arena);
    if (mix_)
        platform::free((void *)mix_);
    platform::free(bufs_);
    platform::free(workerOut_);
    platform::free(workerOutLarge_);
    if (par_) {
        par_->~Parallel();
        platform::free(par_);
    }
    platform::free(eventBufs_);
}

bool Engine::post(const EngineCmd &c)
{
    if (cmds_.push(c))
        return true;
    st_.cmdOverflow.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Engine::retire(Retired::Kind k, void *p)
{
    if (!p)
        return;
    // the retire queue is drained by the control thread every 20 ms; if it is ever full the object
    // is leaked (counted) rather than freed here: never free on the audio thread
    if (!retired_.push(Retired{k, p}))
        st_.retireOverflow.fetch_add(1, std::memory_order_relaxed);
}

void Engine::addEvent(uint8_t target, uint32_t offset, const daw_event_t &e, uint8_t prio, uint32_t order)
{
    // the CPU guard: no new voices while the render is near its deadline (slides only retune a
    // sounding voice; note-offs and everything else always pass)
    if (e.type == DAW_EV_NOTE_ON && guardTripped_ && !(e.flags & DAW_NOTE_FLAG_SLIDE)) {
        st_.cpuGuardDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (nEvents_ >= kBlockEvents) {
        st_.eventDrops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Event &x = eventBufs_->events[nEvents_++];
    x.ev = e;
    if (target < kEngineSlots && slots_[target].inst &&
        (slots_[target].inst->desc()->api_minor < 4 || !(slots_[target].inst->desc()->capabilities & DAW_CAP_MIDI_STREAM))) {
        if (e.type >= DAW_EV_POLY_PRESSURE) { --nEvents_; return; }
        x.ev.flags &= ~DAW_NOTE_FLAG_MIDI;
    }
    x.ev.offset = offset;
    x.target = target;
    x.prio = prio;
    x.order = order ? order : UINT32_MAX;
}

// ---- commands -----------------------------------------------------------------------------

void Engine::handle(const EngineCmd &c)
{
    switch (c.type) {
    case EngineCmd::Play:
        if (paused_) {
            paused_ = false;
            playing_ = true;
            chase_ = true; midiChaseActive_ = false;
            break;
        }
        if (playing_)
            break;
        playing_ = true;
        chase_ = true; midiChaseActive_ = false;
        run_ = 0;
        loopCount_ = 0;
        lastAutoTick_ = -1;
        for (auto &v : autoLast_)
            v = -1.0f;
        countIn_ = countInBars_ > 0 ? (double)countInBars_ * ts_.ticksPerBar() : 0;
        break;
    case EngineCmd::Stop:
        releaseAll(false);
        playing_ = paused_ = false;
        chase_ = false; midiChaseActive_ = false;
        countIn_ = 0;
        tick_ = (mode_ == PlayMode::Song && loopOn_) ? loopStart_ : 0;
        queued_ = -1;
        break;
    case EngineCmd::Pause:
        if (playing_) {
            releaseAll(false);
            playing_ = false;
            paused_ = true;
            countIn_ = 0;
        }
        break;
    case EngineCmd::Continue:
        if (paused_) {
            paused_ = false;
            playing_ = true;
            chase_ = true; midiChaseActive_ = false;
        }
        break;
    case EngineCmd::Seek:
        if (playing_)
            releaseAll(false);
        tick_ = c.i[0] < 0 ? 0 : c.i[0];
        chase_ = true; midiChaseActive_ = false;
        lastAutoTick_ = -1;
        for (auto &v : autoLast_)
            v = -1.0f;
        break;
    case EngineCmd::SetMode:
        if ((PlayMode)c.a != mode_) {
            releaseAll(false);
            mode_ = (PlayMode)c.a;
            tick_ = 0;
            queued_ = -1;
            chase_ = true; midiChaseActive_ = false;
        }
        break;
    case EngineCmd::SetPattern:
        pattern_ = c.u32;
        queued_ = -1;
        break;
    case EngineCmd::QueuePattern:
        if (playing_ && mode_ == PlayMode::Pattern && c.u32 != pattern_)
            queued_ = (int32_t)c.u32;
        else {
            pattern_ = c.u32;
            queued_ = -1;
        }
        break;
    case EngineCmd::SetTempo: {
        double b = c.d;
        bpm_ = b < kMinBpm ? kMinBpm : (b > kMaxBpm ? kMaxBpm : b);
        break;
    }
    case EngineCmd::SetTimeSig:
        ts_.num = c.a ? c.a : 4;
        ts_.den = c.b ? c.b : 4;
        break;
    case EngineCmd::SetLoop:
        loopOn_ = c.a && c.i[1] > c.i[0];
        loopStart_ = c.i[0];
        loopEnd_ = c.i[1];
        break;
    case EngineCmd::SetMetronome:
        metro_ = c.a != 0;
        metroVol_ = c.f[0];
        break;
    case EngineCmd::SetCountIn: countInBars_ = c.a; break;
    case EngineCmd::SwapPlay: {
        const PlayModel *old = play_;
        play_ = static_cast<const PlayModel *>(c.p);
        midiChaseActive_ = false;
        retire(Retired::Play, (void *)old);
        break;
    }
    case EngineCmd::SwapMix: {
        const MixModel *old = mix_;
        mix_ = static_cast<const MixModel *>(c.p);
        retire(Retired::Mix, (void *)old);
        break;
    }
    case EngineCmd::SetInstrument: {
        Slot &s = slots_[c.a % kEngineSlots];
        auto *inst = static_cast<PluginInstance *>(c.p);
        if (s.pending) {  // a replacement still waiting for the fade: superseded
            if (s.pending != kRemoveMarker())
                retire(Retired::Instance, s.pending);
            s.pending = nullptr;
        }
        const bool sounding = s.inst && s.inst->desc()->active_voices && s.inst->desc()->active_voices(s.inst->handle()) > 0;
        if (sounding && inst) {  // a replacement: crossfade, events go to the new one at once
            s.pending = inst;
            s.xf = 0;
            s.fadeOut = false;
        } else if (sounding) {  // removal of a sounding instrument: fade the channel out, then drop it
            s.pending = kRemoveMarker();
            s.fadeOut = true;
        } else {  // nothing to hear: swap now
            retire(Retired::Instance, s.inst);
            s.inst = inst;
            s.fadeOut = false;
        }
        break;
    }
    case EngineCmd::SetFx: {
        FxUnit &u = tracks_[c.a % kEngineTracks].fx[c.b % kEngineFx];
        retire(Retired::Instance, u.inst);
        u.inst = static_cast<PluginInstance *>(c.p);
        u.curMix = 0;  // fades in
        u.failed = false;
        break;
    }
    case EngineCmd::Param: {
        daw_event_t e{};
        e.type = DAW_EV_PARAM;
        e.u.param.id = c.u32;
        e.u.param.value = c.f[0];
        uint8_t target = c.a == 0 ? (uint8_t)(c.b % kEngineSlots)
                                  : (uint8_t)(kFxTarget0 + (c.b % kEngineTracks) * kEngineFx + (c.c % kEngineFx));
        addEvent(target, 0, e, 3);
        break;
    }
    case EngineCmd::ChannelLevel: {
        Slot &s = slots_[c.a % kEngineSlots];
        s.vol = c.f[0];
        s.pan = c.f[1];
        s.mute = c.b != 0;
        s.track = c.c % kEngineTracks;
        break;
    }
    case EngineCmd::TrackLevel: {
        Track &t = tracks_[c.a % kEngineTracks];
        t.vol = c.f[0];
        t.pan = c.f[1];
        t.mute = c.b != 0;
        break;
    }
    case EngineCmd::SendLevel: tracks_[c.a % kEngineTracks].send[c.b % kMaxSendsPerTrack] = c.f[0]; break;
    case EngineCmd::FxState: {
        FxUnit &u = tracks_[c.a % kEngineTracks].fx[c.b % kEngineFx];
        u.enabled = c.c != 0;
        u.mix = c.f[0];
        break;
    }
    case EngineCmd::Note: {
        daw_event_t e{};
        e.type = c.c ? DAW_EV_NOTE_ON : DAW_EV_NOTE_OFF;
        e.flags = c.c ? DAW_NOTE_FLAG_PREVIEW : 0;
        e.u.note.note_id = (int32_t)c.u32;
        e.u.note.key = c.b & 127;
        e.u.note.velocity = c.f[0];
        // live input keeps its arrival order (one priority for on, off and controllers): a burst of
        // on/off pairs inside one block must not become "all offs, then all ons"
        addEvent(c.a % kEngineSlots, 0, e, kLivePrio);
        // MIDI recording: what was played, where in the pattern (pattern mode, playing, not counting in)
        if (record_ && playing_ && !paused_ && countIn_ <= 0 && mode_ == PlayMode::Pattern) {
            RecEvent r{(int32_t)tick_, pattern_, (uint8_t)(c.a % kEngineSlots), (uint8_t)(c.b & 127), c.c != 0, c.f[0]};
            if (!recorded_.push(r))
                st_.eventDrops.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    case EngineCmd::SetRecord: record_ = c.a != 0; break;
    case EngineCmd::Fence: pendingFence_ = c.u32; break;
    case EngineCmd::ResetLoad:
        // Project replacement starts a new measurement window. Otherwise its
        // empty engine can reject new instruments against the previous song's
        // one-second average, or trip the guard with the previous smoothed load.
        statCycles_ = statBlocks_ = statFrames_ = statMaxUs_ = 0;
        statWaitUs_ = 0;
        workerBusyUs_.store(0, std::memory_order_relaxed);
        load_ = 0;
        guardTripped_ = false;
        st_.cpuPermille.store(0, std::memory_order_relaxed);
        st_.loadPermille.store(0, std::memory_order_relaxed);
        st_.renderUsAvg.store(0, std::memory_order_relaxed);
        st_.workerWaitPermille.store(0, std::memory_order_relaxed);
        st_.workerBusyPermille.store(0, std::memory_order_relaxed);
        // Lifetime error counters and the worst render remain diagnostic evidence.
        break;
    case EngineCmd::NotesOff: releaseAll(c.a != 0); break;
    case EngineCmd::Diag:
        diag_ = (Diag)c.a;
        diagPos_ = 0;
        diagPhase_ = 0;
        break;
    case EngineCmd::MasterSafety: masterSafety_ = c.a != 0; break;
    case EngineCmd::SetFxChain: {
        Track &t = tracks_[c.a % kEngineTracks];
        auto *m = static_cast<FxChainMsg *>(c.p);
        FxUnit old[kEngineFx];
        for (int i = 0; i < kEngineFx; i++)
            old[i] = t.fx[i];
        for (int i = 0; i < kEngineFx; i++) {
            FxUnit u;
            u.inst = m->fx[i];
            u.enabled = m->enabled[i];
            u.mix = m->mix[i];
            u.curMix = 0;
            for (int k = 0; k < kEngineFx; k++)  // an instance that moves keeps its fade state
                if (u.inst && old[k].inst == u.inst) {
                    u.curMix = old[k].curMix;
                    u.failed = old[k].failed;
                }
            t.fx[i] = u;
        }
        for (int k = 0; k < kEngineFx; k++) {
            bool kept = false;
            for (int i = 0; i < kEngineFx; i++)
                kept |= old[k].inst && t.fx[i].inst == old[k].inst;
            if (old[k].inst && !kept)
                retire(Retired::Instance, old[k].inst);
        }
        retire(Retired::Chain, m);
        break;
    }
    case EngineCmd::RawEvent: {
        daw_event_t e{};
        e.type = c.b;
        e.u.midi.channel = (uint8_t)c.u32;
        e.u.midi.number = c.c;
        e.u.midi.index = c.c;
        e.u.midi.value = c.f[0];
        addEvent(c.a % kEngineSlots, 0, e, kLivePrio);
        break;
    }
    default: break;
    }
}

void Engine::releaseAll(bool hard)
{
    daw_event_t e{};
    for (auto &a : active_) {
        if (!a.used)
            continue;
        e.type = DAW_EV_NOTE_OFF;
        e.u.note.note_id = a.voiceId;
        e.u.note.key = a.key;
        e.u.note.velocity = a.rel;
        addEvent(a.slot, 0, e, 1);
        a.used = false;
    }
    if (play_ && play_->midiSlots) {
        daw_event_t pedal{}; pedal.type = DAW_EV_SUSTAIN; pedal.flags = DAW_NOTE_FLAG_MIDI;
        for (int s = 0; s < kEngineSlots; ++s) if (play_->midiSlots & (1u << s)) addEvent((uint8_t)s, 0, pedal, 1);
    }
    daw_event_t all{};
    all.type = hard ? DAW_EV_ALL_SOUND_OFF : DAW_EV_ALL_NOTES_OFF;
    for (int s = 0; s < kEngineSlots; s++)
        if (slots_[s].inst)
            addEvent((uint8_t)s, 0, all, 1);
}

// ---- sequencing ---------------------------------------------------------------------------

int32_t Engine::regionStart() const
{
    if (mode_ == PlayMode::Song)
        return loopOn_ ? loopStart_ : 0;
    return 0;
}

int32_t Engine::regionEnd() const
{
    if (mode_ == PlayMode::Song) {
        if (loopOn_)
            return loopEnd_;
        return play_ ? play_->songLength : ts_.ticksPerBar();
    }
    if (play_ && pattern_ < play_->patternCount)
        return play_->patterns[pattern_].length;
    return ts_.ticksPerBar();
}

void Engine::noteOn(uint8_t slot, uint32_t offset, const PlayNote &n, double onRun, int32_t lenTicks, uint32_t chaseMbeats)
{
    if (!slots_[slot].inst)
        return;
    ActiveNote *a = nullptr;
    for (auto &x : active_)
        if (!x.used) {
            a = &x;
            break;
        }
    if (!a) {  // table full: end the note that would end first
        a = &active_[0];
        for (auto &x : active_)
            if (x.offRun < a->offRun)
                a = &x;
        daw_event_t off{};
        off.type = DAW_EV_NOTE_OFF;
        off.u.note.note_id = a->voiceId;
        off.u.note.key = a->key;
        addEvent(a->slot, offset, off, 1);
        st_.noteDrops.fetch_add(1, std::memory_order_relaxed);
    }
    const int32_t id = nextVoiceId_;
    nextVoiceId_ = nextVoiceId_ == 0x7FFFFFFF ? 1 : nextVoiceId_ + 1;
    daw_event_t e{};
    e.type = DAW_EV_NOTE_ON;
    e.flags = n.flags;
    e.u.note.note_id = id;
    e.u.note.key = n.key;
    e.u.note.velocity = n.vel;
    e.u.note.pan = n.pan;
    e.u.note.pitch = n.pitch;
    if (chaseMbeats != 0xFFFFFFFFu) {  // a chased note never slides
        e.flags = (uint16_t)((n.flags & ~DAW_NOTE_FLAG_SLIDE) | DAW_NOTE_FLAG_CHASE);
        e.u.note.chase_mbeats = chaseMbeats;
    }
    const bool slide = (e.flags & DAW_NOTE_FLAG_SLIDE) != 0;
    if (n.midiOnOrder) e.flags |= DAW_NOTE_FLAG_MIDI;
    addEvent(slot, offset, e, n.midiOnOrder ? 2 : slide ? 0 : 2, n.midiOnOrder);
    a->used = true;
    a->offRun = onRun + (lenTicks < 1 ? 1 : lenTicks);
    a->voiceId = id;
    a->slot = slot;
    a->key = n.key;
    a->rel = n.rel;
    a->midiOffOrder = n.midiOffOrder;
}

void Engine::emitPattern(const PlayPattern &p, int32_t lo, int32_t hi, double segT0, double tps, double segPos,
                         double base, int32_t endLocal, uint32_t seed)
{
    // `base`: song tick of this repetition's local tick 0 (minus nothing in pattern mode)
    for (uint32_t s = 0; s < p.seqCount; s++) {
        const PlaySeq &q = p.seqs[s];
        const bool poly = q.loopLen > 0 && q.loopLen < p.length;
        const int32_t L = poly ? q.loopLen : p.length;
        if (L <= 0) continue;
        if (q.midiCount) {
            const int32_t midiLen = p.length;
            for (int32_t k = lo / midiLen; k <= (hi - 1) / midiLen; ++k) {
                const int32_t b = k * midiLen;
                uint32_t a = 0, z = q.midiCount;
                while (a < z) { const uint32_t m = (a + z) / 2; if (q.midi[m].tick < lo - b) a = m + 1; else z = m; }
                for (; a < q.midiCount && b + q.midi[a].tick < hi; ++a) {
                    const double at = base + b + q.midi[a].tick;
                    emitMidi(q.slot, q.midi[a], offsetAt(segPos, (at - segT0) / tps));
                }
            }
        }
        if (q.count == 0) continue;
        const int32_t k0 = lo / L, k1 = (hi - 1) / L;
        for (int32_t k = k0; k <= k1; k++) {
            const int32_t b = k * L;
            const int32_t a2 = (lo > b ? lo : b) - b;
            const int32_t b2 = (hi < b + L ? hi : b + L) - b;
            // first note with start >= a2
            uint32_t i0 = 0, i1 = q.count;
            while (i0 < i1) {
                uint32_t m = (i0 + i1) >> 1;
                if (q.notes[m].start < a2)
                    i0 = m + 1;
                else
                    i1 = m;
            }
            for (uint32_t i = i0; i < q.count && q.notes[i].start < b2; i++) {
                const PlayNote &n = q.notes[i];
                const int32_t local = b + n.start;
                if (local >= p.length)
                    break;
                if (n.prob < 255 && (hash32(n.id * 2654435761u ^ (seed * 40503u + (uint32_t)k)) & 255u) >= n.prob)
                    continue;
                int32_t len = n.len;
                if (local + len > endLocal)
                    len = endLocal - local;
                if (len <= 0)
                    continue;
                const double segTick = base + local;
                const uint32_t off = offsetAt(segPos, (segTick - segT0) / tps);
                noteOn(q.slot, off, n, run_ + (segTick - segT0), len);  // run_ == run tick at segT0
            }
        }
    }
}

void Engine::emitMidi(uint8_t slot, const PlayMidi &m, uint32_t offset, bool replay)
{
    if (!m.size || !slots_[slot].inst) return;
    const uint8_t st = m.data[0], hi = st & 0xf0;
    daw_event_t e{}; e.flags = DAW_NOTE_FLAG_MIDI;
    e.u.midi.channel = st & 15;
    e.u.midi.number = m.size > 1 ? m.data[1] : 0;
    e.u.midi.value = (m.size > 2 ? m.data[2] : m.size > 1 ? m.data[1] : 0) / 127.0f;
    switch (hi) {
    case 0xa0: e.type = DAW_EV_POLY_PRESSURE; break;
    case 0xb0:
        e.type = DAW_EV_CC;
        if (m.data[1] == 64) e.type = DAW_EV_SUSTAIN;
        else if (m.data[1] == 120) e.type = DAW_EV_ALL_SOUND_OFF;
        else if (m.data[1] == 123 || m.data[1] >= 124) e.type = DAW_EV_ALL_NOTES_OFF;
        break;
    case 0xc0: e.type = DAW_EV_PROGRAM; e.u.midi.index = m.data[1]; break;
    case 0xd0: e.type = DAW_EV_CHANNEL_PRESSURE; break;
    case 0xe0:
        e.type = DAW_EV_PITCH_BEND;
        e.u.midi.value = (float)(((int)m.data[2] << 7 | m.data[1]) - 8192) / 8192.0f;
        break;
    default:
        // Meta data has a separate FF + type payload, never confuse it with System Reset FF.
        if (st == 0xff && m.size > 1) return;
        e.type = DAW_EV_MIDI_BYTES; e.u.bytes.data = m.data; e.u.bytes.size = m.size; break;
    }
    // Seek replay finishes before any new note; retain its chronological traversal order.
    addEvent(slot, offset, e, replay ? 1 : 2, replay ? 0 : m.order);
}

bool Engine::chaseMidi(int32_t t, uint32_t offset)
{
    if (!midiChaseActive_) {
        midiChaseActive_ = true;
        midiChaseClip_ = midiChaseSeq_ = midiChaseEvent_ = 0;
        daw_event_t e{}; e.type = DAW_EV_MIDI_RESET; e.flags = DAW_NOTE_FLAG_MIDI;
        for (int s = 0; s < kEngineSlots; ++s) if (play_->midiSlots & (1u << s)) addEvent((uint8_t)s, offset, e, 1);
    }
    // At most 128 scanned / 64 delivered messages in a block, even for huge bank dumps.
    // Clock is held during this brief controller pre-roll; it cannot overflow the event pool.
    uint32_t scanned = 0, delivered = 0;
    const uint32_t n = mode_ == PlayMode::Pattern ? 1 : play_->clipCount;
    while (midiChaseClip_ < n) {
        const PlayClip *c = mode_ == PlayMode::Pattern ? nullptr : &play_->clips[midiChaseClip_];
        const uint32_t pi = c ? c->index : pattern_;
        if ((c && (c->kind != (uint8_t)ClipKind::Pattern || c->start >= t)) || pi >= play_->patternCount) {
            ++midiChaseClip_; midiChaseSeq_ = midiChaseEvent_ = 0; continue;
        }
        const PlayPattern &p = play_->patterns[pi];
        if (p.length <= 0) { ++midiChaseClip_; midiChaseSeq_ = midiChaseEvent_ = 0; continue; }
        const int32_t end = c ? std::min(t, c->end) - c->start + c->offset : t;
        // Only the latest repetition contributes controller state. Imported lanes use one pass.
        const int32_t rep = end > 0 ? (end - 1) / p.length : 0;
        const int32_t local = end - rep * p.length;
        const int32_t min = c ? std::max<int32_t>(0, c->offset - rep * p.length) : 0;
        while (midiChaseSeq_ < p.seqCount) {
            const PlaySeq &q = p.seqs[midiChaseSeq_];
            while (midiChaseEvent_ < q.midiCount) {
                const PlayMidi &e = q.midi[midiChaseEvent_++];
                if (++scanned > 128 || delivered >= 64 || nEvents_ >= kBlockEvents - 32) { --midiChaseEvent_; return false; }
                if (e.tick >= local) { midiChaseEvent_ = q.midiCount; break; }
                if (e.tick < min) continue;
                if (e.nextState < q.midiCount && q.midi[e.nextState].tick < local) continue;
                emitMidi(q.slot, e, offset, true); ++delivered;
            }
            ++midiChaseSeq_; midiChaseEvent_ = 0;
        }
        ++midiChaseClip_; midiChaseSeq_ = midiChaseEvent_ = 0;
    }
    return true;
}

bool Engine::chaseSlot(uint8_t slot) const
{
    const PluginInstance *in = slots_[slot % kEngineSlots].inst;
    return in && (in->desc()->capabilities & DAW_CAP_NOTE_CHASE);
}

void Engine::chase(int32_t t, double segPos)
{
    bool any = false;
    for (int s = 0; s < kEngineSlots && !any; s++)
        any = chaseSlot((uint8_t)s);
    if (!any || !play_)
        return;
    const uint32_t off = offsetAt(segPos, 0);
    // what these slots still hold ends here; the chase starts what sounds at t again
    daw_event_t e{};
    e.type = DAW_EV_NOTE_OFF;
    for (auto &a : active_) {
        if (!a.used || !chaseSlot(a.slot))
            continue;
        e.u.note.note_id = a.voiceId;
        e.u.note.key = a.key;
        e.u.note.velocity = a.rel;
        addEvent(a.slot, off, e, 1);
        a.used = false;
    }
    if (mode_ == PlayMode::Pattern) {
        if (pattern_ < play_->patternCount)
            chasePattern(play_->patterns[pattern_], t, 0, 0x7FFFFFFF, INT32_MIN, loopCount_, off);
        return;
    }
    for (uint32_t ci = 0; ci < play_->clipCount; ci++) {
        const PlayClip &c = play_->clips[ci];
        if (c.start >= t)
            break;  // clips are sorted by start; one starting at t starts its notes itself
        if (t >= c.end || c.kind != (uint8_t)ClipKind::Pattern || c.index >= play_->patternCount)
            continue;
        const PlayPattern &p = play_->patterns[c.index];
        if (p.length <= 0)
            continue;
        // this repetition of the pattern and the one before (its notes can reach into this one)
        const int32_t k = (t - c.start + c.offset) / p.length;
        for (int32_t r = k; r >= 0 && r >= k - 1; r--) {
            const int32_t repBase = c.start - c.offset + r * p.length;
            chasePattern(p, t - repBase, repBase, c.end - repBase, c.start, ci * 131u + (uint32_t)r, off);
        }
    }
}

// notes of `p` (only of chasing slots) that began before pattern-local tick `local` and are still
// sounding there; the pattern's tick 0 is song tick `base`, notes end at `endLocal` at the latest,
// and a note starting before song tick `minStart` (the clip's start) never sounded
void Engine::chasePattern(const PlayPattern &p, int32_t local, int32_t base, int32_t endLocal, int32_t minStart,
                          uint32_t seed, uint32_t offset)
{
    for (uint32_t s = 0; s < p.seqCount; s++) {
        const PlaySeq &q = p.seqs[s];
        if (!chaseSlot(q.slot))
            continue;
        const bool poly = q.loopLen > 0 && q.loopLen < p.length;
        const int32_t L = poly ? q.loopLen : p.length;
        if (L <= 0 || q.count == 0 || local <= 0)
            continue;
        const int32_t last = ((local < p.length ? local : p.length) - 1) / L;
        for (int32_t kk = last; kk >= 0 && kk >= last - 1; kk--) {
            const int32_t b = kk * L;
            for (uint32_t i = 0; i < q.count; i++) {
                const PlayNote &n = q.notes[i];
                const int32_t nl = b + n.start;
                if (n.start >= L || nl >= p.length || nl >= local)
                    break;  // sorted by start: the rest starts later (or never plays)
                int32_t len = n.len;
                if (nl + len > endLocal)
                    len = endLocal - nl;
                if (len <= 0 || nl + len <= local || base + nl < minStart)
                    continue;
                if (n.prob < 255 && (hash32(n.id * 2654435761u ^ (seed * 40503u + (uint32_t)kk)) & 255u) >= n.prob)
                    continue;
                const int32_t elapsed = local - nl;
                noteOn(q.slot, offset, n, run_, len - elapsed, (uint32_t)((int64_t)elapsed * 1000 / kPPQ));
            }
        }
    }
}

void Engine::emitSong(double t0, double t1, double tps, double segPos, double run0)
{
    (void)run0;
    if (!play_)
        return;
    const int32_t S0 = ceilTick(t0), S1 = ceilTick(t1);
    for (uint32_t ci = 0; ci < play_->clipCount; ci++) {
        const PlayClip &c = play_->clips[ci];
        if (c.start >= S1)
            break;
        if (c.end <= S0 || c.kind != (uint8_t)ClipKind::Pattern || c.index >= play_->patternCount)
            continue;
        const PlayPattern &p = play_->patterns[c.index];
        const int32_t L = p.length;
        if (L <= 0)
            continue;
        const int32_t x0 = S0 > c.start ? S0 : c.start, x1 = S1 < c.end ? S1 : c.end;
        if (x0 >= x1)
            continue;
        const int32_t l0 = x0 - c.start + c.offset, l1 = x1 - c.start + c.offset;
        for (int32_t k = l0 / L; k <= (l1 - 1) / L; k++) {
            const int32_t repBase = c.start - c.offset + k * L;  // song tick of this repetition's tick 0
            const int32_t lo = (l0 > k * L ? l0 : k * L) - k * L;
            const int32_t hi = (l1 < (k + 1) * L ? l1 : (k + 1) * L) - k * L;
            emitPattern(p, lo, hi, t0, tps, segPos, (double)repBase, c.end - repBase, ci * 131u + (uint32_t)k);
        }
    }
}

static float mapValue(const PlayAuto &a, float v)
{
    if (a.scale == DAW_SCALE_LOG && a.min > 0)
        return a.min * powf(a.max / a.min, v);
    return a.min + (a.max - a.min) * v;
}

void Engine::automate(double tick)
{
    if (!play_ || play_->autoCount == 0)
        return;
    const int32_t t = (int32_t)tick;
    for (uint32_t ci = 0; ci < play_->clipCount; ci++) {
        const PlayClip &c = play_->clips[ci];
        if (c.start > t)
            break;
        if (c.kind != (uint8_t)ClipKind::Automation || t >= c.end || c.index >= play_->autoCount)
            continue;
        const PlayAuto &a = play_->autos[c.index];
        if (a.pointCount == 0)
            continue;
        const int32_t lt = t - c.start + c.offset;
        float v;
        if (lt <= a.points[0].tick)
            v = a.points[0].value;
        else if (lt >= a.points[a.pointCount - 1].tick)
            v = a.points[a.pointCount - 1].value;
        else {
            uint32_t i = 1;
            while (i < a.pointCount && a.points[i].tick <= lt)
                i++;
            const PlayPoint &p0 = a.points[i - 1], &p1 = a.points[i];
            float f = (float)(lt - p0.tick) / (float)(p1.tick - p0.tick);
            if (p0.tension != 0.0f)
                f = powf(f, exp2f(p0.tension * 3.0f));
            v = p0.value + (p1.value - p0.value) * f;
        }
        float &last = autoLast_[c.index & 63];
        if (fabsf(v - last) < 1e-4f)
            continue;
        last = v;
        const float plain = mapValue(a, v);
        switch (a.kind) {
        case AT_CHAN_VOL: slots_[a.slot % kEngineSlots].vol = plain; break;
        case AT_CHAN_PAN: slots_[a.slot % kEngineSlots].pan = plain; break;
        case AT_TRACK_VOL: tracks_[a.slot % kEngineTracks].vol = plain; break;
        case AT_TRACK_PAN: tracks_[a.slot % kEngineTracks].pan = plain; break;
        case AT_FX_MIX: tracks_[a.slot % kEngineTracks].fx[a.fx % kEngineFx].mix = plain; break;
        case AT_SEND: tracks_[a.slot % kEngineTracks].send[a.fx % kMaxSendsPerTrack] = plain; break;
        case AT_TEMPO: bpm_ = plain < kMinBpm ? kMinBpm : (plain > kMaxBpm ? kMaxBpm : plain); break;
        case AT_CHAN_PARAM:
        case AT_FX_PARAM: {
            daw_event_t e{};
            e.type = DAW_EV_PARAM;
            e.u.param.id = a.paramId;
            e.u.param.value = plain;
            uint8_t target = a.kind == AT_CHAN_PARAM ? (uint8_t)(a.slot % kEngineSlots)
                                                     : (uint8_t)(kFxTarget0 + (a.slot % kEngineTracks) * kEngineFx + a.fx % kEngineFx);
            addEvent(target, 0, e, 3);
            break;
        }
        default: break;
        }
    }
}

void Engine::metronome(double t0, double t1, double tps, double segPos, bool countIn)
{
    if (!metro_ && !countIn)
        return;
    const int32_t beat = ts_.ticksPerBeat(), bar = ts_.ticksPerBar();
    int32_t b = ceilTick(t0 / beat) * beat;
    for (; b < t1 && nClicks_ < 8; b += beat)
        clicks_[nClicks_++] = {offsetAt(segPos, (b - t0) / tps), (b % bar) == 0};
}

void Engine::noteOffs(double run0, double run1, double tps, double segPos)
{
    daw_event_t e{};
    e.type = DAW_EV_NOTE_OFF;
    for (auto &a : active_) {
        if (!a.used || a.offRun >= run1)
            continue;
        e.u.note.note_id = a.voiceId;
        e.u.note.key = a.key;
        e.u.note.velocity = a.rel;
        e.flags = a.midiOffOrder ? DAW_NOTE_FLAG_MIDI : 0;
        addEvent(a.slot, offsetAt(segPos, (a.offRun - run0) / tps), e, a.midiOffOrder ? 2 : 1, a.midiOffOrder);
        a.used = false;
    }
}

void Engine::sequence(uint32_t frames)
{
    // Segments start at fractional sample positions: a loop boundary falls between two samples
    // exactly where the tick clock reaches it, so loops do not drift against the sample clock.
    double pos = 0;  // segment start, samples from the block start
    int guard = 0;
    while (pos < (double)frames - 1e-9 && guard++ < 64) {
        const double tps = ticksPerSample(bpm_, cfg_.sampleRate);
        const double avail = (double)frames - pos;
        if (countIn_ > 0) {
            const double total = (double)countInBars_ * ts_.ticksPerBar();
            const double len = countIn_ / tps < avail ? countIn_ / tps : avail;
            const double c0 = total - countIn_;
            metronome(c0, c0 + len * tps, tps, pos, true);
            countIn_ -= len * tps;
            if (countIn_ < 1e-6)
                countIn_ = 0;
            pos += len;
            continue;
        }
        const int32_t rs = regionStart(), re = regionEnd();
        if (tick_ < rs || tick_ >= re)
            tick_ = rs;
        const double toEnd = (re - tick_) / tps;
        const bool wraps = toEnd <= avail;
        const double len = wraps ? toEnd : avail;
        const double t0 = tick_, t1 = wraps ? (double)re : tick_ + len * tps;
        const double run0 = run_;
        if (chase_) {
            if (play_ && play_->midiSlots && !chaseMidi(ceilTick(t0), offsetAt(pos, 0))) break;
            chase_ = false; midiChaseActive_ = false;
            chase(ceilTick(t0), pos);
        }
        if (mode_ == PlayMode::Pattern) {
            if (play_ && pattern_ < play_->patternCount) {
                const int32_t lo = ceilTick(t0), hi = ceilTick(t1);
                if (hi > lo)
                    emitPattern(play_->patterns[pattern_], lo, hi, t0, tps, pos, 0.0, 0x7FFFFFFF, loopCount_);
            }
        } else {
            if (t0 != lastAutoTick_) {
                automate(t0);
                lastAutoTick_ = t0;
            }
            emitSong(t0, t1, tps, pos, run0);
        }
        metronome(t0, t1, tps, pos, false);
        const double run1 = run0 + (t1 - t0);
        noteOffs(run0, run1, tps, pos);
        run_ = run1;
        tick_ = t1;
        if (wraps) {
            tick_ = rs;
            chase_ = true; midiChaseActive_ = false;
            loopCount_++;
            st_.loops.store(loopCount_, std::memory_order_relaxed);
            if (mode_ == PlayMode::Pattern && queued_ >= 0) {
                pattern_ = (uint32_t)queued_;
                queued_ = -1;
            }
        }
        pos += len;
    }
}

// ---- audio --------------------------------------------------------------------------------

void Engine::render(float *outL, float *outR, uint32_t frames)
{
    // A worker join can sleep this core. Measure its deadline with the monotonic
    // timer, independently of any CPU idle/frequency behavior during that wait.
    const uint64_t beginUs = cfg_.worker ? platform::micros() : 0;
    const uint32_t c0 = platform::cycles();
    if (!ok() || frames == 0) {
        for (uint32_t i = 0; i < frames; i++)
            outL[i] = outR[i] = 0;
        return;
    }
    if (frames > kEngineMaxBlock)
        frames = kEngineMaxBlock;
    nEvents_ = 0;
    nClicks_ = 0;
    EngineCmd c;
    for (int i = 0; i < 128 && cmds_.pop(c); i++)
        handle(c);
    if (playing_)
        sequence(frames);
    processBlock(outL, outR, frames);
    st_.fence.store(pendingFence_, std::memory_order_release);
    samples_ += frames;
    const uint32_t elapsedCycles = cfg_.worker
        ? (uint32_t)((platform::micros() - beginUs) * platform::cyclesPerMicro())
        : platform::cycles() - c0;
    publish(frames, elapsedCycles);
}

void Engine::runInstrumentJob(void *arg)
{
    auto &job = *static_cast<InstrumentJob *>(arg);
    const auto *d = job.inst->desc();
    const uint32_t frames = job.ctx.frames;
    const uint32_t active = d->active_voices ? d->active_voices(job.inst->handle()) : 1;
    if (job.ctx.event_count == 0 && active == 0) {
        job.silent = true;
        memset(job.left, 0, frames * sizeof(float));
        memset(job.right, 0, frames * sizeof(float));
    } else {
        float *outs[2] = {job.left, job.right};
        job.ctx.outputs = outs;
        job.ctx.inputs = nullptr;
        const uint32_t begin = platform::cycles();
        d->process(job.inst->handle(), &job.ctx);
        job.cycles = platform::cycles() - begin;
        job.inst->cycles.fetch_add(job.cycles, std::memory_order_relaxed);
        if (!finiteSum(job.left, frames) || !finiteSum(job.right, frames)) {
            job.engine->st_.nanResets.fetch_add(1, std::memory_order_relaxed);
            daw_event_t off{};
            off.type = DAW_EV_ALL_SOUND_OFF;
            job.ctx.events = &off;
            job.ctx.event_count = 1;
            d->process(job.inst->handle(), &job.ctx);
            memset(job.left, 0, frames * sizeof(float));
            memset(job.right, 0, frames * sizeof(float));
        }
    }
    job.voices = d->active_voices ? d->active_voices(job.inst->handle()) : 0;
    job.inst->voices.store(job.voices, std::memory_order_relaxed);
}

void Engine::workerInstruments(void *arg)
{
    Engine &e = *static_cast<Engine *>(arg);
    Parallel &P = *e.par_;
    const uint64_t t0 = platform::micros();
    int buf = 0;
    for (int i = P.nCand - 1; i >= 0 && buf < kWorkerBufs; --i) {
        const int s = P.cand[i];
        uint8_t expect = kFree;
        if (!P.slotState[s].compare_exchange_strong(expect, kWorker, std::memory_order_acq_rel))
            break;  // the audio core got here first: everything before is its own
        InstrumentJob &job = P.jobs[buf];
        job = InstrumentJob{&e, e.slots_[s].inst, P.ctx, e.workerBuf(buf, 0), e.workerBuf(buf, 1)};
        job.ctx.events = P.count[s] ? &P.sorted[P.start[s]] : nullptr;
        job.ctx.event_count = P.count[s];
        runInstrumentJob(&job);
        Slot &sl = e.slots_[s];
        sl.lastCycles = job.cycles;
        if (P.strip[s]) {  // mix into its own track and run that chain; the buffer is free again
            P.stripVoices[s] = job.voices;
            e.mixSlot(s, job.left, job.right, job.silent, P.k, P.signal, P.ctx.frames);
            daw_process_t ctx = P.ctx;
            e.trackChain(sl.track, ctx, job.left, job.right, P.count, P.start, P.sorted, P.signal, P.k);
        } else {
            P.slotJob[s] = (int8_t)buf++;
        }
        P.slotState[s].store(kWorkerDone, std::memory_order_release);
    }
    e.workerBusyUs_.fetch_add((uint32_t)(platform::micros() - t0), std::memory_order_relaxed);
}

void Engine::workerChains(void *arg)
{
    Engine &e = *static_cast<Engine *>(arg);
    Parallel &P = *e.par_;
    daw_process_t ctx = P.ctx;
    float *scratchL = e.workerBuf(0, 0), *scratchR = e.workerBuf(0, 1);  // the instruments are mixed
    for (;;) {
        // the ready chain the audio core will reach last; the last track (master) stays there
        const int32_t pos = P.audioPos.load(std::memory_order_acquire);
        int pick = -1;
        bool later = false, other = false;
        for (int32_t oi = (int32_t)P.nOrder - 2; oi > pos; --oi) {
            const int ti = P.order[oi] % kEngineTracks;
            if (!P.chainWork[ti] || P.trackState[ti].load(std::memory_order_relaxed) != kFree)
                continue;
            if (pick >= 0) {
                other = true;  // the audio core has another chain to run before this one
                break;
            }
            if (P.pending[ti].load(std::memory_order_acquire) == 0)
                pick = ti;
            else
                later = true;  // will be ready once the audio core has routed its sources
        }
        if (pick < 0) {
            if (!later)
                return;
            spinPause();
            continue;
        }
        // Taking the very next chain while the audio core has nothing else to do only makes it
        // wait (and this core also serves Wi-Fi): leave it unless the audio core is busy.
        if (!other && !P.audioChain.load(std::memory_order_acquire)) {
            spinPause();
            continue;
        }
        uint8_t expect = kFree;
        if (!P.trackState[pick].compare_exchange_strong(expect, kWorker, std::memory_order_acq_rel))
            continue;
        const uint64_t t0 = platform::micros();
        e.trackChain(pick, ctx, scratchL, scratchR, P.count, P.start, P.sorted, P.signal, P.k);
        P.trackState[pick].store(kWorkerDone, std::memory_order_release);
        e.workerBusyUs_.fetch_add((uint32_t)(platform::micros() - t0), std::memory_order_relaxed);
    }
}

void Engine::mixSlot(int s, const float *L, const float *R, bool silent, float k, bool *signal, uint32_t frames)
{
    Slot &sl = slots_[s];
    float tl, tr;
    balance(sl.pan, tl, tr);
    const float g = (sl.mute || sl.fadeOut) ? 0.0f : sl.vol;
    tl *= g;
    tr *= g;
    float *dl = bufs_->track[sl.track][0], *dr = bufs_->track[sl.track][1];
    float cl = sl.curL, cr = sl.curR, pk = 0;
    const bool stableGain = cl + (tl - cl) * k == cl && cr + (tr - cr) * k == cr;
    if (silent) {
        cl = tl;
        cr = tr;
    } else {
        for (uint32_t i = 0; i < frames; i++) {
            if (!stableGain) {
                cl += (tl - cl) * k;
                cr += (tr - cr) * k;
            }
            const float a = L[i] * cl, b = R[i] * cr;
            dl[i] += a;
            dr[i] += b;
            const float m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
            pk = m > pk ? m : pk;
        }
    }
    sl.curL = cl;
    sl.curR = cr;
    if (pk > 0.0f)
        signal[sl.track] = true;
    const float prev = st_.slotMeter[s].load();
    st_.slotMeter[s].store(pk > prev * 0.85f ? pk : prev * 0.85f);
}

void Engine::trackChain(int ti, daw_process_t &ctx, float *scratchL, float *scratchR, const uint16_t *count,
                        const uint16_t *start, const daw_event_t *sorted, bool *signal, float k)
{
    const uint32_t frames = ctx.frames;
    Track &t = tracks_[ti];
    float *L = bufs_->track[ti][0], *R = bufs_->track[ti][1];
    for (int f = 0; f < kEngineFx; f++) {
        FxUnit &u = t.fx[f];
        if (!u.inst || u.failed)
            continue;
        const uint32_t tgt = kFxTarget0 + (uint32_t)ti * kEngineFx + (uint32_t)f;
        const uint32_t n = count[tgt];
        const float target = u.enabled ? u.mix : 0.0f;
        if (target == 0.0f && u.curMix < 1e-4f && n == 0) {
            u.curMix = 0;
            continue;  // bypassed
        }
        const daw_plugin_descriptor_t *d = u.inst->desc();
        // An effect may have a tail or generate output from silence: always run it.
        const float *ins[2] = {L, R};
        float *outs[2] = {scratchL, scratchR};
        ctx.inputs = ins;
        ctx.outputs = outs;
        ctx.events = n ? &sorted[start[tgt]] : nullptr;
        ctx.event_count = n;
        const uint32_t p0 = platform::cycles();
        d->process(u.inst->handle(), &ctx);
        u.inst->cycles.fetch_add(platform::cycles() - p0, std::memory_order_relaxed);
        if (!finiteSum(scratchL, frames) || !finiteSum(scratchR, frames)) {
            // a broken effect is taken out of the chain (dry passes); activate() may allocate,
            // so the control thread must re-create it (status: fxFailures)
            st_.nanResets.fetch_add(1, std::memory_order_relaxed);
            st_.fxFailures.fetch_add(1, std::memory_order_relaxed);
            u.failed = true;
            continue;
        }
        float m = u.curMix;
        const bool stableMix = m + (target - m) * k == m;
        for (uint32_t i = 0; i < frames; i++) {
            if (!stableMix) m += (target - m) * k;
            L[i] += (scratchL[i] - L[i]) * m;
            R[i] += (scratchR[i] - R[i]) * m;
        }
        u.curMix = m;
        if (!signal[ti]) signal[ti] = hasSignal(L, R, frames);
    }
    float tl, tr;
    balance(t.pan, tl, tr);
    const float g = t.mute ? 0.0f : t.vol;
    tl *= g;
    tr *= g;
    if (!signal[ti]) {
        t.curL = advanceSilentGain(t.curL, tl, k, frames);
        t.curR = advanceSilentGain(t.curR, tr, k, frames);
        t.meterL *= 0.85f;
        t.meterR *= 0.85f;
        st_.meterL[ti].store(t.meterL);
        st_.meterR[ti].store(t.meterR);
        if (ti != 0)
            for (int s = 0; s < kMaxSendsPerTrack; s++) {
                const int dst = mix_ ? mix_->sendTarget[ti][s] : -1;
                const float target = t.send[s];
                t.curSend[s] = dst < 0 || dst == ti || (target == 0.0f && t.curSend[s] < 1e-5f)
                                   ? target
                                   : advanceSilentGain(t.curSend[s], target, k, frames);
            }
        return;  // no samples to gain, meter, route or send; buffers are already zero
    }
    float cl = t.curL, cr = t.curR, pl = 0, pr = 0;
    const bool stableGain = cl + (tl - cl) * k == cl && cr + (tr - cr) * k == cr;
    for (uint32_t i = 0; i < frames; i++) {
        if (!stableGain) {
            cl += (tl - cl) * k;
            cr += (tr - cr) * k;
        }
        L[i] *= cl;
        R[i] *= cr;
        pl = fabsf(L[i]) > pl ? fabsf(L[i]) : pl;
        pr = fabsf(R[i]) > pr ? fabsf(R[i]) : pr;
    }
    t.curL = cl;
    t.curR = cr;
    t.meterL = pl > t.meterL * 0.85f ? pl : t.meterL * 0.85f;
    t.meterR = pr > t.meterR * 0.85f ? pr : t.meterR * 0.85f;
    st_.meterL[ti].store(t.meterL);
    st_.meterR[ti].store(t.meterR);
}

void Engine::trackRoute(int ti, bool *signal, float k, uint32_t frames)
{
    if (ti == 0 || !signal[ti])
        return;
    Track &t = tracks_[ti];
    Buffers &B = *bufs_;
    const float *L = B.track[ti][0], *R = B.track[ti][1];
    const int dst = mix_ ? (mix_->route[ti] < 0 ? 0 : mix_->route[ti]) : 0;
    signal[dst] = true;
    float *DL = B.track[dst][0], *DR = B.track[dst][1];
    for (uint32_t i = 0; i < frames; i++) {
        DL[i] += L[i];
        DR[i] += R[i];
    }
    for (int s = 0; s < kMaxSendsPerTrack; s++) {
        const int st = mix_ ? mix_->sendTarget[ti][s] : -1;
        float lvl = t.curSend[s];
        const float tgt = t.send[s];
        if (st < 0 || st == ti || (tgt == 0.0f && lvl < 1e-5f)) {
            t.curSend[s] = tgt;
            continue;
        }
        float *SL = B.track[st][0], *SR = B.track[st][1];
        signal[st] = true;
        const bool stableSend = lvl + (tgt - lvl) * k == lvl;
        for (uint32_t i = 0; i < frames; i++) {
            if (!stableSend) lvl += (tgt - lvl) * k;
            SL[i] += L[i] * lvl;
            SR[i] += R[i] * lvl;
        }
        t.curSend[s] = lvl;
    }
}

void Engine::processBlock(float *outL, float *outR, uint32_t frames)
{
    Buffers &B = *bufs_;
    EventBuffers &E = *eventBufs_;
    // 1. events grouped by target (counting sort), then by offset / priority (insertion sort)
    uint16_t *start = start_;
    uint16_t count[kTargets] = {};
    for (uint32_t i = 0; i < nEvents_; i++)
        count[E.events[i].target]++;
    start[0] = 0;
    for (uint32_t t = 0; t < kTargets; t++)
        start[t + 1] = (uint16_t)(start[t] + count[t]);
    {
        uint16_t fill[kTargets];
        memcpy(fill, start, sizeof fill);
        for (uint32_t i = 0; i < nEvents_; i++)
            E.order[fill[E.events[i].target]++] = (uint16_t)i;
    }
    for (uint32_t t = 0; t < kTargets; t++) {
        for (uint32_t i = start[t] + 1u; i < start[t + 1]; i++) {
            uint16_t v = E.order[i];
            const Event &ev = E.events[v];
            const uint32_t key = ev.ev.offset * 4u + ev.prio;
            uint32_t j = i;
            while (j > start[t]) {
                const Event &pe = E.events[E.order[j - 1]];
                const uint32_t prev = pe.ev.offset * 4u + pe.prio;
                if (prev < key || (prev == key && pe.order <= ev.order))
                    break;
                E.order[j] = E.order[j - 1];
                j--;
            }
            E.order[j] = v;
        }
        for (uint32_t i = start[t]; i < start[t + 1]; i++) {
            E.sorted[i] = E.events[E.order[i]].ev;
            if (E.sorted[i].offset >= frames)
                E.sorted[i].offset = frames - 1;
        }
    }

    daw_process_t ctx{};
    ctx.struct_size = sizeof ctx;
    ctx.frames = frames;
    ctx.transport_flags = playing_ ? DAW_TRANSPORT_PLAYING : 0;
    ctx.sample_pos = samples_;
    ctx.tempo_bpm = bpm_;
    ctx.song_pos_beats = tick_ / (double)kPPQ;

    for (int t = 0; t < kEngineTracks; t++) {
        memset(B.track[t][0], 0, frames * sizeof(float));
        memset(B.track[t][1], 0, frames * sizeof(float));
    }

    const float k = 1.0f - expf(-1.0f / (0.005f * cfg_.sampleRate));  // 5 ms gain smoothing
    bool signal[kEngineTracks] = {};
    uint32_t voices = 0;

    // 2. instruments
    // With a worker, it claims eligible slots from the end of the slot list into its own buffers
    // while this core renders from the front; they meet in the middle. Every accumulation into the
    // tracks stays here, in slot order: the mix is bit-identical to the serial renderer.
    Parallel *P = cfg_.worker ? par_ : nullptr;
    bool phaseA = false;
    bool isCand[kEngineSlots] = {};
    bool stripTrack[kEngineTracks] = {};  // effect chain already run with its channel strip
    if (P) {
        int live = 0;
        bool sounding[kEngineSlots] = {};
        for (int s = 0; s < kEngineSlots; ++s) {
            const Slot &sl = slots_[s];
            P->slotState[s].store(kFree, std::memory_order_relaxed);
            P->slotJob[s] = -1;
            if (!sl.inst)
                continue;
            const auto *d = sl.inst->desc();
            const uint32_t v = d->active_voices ? d->active_voices(sl.inst->handle()) : 1;
            if (!v && !count[s] && !sl.pending)
                continue;  // silent: nothing worth overlapping
            sounding[s] = true;
            live++;
        }
        const uint32_t budgetCycles = frames * (platform::cyclesPerMicro() * 1000000u / cfg_.sampleRate);
        // tracks with exactly one instrument slot and no other track routed or sent into them
        uint8_t writers[kEngineTracks] = {};
        for (int s = 0; s < kEngineSlots; ++s)
            if (slots_[s].inst)
                writers[slots_[s].track % kEngineTracks]++;
        if (mix_)
            for (uint32_t oi = 0; oi < mix_->count; ++oi) {
                const int ti = mix_->order[oi] % kEngineTracks;
                if (ti == 0)
                    continue;
                const int dst = mix_->route[ti] < 0 ? 0 : mix_->route[ti];
                if (dst != ti)
                    writers[dst] = 2;
                for (int x = 0; x < kMaxSendsPerTrack; x++)
                    if (mix_->sendTarget[ti][x] >= 0 && mix_->sendTarget[ti][x] != ti)
                        writers[mix_->sendTarget[ti][x]] = 2;
            }
        P->nCand = 0;
        for (int s = 0; s < kEngineSlots; ++s) {
            const Slot &sl = slots_[s];
            P->strip[s] = false;
            if (!sounding[s] || sl.pending || sl.fadeOut)
                continue;  // swaps and removals stay on this core (they retire instances)
            // Small jobs are not worth a hand-off: both cores share the caches, and every extra
            // plugin running beside the audio core's work costs it misses (measured on the board:
            // offloading many 1-3 % instruments made a shared-bus groove 4 points slower). Use the
            // last block's cost, or the plugin's estimate before the first one.
            const auto *d = sl.inst->desc();
            const uint32_t v = d->active_voices ? d->active_voices(sl.inst->handle()) : 1;
            const uint64_t est = sl.lastCycles ? sl.lastCycles
                                               : (uint64_t)budgetCycles * (d->cpu_permille_base + d->cpu_permille_voice * (v ? v : 1)) / 1000;
            if (est * 1000 < (uint64_t)budgetCycles * cfg_.workerMinPermille)
                continue;
            P->cand[P->nCand++] = (uint8_t)s;
            isCand[s] = true;
            // only worth it with effects to run: otherwise it just moves the mixing to the worker
            bool fx = false;
            for (int f = 0; f < kEngineFx && !fx; f++) {
                const FxUnit &u = tracks_[sl.track].fx[f];
                const uint32_t nf = count[kFxTarget0 + (uint32_t)sl.track * kEngineFx + (uint32_t)f];
                fx = u.inst && !u.failed && !((u.enabled ? u.mix : 0.0f) == 0.0f && u.curMix < 1e-4f && nf == 0);
            }
            P->strip[s] = sl.track != 0 && writers[sl.track] == 1 && fx;
        }
        if (live >= 2 && P->nCand > 0) {
            P->ctx = ctx;
            P->count = count;
            P->start = start;
            P->sorted = E.sorted;
            P->signal = signal;
            P->k = k;
            phaseA = true;
            cfg_.worker->start(cfg_.worker->context, workerInstruments, this);
        }
    }
    auto runInstrument = [&](PluginInstance *inst, const daw_event_t *ev, uint32_t nev, float *L, float *R, uint32_t &vc) {
        InstrumentJob job{this, inst, ctx, L, R};
        job.ctx.events = nev ? ev : nullptr;
        job.ctx.event_count = nev;
        runInstrumentJob(&job);
        vc = job.voices;
        return job;
    };
    const float xfStep = 1.0f / (0.005f * cfg_.sampleRate);  // instrument swaps crossfade over 5 ms
    for (int s = 0; s < kEngineSlots; s++) {
        Slot &sl = slots_[s];
        if (!sl.inst)
            continue;
        const uint32_t n = count[s];
        const daw_event_t *ev = n ? &E.sorted[start[s]] : nullptr;
        float *L = B.slot[0], *R = B.slot[1];
        // a replacement takes every event from the moment it was installed (no note is lost to the
        // instance that is going away); both play while the old one fades out and the new one in
        PluginInstance *next = sl.pending && sl.pending != kRemoveMarker() ? sl.pending : nullptr;
        uint32_t vOld = 0, vNew = 0;
        bool silent;
        bool fromWorker = false;
        if (phaseA && isCand[s]) {
            uint8_t expect = kFree;
            if (!P->slotState[s].compare_exchange_strong(expect, kAudio, std::memory_order_acq_rel)) {
                if (P->strip[s]) {
                    stripTrack[sl.track] = true;  // the worker mixes it and runs its chain
                    continue;
                }
                const uint64_t w0 = platform::micros();
                while (P->slotState[s].load(std::memory_order_acquire) != kWorkerDone)
                    spinPause();
                statWaitUs_ += platform::micros() - w0;
                fromWorker = true;
            }
        }
        if (phaseA && isCand[s] && P->strip[s] && !fromWorker) {  // a strip of this core
            const auto job = runInstrument(sl.inst, ev, n, L, R, vOld);
            sl.lastCycles = job.cycles;
            voices += vOld;
            mixSlot(s, L, R, job.silent, k, signal, frames);
            trackChain(sl.track, ctx, B.scratch[0], B.scratch[1], count, start, E.sorted, signal, k);
            stripTrack[sl.track] = true;
            continue;
        }
        if (fromWorker) {
            const InstrumentJob &job = P->jobs[P->slotJob[s]];
            L = job.left;
            R = job.right;
            vOld = job.voices;
            silent = job.silent;
            sl.lastCycles = job.cycles;
        } else {
            const auto job = runInstrument(sl.inst, next ? nullptr : ev, next ? 0 : n, L, R, vOld);
            silent = job.silent;
            sl.lastCycles = job.cycles;
        }
        if (next) {
            float *NL = B.scratch[0], *NR = B.scratch[1];
            const auto job = runInstrument(next, ev, n, NL, NR, vNew);
            const bool silentNew = job.silent;
            sl.lastCycles += job.cycles;
            float xf = sl.xf;
            for (uint32_t i = 0; i < frames; i++) {
                xf = xf + xfStep < 1.0f ? xf + xfStep : 1.0f;
                L[i] = L[i] * (1.0f - xf) + NL[i] * xf;
                R[i] = R[i] * (1.0f - xf) + NR[i] * xf;
            }
            sl.xf = xf;
            silent = silent && silentNew;
            if (xf >= 1.0f) {  // the old instance is out: retire it
                retire(Retired::Instance, sl.inst);
                sl.inst = next;
                sl.pending = nullptr;
                sl.xf = 0;
            }
        }
        voices += vOld + vNew;
        mixSlot(s, L, R, silent, k, signal, frames);
        if (sl.fadeOut && sl.curL < 1e-4f && sl.curR < 1e-4f) {  // removal: faded out, now gone
            retire(Retired::Instance, sl.inst);
            sl.inst = nullptr;
            sl.pending = nullptr;
            sl.fadeOut = false;
        }
    }
    if (phaseA) {
        const uint64_t w0 = platform::micros();
        cfg_.worker->join(cfg_.worker->context);
        statWaitUs_ += platform::micros() - w0;
        for (int s = 0; s < kEngineSlots; ++s)
            if (P->strip[s] && P->slotState[s].load(std::memory_order_relaxed) == kWorkerDone)
                voices += P->stripVoices[s];
    }

    // 3. mixer: inserts in dependency order, master last. With a worker, it takes effect chains
    // of tracks whose inputs are complete (all sources accumulated), from the end of the order;
    // routing and sends stay here, in the serial order.
    static const uint8_t kDefaultOrder[kEngineTracks] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0};
    const uint8_t *order = mix_ ? mix_->order : kDefaultOrder;
    const uint32_t nOrder = mix_ ? mix_->count : kEngineTracks;
    auto routeTargets = [&](int ti, auto &&fn) {  // the tracks trackRoute() can write into
        if (ti == 0)
            return;
        const int dst = mix_ ? (mix_->route[ti] < 0 ? 0 : mix_->route[ti]) : 0;
        if (dst != ti)
            fn(dst);
        for (int s = 0; s < kMaxSendsPerTrack; s++) {
            const int st = mix_ ? mix_->sendTarget[ti][s] : -1;
            if (st >= 0 && st != ti)
                fn(st);
        }
    };
    bool phaseB = false;
    if (P) {
        // chains at the same depth of the routing graph can run at the same time; a pure series
        // (instruments -> bus -> bus -> master) gives the worker nothing but hand-offs
        // counted in plain arrays: atomic read-modify-writes are library calls on this target
        uint8_t depth[kEngineTracks] = {}, perDepth[kEngineTracks] = {}, sources[kEngineTracks] = {};
        bool parallelChains = false;
        for (uint32_t oi = 0; oi < nOrder; oi++) {
            const int ti = order[oi] % kEngineTracks;
            P->chainWork[ti] = false;
            for (int f = 0; f < kEngineFx && !stripTrack[ti]; f++) {
                const FxUnit &u = tracks_[ti].fx[f];
                const uint32_t n = count[kFxTarget0 + (uint32_t)ti * kEngineFx + (uint32_t)f];
                if (u.inst && !u.failed && !((u.enabled ? u.mix : 0.0f) == 0.0f && u.curMix < 1e-4f && n == 0))
                    P->chainWork[ti] = true;
            }
            if (P->chainWork[ti] && oi + 1 < nOrder && ++perDepth[depth[ti]] >= 2)  // not the master
                parallelChains = true;
            routeTargets(ti, [&](int x) {
                sources[x]++;
                if (depth[x] < depth[ti] + 1)
                    depth[x] = (uint8_t)(depth[ti] + 1);
            });
        }
        if (parallelChains) {
            for (int t = 0; t < kEngineTracks; t++) {
                P->pending[t].store(sources[t], std::memory_order_relaxed);
                P->trackState[t].store(kFree, std::memory_order_relaxed);
            }
            P->ctx = ctx;
            P->count = count;
            P->start = start;
            P->sorted = E.sorted;
            P->signal = signal;
            P->k = k;
            P->order = order;
            P->nOrder = nOrder;
            P->audioPos.store(-1, std::memory_order_relaxed);
            P->audioChain.store(false, std::memory_order_relaxed);
            phaseB = true;
            cfg_.worker->start(cfg_.worker->context, workerChains, this);
        }
    }
    for (uint32_t oi = 0; oi < nOrder; oi++) {
        const int ti = order[oi] % kEngineTracks;
        bool chained = false;
        if (phaseB) {
            P->audioPos.store((int32_t)oi, std::memory_order_release);
            if (P->chainWork[ti] && oi + 1 < nOrder) {
                uint8_t expect = kFree;
                if (!P->trackState[ti].compare_exchange_strong(expect, kAudio, std::memory_order_acq_rel)) {
                    const uint64_t w0 = platform::micros();
                    while (P->trackState[ti].load(std::memory_order_acquire) != kWorkerDone)
                        spinPause();
                    statWaitUs_ += platform::micros() - w0;
                    chained = true;
                }
            }
        }
        if (!chained && !stripTrack[ti]) {
            if (phaseB)
                P->audioChain.store(true, std::memory_order_release);
            trackChain(ti, ctx, B.scratch[0], B.scratch[1], count, start, E.sorted, signal, k);
            if (phaseB)
                P->audioChain.store(false, std::memory_order_release);
        }
        trackRoute(ti, signal, k, frames);
        if (phaseB)  // its targets may now be complete
            routeTargets(ti, [&](int x) { P->pending[x].fetch_sub(1, std::memory_order_release); });
    }
    if (phaseB) {
        P->audioPos.store((int32_t)nOrder, std::memory_order_release);
        const uint64_t w0 = platform::micros();
        cfg_.worker->join(cfg_.worker->context);
        statWaitUs_ += platform::micros() - w0;
    }

    // 4. output: master, metronome, diagnostics, safety
    memcpy(outL, B.track[0][0], frames * sizeof(float));
    memcpy(outR, B.track[0][1], frames * sizeof(float));
    if (nClicks_)
        click(outL, outR, frames);
    else if (clickEnv_ > 1e-4f)
        click(outL, outR, frames);
    if (diag_ != Diag::Off)
        diag(outL, outR, frames);
    if (!finiteSum(outL, frames) || !finiteSum(outR, frames)) {
        st_.nanResets.fetch_add(1, std::memory_order_relaxed);
        memset(outL, 0, frames * sizeof(float));
        memset(outR, 0, frames * sizeof(float));
    }
    if (masterSafety_)
        for (uint32_t i = 0; i < frames; i++) {
            outL[i] = outL[i] > 1.0f ? 1.0f : (outL[i] < -1.0f ? -1.0f : outL[i]);
            outR[i] = outR[i] > 1.0f ? 1.0f : (outR[i] < -1.0f ? -1.0f : outR[i]);
        }
    st_.voices.store(voices, std::memory_order_relaxed);
}

void Engine::click(float *L, float *R, uint32_t frames)
{
    const float decay = expf(-1.0f / (0.012f * cfg_.sampleRate));
    uint32_t ci = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (ci < nClicks_ && clicks_[ci].offset <= i) {
            clickEnv_ = 1.0f;
            clickPhase_ = 0;
            clickInc_ = (clicks_[ci].accent ? 1760.0f : 1320.0f) / (float)cfg_.sampleRate;
            ci++;
        }
        if (clickEnv_ > 1e-4f) {
            const float s = sinTurns(clickPhase_) * clickEnv_ * metroVol_ * 0.5f;
            clickPhase_ += clickInc_;
            if (clickPhase_ >= 1.0f)
                clickPhase_ -= 1.0f;
            clickEnv_ *= decay;
            L[i] += s;
            R[i] += s;
        }
    }
}

void Engine::diag(float *L, float *R, uint32_t frames)
{
    const float sr = (float)cfg_.sampleRate;
    for (uint32_t i = 0; i < frames; i++, diagPos_++) {
        float l = 0, r = 0;
        switch (diag_) {
        case Diag::Sine440: {
            const float s = 0.25f * sinTurns(diagPhase_);  // -12 dBFS
            diagPhase_ += 440.0f / sr;
            l = r = s;
            break;
        }
        case Diag::LeftRight: {  // 1 s 440 Hz left only, 1 s 660 Hz right only
            const bool left = (diagPos_ / cfg_.sampleRate) % 2 == 0;
            const float s = 0.25f * sinTurns(diagPhase_);
            diagPhase_ += (left ? 440.0f : 660.0f) / sr;
            (left ? l : r) = s;
            break;
        }
        case Diag::LowLevel: {  // 1 kHz at -60 dBFS
            const float s = 0.001f * sinTurns(diagPhase_);
            diagPhase_ += 1000.0f / sr;
            l = r = s;
            break;
        }
        case Diag::Click:  // a click every 500 ms (the metronome sound)
            if (diagPos_ % (cfg_.sampleRate / 2) == 0) {
                clickEnv_ = 1.0f;
                clickPhase_ = 0;
                clickInc_ = 1760.0f / sr;
            }
            if (clickEnv_ > 1e-4f) {
                l = r = sinTurns(clickPhase_) * clickEnv_ * 0.5f;
                clickPhase_ += clickInc_;
                clickEnv_ *= 0.9981f;
            }
            break;
        default: break;  // Silence
        }
        if (diagPhase_ >= 1.0f)
            diagPhase_ -= 1.0f;
        L[i] = l;
        R[i] = r;
    }
}

void Engine::publish(uint32_t frames, uint32_t cyc)
{
    const uint32_t cpm = platform::cyclesPerMicro();
    const uint32_t us = cyc / (cpm ? cpm : 1);
    const uint32_t budget = frames * 1000000u / cfg_.sampleRate;
    if (us > budget)
        st_.overruns.fetch_add(1, std::memory_order_relaxed);
    const uint32_t testLoad = st_.testLoadPermille.load(std::memory_order_relaxed);
    const int32_t blockLoad = (int32_t)((uint64_t)us * 1000u / (budget ? budget : 1)) + (int32_t)testLoad;
    load_ += (blockLoad - load_) / 8;  // ~8 blocks (46 ms at 256 frames)
    st_.loadPermille.store((uint32_t)(load_ > 0 ? load_ : 0), std::memory_order_relaxed);
    // trips above the threshold, clears 10 % of the deadline below it
    if (cfg_.cpuGuardPermille)
        guardTripped_ = load_ > (int32_t)cfg_.cpuGuardPermille || (guardTripped_ && load_ > (int32_t)cfg_.cpuGuardPermille - 100);
    statCycles_ += cyc;
    statBlocks_++;
    statFrames_ += frames;
    if (us > statMaxUs_)
        statMaxUs_ = us;
    if (statFrames_ >= cfg_.sampleRate) {  // once per second of audio
        const uint32_t avg = statCycles_ / statBlocks_ / (cpm ? cpm : 1);
        st_.renderUsAvg.store(avg, std::memory_order_relaxed);
        st_.renderUsMax.store(statMaxUs_ > st_.renderUsMax.load() ? statMaxUs_ : st_.renderUsMax.load(), std::memory_order_relaxed);
        const uint64_t periodUs = (uint64_t)statFrames_ * 1000000u / cfg_.sampleRate;
        const uint64_t totalUs = (uint64_t)statCycles_ / (cpm ? cpm : 1);
        st_.cpuPermille.store((uint32_t)(totalUs * 1000u / (periodUs ? periodUs : 1)) + testLoad, std::memory_order_relaxed);
        auto pub = [&](PluginInstance *p) {
            if (!p)
                return;
            const uint64_t pu = p->cycles.exchange(0) / (cpm ? cpm : 1);
            p->cpuPermille.store((uint32_t)(pu * 1000u / (periodUs ? periodUs : 1)), std::memory_order_relaxed);
        };
        for (auto &s : slots_)
            pub(s.inst);
        for (auto &t : tracks_)
            for (auto &f : t.fx)
                pub(f.inst);
        if (cfg_.worker) {
            st_.workerWaitPermille.store((uint32_t)(statWaitUs_ * 1000u / (periodUs ? periodUs : 1)), std::memory_order_relaxed);
            st_.workerBusyPermille.store((uint32_t)(workerBusyUs_.exchange(0) * 1000ull / (periodUs ? periodUs : 1)), std::memory_order_relaxed);
            statWaitUs_ = 0;
        }
        statCycles_ = statBlocks_ = statFrames_ = statMaxUs_ = 0;
    }
    st_.blocks.fetch_add(1, std::memory_order_relaxed);
    st_.playing.store(countIn_ > 0 ? 3u : (playing_ ? 1u : (paused_ ? 2u : 0u)), std::memory_order_relaxed);
    st_.mode.store((uint32_t)mode_, std::memory_order_relaxed);
    st_.tick.store((int32_t)tick_, std::memory_order_relaxed);
    st_.patternIndex.store(pattern_, std::memory_order_relaxed);
    st_.queuedPattern.store(queued_, std::memory_order_relaxed);
    st_.bpm.store((float)bpm_);
}

}  // namespace daw
