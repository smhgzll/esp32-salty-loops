// SPDX-License-Identifier: GPL-3.0-or-later
// The real-time audio engine: transport, sample-clock sequencer, instruments, mixer graph,
// metronome and diagnostic tones. render() runs on the audio thread (ESP32-S3: core 1, priority
// 20, optionally one instrument on core 0) never allocates, takes an application mutex, logs or
// touches the project document. The platform worker uses bounded task notifications. Everything it needs
// arrives through a bounded SPSC command queue from the control thread:
//   - immutable compiled snapshots (PlayModel, MixModel) swapped by pointer,
//   - plugin instances installed / replaced by pointer (old ones handed back for destruction),
//   - parameter, level, transport and live-note commands.
// Status flows back through atomics (EngineStatus) and a retire queue.
#pragma once

#include <stdint.h>

#include <atomic>

#include "daw/model.hpp"
#include "daw/rt.hpp"
#include "daw_plugin_api.h"

namespace daw {

class PluginInstance;

constexpr uint32_t kEngineMaxBlock = 256;
constexpr int kEngineSlots = kMaxChannels;     // instrument slots
constexpr int kEngineTracks = kMaxMixerTracks; // mixer tracks
constexpr int kEngineFx = kMaxFxPerTrack;
constexpr uint32_t kBlockEvents = 512;          // all plugin events of one block
constexpr uint32_t kMaxActiveNotes = 256;       // sequenced notes waiting for their note-off

// ---- compiled snapshots (built by compile.cpp on the control thread, read-only afterwards) ----

struct PlayNote {
    int32_t start, len;
    uint32_t id;
    uint8_t key;
    uint8_t flags;  // DAW_NOTE_FLAG_SLIDE | DAW_NOTE_FLAG_ACCENT
    uint8_t prob;   // 0..255, 255 = always
    uint8_t pad;
    float vel, pan, pitch, rel;
    uint32_t midiOnOrder, midiOffOrder;
};

struct PlayMidi {
    int32_t tick;
    uint32_t order, size, nextState; // next replaceable state event (for bounded seek replay)
    const uint8_t *data;
};

struct PlaySeq {
    uint8_t slot;        // engine instrument slot
    int32_t loopLen;     // > 0: the steps repeat every loopLen ticks (polymeter); 0: pattern length
    uint32_t count;
    const PlayNote *notes;  // sorted by start
    uint32_t midiCount;
    const PlayMidi *midi;
};

struct PlayPattern {
    uint32_t id;
    int32_t length;
    uint32_t seqCount;
    const PlaySeq *seqs;
};

struct PlayClip {
    int32_t start, end, offset;
    uint16_t index;  // pattern index / automation index
    uint8_t kind;    // ClipKind
    uint8_t track;
};

struct PlayPoint {
    int32_t tick;
    float value, tension;
};

struct PlayAuto {
    uint8_t kind;      // AutoTargetKind below
    uint8_t slot;      // instrument slot / mixer track
    uint8_t fx;        // fx index on the track
    uint8_t scale;     // daw_param_scale_t of a plugin parameter
    uint32_t paramId;
    float min, max;
    uint32_t pointCount;
    const PlayPoint *points;
};

enum AutoTargetKind : uint8_t { AT_NONE, AT_CHAN_VOL, AT_CHAN_PAN, AT_CHAN_PARAM, AT_TRACK_VOL, AT_TRACK_PAN, AT_FX_PARAM, AT_FX_MIX, AT_SEND, AT_TEMPO };

struct PlayModel {
    uint32_t version;
    uint32_t patternCount;
    const PlayPattern *patterns;
    uint32_t clipCount;
    const PlayClip *clips;   // sorted by start
    uint32_t autoCount;
    const PlayAuto *autos;
    int32_t songLength;
    uint32_t midiSlots;      // bitmask; zero keeps the legacy sequencing path unchanged
    void *arena;             // one allocation holding everything above (freed as a whole)
};

struct MixModel {
    uint8_t order[kEngineTracks];          // processing order: inserts (dependencies first), master last
    uint8_t count;
    int8_t route[kEngineTracks];           // destination of each insert (master: -1 = output)
    int8_t sendTarget[kEngineTracks][kMaxSendsPerTrack];  // -1 = unused
};

// ---- commands -----------------------------------------------------------------------------

struct EngineCmd {
    enum Type : uint8_t {
        Play, Stop, Pause, Continue, Seek, SetMode, SetPattern, QueuePattern, SetTempo, SetTimeSig, SetLoop,
        SetMetronome, SetCountIn, SwapPlay, SwapMix, SetInstrument, SetFx, Param, ChannelLevel, TrackLevel,
        SendLevel, FxState, Note, NotesOff, Diag, MasterSafety, SetFxChain, RawEvent, SetRecord, Fence, ResetLoad
    };
    uint8_t type;
    uint8_t a, b, c;
    uint32_t u32;
    union {
        float f[4];
        double d;
        int32_t i[4];
        void *p;
    };
};

// SetFxChain payload: a track's complete effect chain. The engine keeps the per-unit state of
// instances that stay in the chain (wherever they move) and retires the ones that left.
struct FxChainMsg {
    PluginInstance *fx[kMaxFxPerTrack];
    bool enabled[kMaxFxPerTrack];
    float mix[kMaxFxPerTrack];
};

// a live note as the engine played it while recording (MIDI recording): the pattern tick of the
// block it sounded in, so network jitter does not move it
struct RecEvent {
    int32_t tick;      // pattern-relative
    uint32_t pattern;  // pattern index
    uint8_t slot, key;
    bool on;
    float vel;
};

// what the audio thread hands back to the control thread for destruction
struct Retired {
    enum Kind : uint8_t { Instance, Play, Mix, Chain } kind;
    void *p;
};

enum class Diag : uint8_t { Off = 0, Sine440, LeftRight, LowLevel, Silence, Click };
enum class PlayMode : uint8_t { Pattern = 0, Song = 1 };

struct EngineStatus {
    std::atomic<uint32_t> fence{0};           // command fence completed at the end of a render block
    std::atomic<uint32_t> blocks{0};          // rendered blocks (sample clock = blocks * blockFrames)
    std::atomic<uint32_t> playing{0};         // 0 stopped, 1 playing, 2 paused, 3 count-in
    std::atomic<uint32_t> mode{0};
    std::atomic<int32_t> tick{0};             // pattern- or song-relative position
    std::atomic<uint32_t> patternIndex{0};
    std::atomic<int32_t> queuedPattern{-1};
    std::atomic<uint32_t> loops{0};           // pattern / song wraps
    AtomicFloat bpm;
    std::atomic<uint32_t> renderUsAvg{0}, renderUsMax{0}, budgetUs{0};
    std::atomic<uint32_t> overruns{0};        // blocks whose render took longer than the block period
    std::atomic<uint32_t> eventDrops{0};      // events that did not fit the block's event pool
    std::atomic<uint32_t> noteDrops{0};       // active-note table full: oldest note ended early
    std::atomic<uint32_t> nanResets{0};       // non-finite output blocks muted
    std::atomic<uint32_t> fxFailures{0};      // effects taken out of their chain (non-finite output)
    std::atomic<uint32_t> cmdOverflow{0};     // (control side) commands refused: queue full
    std::atomic<uint32_t> retireOverflow{0};  // objects leaked because the retire queue was full
    std::atomic<uint32_t> voices{0};
    std::atomic<uint32_t> cpuPermille{0};     // render time / block period, x1000 (1 s average)
    std::atomic<uint32_t> loadPermille{0};    // the same, smoothed over ~50 ms (the CPU guard's input)
    std::atomic<uint32_t> cpuGuardDrops{0};   // note-ons skipped by the CPU guard
    std::atomic<uint32_t> testLoadPermille{0};  // tests only: added to every load measurement
    // with a worker (1 s averages, x1000 of the block period): time the audio core spent waiting
    // for worker results, and time the worker spent rendering
    std::atomic<uint32_t> workerWaitPermille{0}, workerBusyPermille{0};
    AtomicFloat meterL[kEngineTracks], meterR[kEngineTracks];
    AtomicFloat slotMeter[kEngineSlots];
};

class Engine {
public:
    // One persistent worker, owned by the platform. start/join publish and finish a
    // single job without allocating. The engine starts at most two per block (instruments,
    // then effect chains); each job claims work items itself, and the engine joins it before
    // the next phase and before retiring anything.
    struct Worker {
        void *context;
        void (*start)(void *, void (*)(void *), void *);
        void (*join)(void *);
    };
    struct Config {
        uint32_t sampleRate = 44100;
        uint32_t blockFrames = 256;
        // CPU guard: new note-ons are skipped while the measured render load (smoothed over
        // ~50 ms) is above this share of the block period, x1000. 0 = off (QEMU: timing not real).
        uint32_t cpuGuardPermille = 0;
        Worker *worker = nullptr;
        // instrument jobs cheaper than this share of the block (x1000, last block's cost) stay on
        // the audio core; 0 offloads everything (tests). The board uses 25 (docs/PERFORMANCE.md).
        uint32_t workerMinPermille = 0;
    };
    explicit Engine(const Config &cfg);
    ~Engine();
    bool ok() const { return bufs_ != nullptr && eventBufs_ != nullptr && cmds_.ok() && retired_.ok() && recorded_.ok(); }
    const Config &config() const { return cfg_; }

    // control thread
    bool post(const EngineCmd &c);
    bool popRetired(Retired &r) { return retired_.pop(r); }
    bool popRecorded(RecEvent &r) { return recorded_.pop(r); }
    EngineStatus &status() { return st_; }
    uint32_t queueFree() const { return cmds_.capacity() - cmds_.size(); }

    // audio thread: frames <= blockFrames
    void render(float *outL, float *outR, uint32_t frames);

private:
    struct Event {
        daw_event_t ev;
        uint8_t target;  // instrument slot, or kFxTarget0 + track * kEngineFx + fx
        uint32_t order; // original MIDI order; 0 = ordinary DAW ordering
        uint8_t prio;    // ordering at equal offsets: slide-on, off, on, other
    };
    static constexpr uint8_t kFxTarget0 = kEngineSlots;
    // ordering of events at the same sample: sequenced slide-ons (0) before sequenced note-offs (1)
    // before note-ons (2); live input (2, stable) keeps its arrival order; parameters (3) last
    static constexpr uint8_t kLivePrio = 2;
    static constexpr uint32_t kTargets = kEngineSlots + kEngineTracks * kEngineFx;

    struct ActiveNote {
        double offRun;  // run-tick of the note-off
        int32_t voiceId;
        uint8_t slot, key;
        float rel;
        bool used;
        uint32_t midiOffOrder;
    };
    struct Slot {
        PluginInstance *inst = nullptr;
        PluginInstance *pending = nullptr;  // a replacement being crossfaded in, or kRemoveMarker()
        float xf = 0;                       // crossfade position 0..1
        float vol = 0.78f, pan = 0, gl = 1, gr = 1;  // targets
        float curL = 0, curR = 0;                     // smoothed gains
        bool mute = false;
        uint8_t track = 0;
        bool fadeOut = false;
        uint32_t lastCycles = 0;
    };
    struct FxUnit {
        PluginInstance *inst = nullptr;
        bool enabled = true;
        bool failed = false;  // produced NaN / Inf: bypassed until replaced
        float mix = 1, curMix = 0;
    };
    struct Track {
        float vol = 0.8f, pan = 0, curL = 0, curR = 0;
        bool mute = false;
        float send[kMaxSendsPerTrack] = {}, curSend[kMaxSendsPerTrack] = {};
        FxUnit fx[kEngineFx];
        float meterL = 0, meterR = 0;
    };
    struct Buffers {  // internal RAM
        float track[kEngineTracks][2][kEngineMaxBlock];
        float slot[2][kEngineMaxBlock];
        float scratch[2][kEngineMaxBlock];
    };
    // Parallel rendering (docs/AUDIO_ENGINE.md): the worker claims instrument slots from the end
    // of the slot list and ready effect chains from the end of the mixer order, the audio core
    // works from the front; every accumulation stays on the audio core in the serial order.
    static constexpr int kWorkerBufs = 4;  // instrument outputs the worker can hold per block
    enum : uint8_t { kFree = 0, kAudio = 1, kWorker = 2, kWorkerDone = 3 };
    float (*workerOut_)[2][kEngineMaxBlock] = nullptr;  // [kWorkerBufs]; [0] in internal RAM
    float (*workerOutLarge_)[2][kEngineMaxBlock] = nullptr;
    float *workerBuf(int b, int ch) { return b == 0 ? workerOut_[0][ch] : workerOutLarge_[b - 1][ch]; }
    struct InstrumentJob {
        Engine *engine;
        PluginInstance *inst;
        daw_process_t ctx;
        float *left, *right;
        uint32_t voices = 0, cycles = 0;
        bool silent = false;
    };
    static void runInstrumentJob(void *);
    struct Parallel {  // per block, written by the audio core before each start()
        daw_process_t ctx;
        uint8_t cand[kEngineSlots];  // worker-eligible slots in slot order
        int nCand = 0;
        // a channel strip: the slot is the only writer of its insert track, so whichever core
        // renders it also mixes it into the track and runs that track's effect chain
        bool strip[kEngineSlots] = {};
        uint32_t stripVoices[kEngineSlots] = {};
        const uint16_t *count = nullptr, *start = nullptr;
        const daw_event_t *sorted = nullptr;
        bool *signal = nullptr;
        float k = 0;
        const uint8_t *order = nullptr;
        uint32_t nOrder = 0;
        bool chainWork[kEngineTracks] = {};
        std::atomic<uint8_t> slotState[kEngineSlots];
        int8_t slotJob[kEngineSlots];
        std::atomic<uint8_t> trackState[kEngineTracks];
        std::atomic<uint8_t> pending[kEngineTracks];  // source tracks not yet accumulated
        std::atomic<int32_t> audioPos{-1};            // mixer-order position of the audio core
        std::atomic<bool> audioChain{false};          // the audio core is running a chain itself
        InstrumentJob jobs[kWorkerBufs];
    };
    Parallel *par_ = nullptr;  // internal RAM, only with a worker
    uint64_t statWaitUs_ = 0;               // audio core waiting for the worker (this second)
    std::atomic<uint32_t> workerBusyUs_{0};  // worker rendering time (this second)
    static void workerInstruments(void *);
    static void workerChains(void *);
    // the part of a mixer track that touches only its own buffer and state (effects, gain,
    // meter; when silent also its send levels), and its routing into other tracks
    void trackChain(int ti, daw_process_t &ctx, float *scratchL, float *scratchR, const uint16_t *count,
                    const uint16_t *start, const daw_event_t *sorted, bool *signal, float k);
    void trackRoute(int ti, bool *signal, float k, uint32_t frames);
    // a slot's output into its track: gain smoothing, slot meter, signal flag
    void mixSlot(int s, const float *L, const float *R, bool silent, float k, bool *signal, uint32_t frames);
    struct EventBuffers {  // once per event/block, never in the sample loop: PSRAM
        Event events[kBlockEvents];
        daw_event_t sorted[kBlockEvents];
        uint16_t order[kBlockEvents];
    };

    static PluginInstance *kRemoveMarker() { return reinterpret_cast<PluginInstance *>(uintptr_t(1)); }
    void handle(const EngineCmd &c);
    void retire(Retired::Kind k, void *p);
    void addEvent(uint8_t target, uint32_t offset, const daw_event_t &e, uint8_t prio, uint32_t order = 0);
    void noteOn(uint8_t slot, uint32_t offset, const PlayNote &n, double onRun, int32_t lenTicks,
                uint32_t chaseMbeats = 0xFFFFFFFFu);
    void releaseAll(bool hard);
    // ABI 1.2 note chase: for slots whose plugin asks for it, end their held notes and start the
    // notes sounding at song tick t (begun before it) with how much of them has passed
    bool chaseSlot(uint8_t slot) const;
    void chase(int32_t t, double segPos);
    void chasePattern(const PlayPattern &p, int32_t local, int32_t base, int32_t endLocal, int32_t minStart,
                      uint32_t seed, uint32_t offset);
    void emitMidi(uint8_t slot, const PlayMidi &, uint32_t offset, bool replay = false);
    bool chaseMidi(int32_t tick, uint32_t offset); // bounded replay; clock waits until complete
    uint32_t midiChaseClip_ = 0, midiChaseSeq_ = 0, midiChaseEvent_ = 0;
    bool midiChaseActive_ = false;
    void sequence(uint32_t frames);
    // notes of `p` with local start in [lo, hi); local tick L sounds at song tick base + L
    // (segPos: the segment's start in samples from the block start; fractional)
    void emitPattern(const PlayPattern &p, int32_t lo, int32_t hi, double segT0, double tps, double segPos,
                     double base, int32_t endLocal, uint32_t seed);
    void emitSong(double t0, double t1, double tps, double segPos, double run0);
    void automate(double tick);
    void metronome(double t0, double t1, double tps, double segPos, bool countIn);
    void noteOffs(double run0, double run1, double tps, double segPos);
    void processBlock(float *outL, float *outR, uint32_t frames);
    void diag(float *L, float *R, uint32_t frames);
    void click(float *L, float *R, uint32_t frames);
    void publish(uint32_t frames, uint32_t cycles);
    int32_t regionStart() const;
    int32_t regionEnd() const;

    Config cfg_;
    // Payloads are touched per event/block; queue indices stay in the internal Engine object.
    SpscQueue<EngineCmd, 256, true> cmds_;
    SpscQueue<Retired, 256, true> retired_;
    SpscQueue<RecEvent, 256, true> recorded_;
    bool record_ = false;
    EngineStatus st_;
    Buffers *bufs_ = nullptr;
    EventBuffers *eventBufs_ = nullptr;
    uint32_t nEvents_ = 0;
    uint32_t pendingFence_ = 0;

    const PlayModel *play_ = nullptr;
    const MixModel *mix_ = nullptr;
    Slot slots_[kEngineSlots];
    Track tracks_[kEngineTracks];
    ActiveNote active_[kMaxActiveNotes];
    int32_t nextVoiceId_ = 1;

    // transport
    bool playing_ = false, paused_ = false;
    PlayMode mode_ = PlayMode::Pattern;
    double tick_ = 0;      // position in the pattern / song
    double run_ = 0;       // monotonic ticks since play started (note-offs live here)
    double bpm_ = 130.0;
    TimeSig ts_;
    uint32_t pattern_ = 0;
    int32_t queued_ = -1;
    bool loopOn_ = false;
    int32_t loopStart_ = 0, loopEnd_ = 0;
    bool metro_ = false;
    float metroVol_ = 0.5f;
    int countInBars_ = 0;
    double countIn_ = 0;   // ticks of count-in left (0 = none)
    uint32_t loopCount_ = 0;
    bool chase_ = false;   // the position jumped / playback started: chase at the next segment
    double lastAutoTick_ = -1;
    float autoLast_[64];

    // click / diag generators
    struct PendingClick {
        uint32_t offset;
        bool accent;
    };
    PendingClick clicks_[8];
    uint32_t nClicks_ = 0;
    uint16_t start_[kTargets + 1];
    float clickPhase_ = 0, clickEnv_ = 0, clickInc_ = 0;
    Diag diag_ = Diag::Off;
    uint64_t diagPos_ = 0;
    float diagPhase_ = 0;
    bool masterSafety_ = true;

    // stats
    uint64_t samples_ = 0;
    uint32_t statCycles_ = 0, statBlocks_ = 0, statMaxUs_ = 0, statFrames_ = 0;
    int32_t load_ = 0;           // smoothed render load, x1000 of the block period
    bool guardTripped_ = false;  // the CPU guard is skipping note-ons
};

}  // namespace daw
