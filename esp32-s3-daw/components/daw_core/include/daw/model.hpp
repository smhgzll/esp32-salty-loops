// SPDX-License-Identifier: GPL-3.0-or-later
// The project document: the authoritative musical state, owned and edited by the control thread
// only (daw::Session). The audio thread never reads it; it plays compiled snapshots (engine.hpp).
#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "daw/music.hpp"

namespace daw {

using Id = uint32_t;

// limits (data model; the engine's real-time capacities are in engine.hpp)
constexpr int kMaxChannels = 32;          // channel rack rows (engine slots)
constexpr int kMaxMixerTracks = 17;       // master (0) + 16 inserts
constexpr int kMaxFxPerTrack = 4;
constexpr int kMaxSendsPerTrack = 4;
constexpr int kMaxPatterns = 256;
constexpr int kMaxSteps = 128;
constexpr int kMaxNotesPerSeq = 4096;
// the longest pattern, note and clip, and the latest start (1000 bars of 4/4: a whole tracker song
// of 30 minutes at 133 BPM is one pattern holding one note)
constexpr int32_t kMaxSpanTicks = kPPQ * 4 * 1000;
constexpr int kMaxClips = 2048;
constexpr int kMaxPlaylistTracks = 64;
constexpr int kMaxAutomation = 64;
constexpr int kMaxAutoPoints = 1024;
constexpr int kStepTicks = kPPQ / 4;      // a step is a 1/16 note

struct PluginState {
    std::string pluginId;  // "" = empty slot
    std::string version;
    std::vector<std::pair<std::string, float>> params;  // parameter key -> plain value
    std::string blob;      // base64 opaque state (DAW_CAP_STATE_BLOB)
    std::string preset;    // name of the last loaded preset (information)
    bool missing = false;  // plugin not available in this firmware: kept verbatim, not played
    std::string rawJson;   // a missing plugin's original instance JSON, written back unchanged
    // ABI 1.1 resources: slot key -> sample library name. A name the library does not have is kept
    // (the slot stays empty and the plugin silent) and listed in missingResources (runtime only).
    std::vector<std::pair<std::string, std::string>> resources;
    std::vector<std::string> missingResources;
    std::string loadError;  // runtime-only instantiation failure, shown in the plugin editor
    const std::string *resource(const std::string &slot) const;

    float *param(const std::string &key);
    void setParam(const std::string &key, float v);
};

// MIDI 1.0 messages, including opaque SysEx/system/meta bytes. Kept separately from editable
// notes; order is the original file order at equal ticks (0 means ordinary DAW ordering).
constexpr size_t kMaxMidiEvents = 65536;
constexpr size_t kMaxMidiBytes = 512 * 1024;
struct MidiEvent {
    int32_t tick = 0;
    uint32_t order = 0;
    std::vector<uint8_t> data;
};

struct Note {
    Id id = 0;
    int32_t start = 0;  // ticks from the pattern start
    int32_t len = kStepTicks;
    uint8_t key = 60;
    float vel = 0.78f;  // 0..1
    float pan = 0.0f;   // -1..1 (DAW_EXPR_PAN)
    float pitch = 0.0f; // fine pitch, semitones -1..1 (DAW_EXPR_PITCH)
    float rel = 0.5f;   // release velocity
    float prob = 1.0f;  // probability 0..1
    bool slide = false;
    bool accent = false; // from an accented step (DAW_NOTE_FLAG_ACCENT)
    uint32_t midiOnOrder = 0, midiOffOrder = 0; // MIDI imports only; legacy notes keep their order
};

struct Step {
    bool on = false;
    float vel = 0.78f;
    float pan = 0.0f;
    uint8_t key = 0;     // 0 = the channel's root key
    bool accent = false;
    float prob = 1.0f;
};

// one channel's content inside one pattern
struct ChannelSeq {
    Id channel = 0;
    uint16_t stepCount = 16;   // 1..128, independent per channel (polymeter)
    std::vector<Step> steps;   // stepCount entries
    std::vector<Note> notes;   // piano roll notes, sorted by start
    bool pianoRoll = false;    // which of the two plays
    Id nextNoteId = 1;
    std::vector<MidiEvent> midi; // timed non-note messages, sorted by tick / order

    void resizeSteps(int n);
    void sortNotes();
};

struct Pattern {
    Id id = 0;
    std::string name;
    uint32_t color = 0x5f8fff;
    int32_t lengthTicks = 0;   // 0 = automatic (content rounded up to whole bars)
    float swing = 0.0f;        // 0..1: delays every second step by up to half a step
    std::vector<ChannelSeq> seqs;

    ChannelSeq *find(Id channel);
    const ChannelSeq *find(Id channel) const;
    ChannelSeq &get(Id channel);  // creates an empty sequence
};

struct Channel {
    Id id = 0;
    std::string name;
    uint32_t color = 0xff9a3c;
    std::string icon;
    PluginState inst;
    float vol = 0.78f;   // 0..1.25 linear
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    int track = 0;       // mixer track
    uint8_t root = 60;   // key of a plain step
};

struct FxSlot {
    Id id = 0;
    PluginState inst;
    bool enabled = true;
    float mix = 1.0f;
};

struct Send {
    int target = 0;
    float level = 0.0f;
};

struct MixerTrack {
    int index = 0;
    std::string name;
    uint32_t color = 0x8a8f98;
    float vol = 0.8f;    // 0..1.25 linear
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    int route = 0;       // destination track (inserts only; master goes to the output)
    std::vector<Send> sends;
    std::vector<FxSlot> fx;
};

struct PlaylistTrack {
    std::string name;
    uint32_t color = 0x6b7280;
    bool mute = false;
    bool solo = false;
};

enum class ClipKind : uint8_t { Pattern = 0, Automation = 1, Audio = 2 };

struct Clip {
    Id id = 0;
    ClipKind kind = ClipKind::Pattern;
    int track = 0;
    int32_t start = 0;
    int32_t len = 0;
    int32_t offset = 0;  // where inside the pattern / automation the clip starts
    Id ref = 0;          // pattern / automation / sample id
    bool muted = false;
};

struct AutomationTarget {
    enum Kind : uint8_t { None, ChannelVol, ChannelPan, ChannelParam, TrackVol, TrackPan, FxParam, FxMix, SendLevel, Tempo };
    Kind kind = None;
    Id channel = 0;
    int track = 0;
    Id fx = 0;
    int send = 0;
    std::string param;  // plugin parameter key
};

struct AutoPoint {
    int32_t tick = 0;
    float value = 0.5f;  // normalised 0..1 over the target's range
    float tension = 0.0f;
};

struct Automation {
    Id id = 0;
    std::string name;
    uint32_t color = 0x40c4aa;
    AutomationTarget target;
    std::vector<AutoPoint> points;  // sorted by tick
    int32_t lengthTicks = kPPQ * 16;
    float valueAt(int32_t tick) const;
};

struct Marker {
    Id id = 0;
    int32_t tick = 0;
    std::string name;
};

struct SampleRef {
    Id id = 0;
    std::string name;
    std::string key;  // storage key
    uint32_t frames = 0;
    bool missing = false;
};

struct Project {
    static constexpr int kSchema = 2;
    int schema = kSchema;
    std::string name = "Untitled";
    std::string author;
    double bpm = 130.0;
    TimeSig ts;
    std::vector<Channel> channels;
    std::vector<Pattern> patterns;
    std::vector<MixerTrack> mixer;      // index 0 = master
    std::vector<PlaylistTrack> tracks;
    std::vector<Clip> clips;
    std::vector<Marker> markers;
    std::vector<Automation> automation;
    std::vector<SampleRef> samples;
    bool loopOn = false;
    int32_t loopStart = 0, loopEnd = 0;  // song loop range (ticks)
    bool metronome = false;
    int countInBars = 0;
    Id currentPattern = 0;
    Id nextId = 1;

    Id newId() { return nextId++; }
    Channel *channel(Id id);
    Pattern *pattern(Id id);
    const Pattern *pattern(Id id) const;
    Automation *automationById(Id id);
    Clip *clip(Id id);
    FxSlot *fx(Id id, int *trackOut = nullptr);
    int channelIndex(Id id) const;
    int patternIndex(Id id) const;
    int32_t patternLength(const Pattern &p) const;
    int32_t songLength() const;  // end of the last clip, at least one bar
    void ensureMixer();           // master + 16 inserts
    void normalise();             // repair indices / limits after loading or editing
};

// steps -> the notes they play (one per enabled step and loop repetition is done by the engine)
void stepsToNotes(const ChannelSeq &seq, uint8_t root, float swing, std::vector<Note> &out);

// the demo project created by "New"
Project defaultProject();
Project emptyProject();

uint32_t crc32(const void *data, size_t len, uint32_t crc = 0);

}  // namespace daw
