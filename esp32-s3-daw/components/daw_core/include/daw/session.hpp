// SPDX-License-Identifier: GPL-3.0-or-later
// The control plane: owns the project document, validates and applies the protocol's operations,
// keeps the engine in sync (compiled snapshots, plugin instances, levels) and tells every
// connected client what changed. Single-threaded: on the ESP32-S3 it runs in the control task
// (core 0); on Linux in the server's event loop. Transports (HTTP / WebSocket servers) only
// move bytes: handleText() in, the `send` callback out. docs/PROTOCOL.md describes the messages.
#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "daw/engine.hpp"
#include "daw/audio_file.hpp"
#include "daw/midi.hpp"
#include "daw/model.hpp"
#include "daw/preset_library.hpp"
#include "daw/sample_store.hpp"
#include "daw/sf2_info.hpp"
#include "daw/store.hpp"

struct cJSON;

namespace daw {

class PluginInstance;

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxMessageBytes = 256 * 1024;

enum class MsgClass : uint8_t {
    State,  // must arrive (acks, patches): a transport that cannot queue one marks the client for resync
    Live,   // status / meters: droppable, only the newest matters
};

class Session {
public:
    struct Config {
        std::string deviceName = "esp32-s3-daw";
        std::string firmware = "0.1.0";
        std::string target = "host";      // "esp32s3", "qemu", "host"
        ProjectStore *store = nullptr;     // project slots (slot 0 = autosave), may be null
        SampleStore *samples = nullptr;    // the sample library (mounted), may be null
        PresetLibrary *presets = nullptr;  // user presets (mounted), may be null
        uint32_t autosaveMs = 10000;       // after the last change, only while stopped
        std::function<void(cJSON *diag)> platformDiag;  // adds platform fields (heap, underruns, ...)
        std::function<uint32_t()> unixTime;             // for slot timestamps, may be null
        // ESP32-S3: erasing / writing flash stalls code running from flash on both cores, i.e. the
        // audio task. Then saving and deleting projects are refused while the transport runs.
        bool flashWritesStallAudio = false;
        // CPU admission: new instruments and effects (and re-enabled effects) are refused while the
        // measured 1 s render load is above this share of the deadline, x1000. 0 = off (QEMU).
        uint32_t cpuAdmitPermille = 0;
        // A live platform yields to the audio task during stopped project replacement.
        // Offline owners leave this empty; tests can advance a render block themselves.
        std::function<void()> waitForAudio;
    };
    using SendFn = std::function<void(int client, const std::string &text, MsgClass cls)>;  // client -1 = all

    Session(Engine &engine, const Config &cfg);
    ~Session();

    // start with a project (instantiates plugins, compiles, configures the engine)
    void start(Project p);
    // boot: the autosave slot if it holds a valid project, else the demo project
    void boot();

    void setSender(SendFn fn) { send_ = std::move(fn); }
    void clientConnected(int id);
    void clientDisconnected(int id);
    void handleText(int client, const char *data, size_t len);
    void resync(int client);  // the transport dropped a State message: send a full snapshot

    void tick(uint32_t nowMs);  // call every ~20 ms: retired objects, status, meters, autosave

    // HTTP helpers (export / import / status)
    std::string exportProject() const;
    bool importProject(const char *text, size_t len, std::string *err);
    std::string statusJson();
    // The sample library (HTTP): add / replace a file; channels using that name reload it.
    // reclaimModules opts module opens into keeping only this song (WAV / SF2 stay).
    bool importSample(const std::string &name, const uint8_t *data, size_t size, std::string *err, bool reclaimModules = false);
    // a SoundFont uploaded in pieces straight into the library (larger than free RAM)
    bool importSampleBegin(const std::string &name, size_t size, const uint8_t *head, size_t len, std::string *err, bool reclaimModules = false);
    bool importSampleAppend(const uint8_t *data, size_t len, std::string *err);
    bool importSampleFinish(std::string *err);
    void importSampleAbort();
    std::string samplesJson();
    const Project &project() const { return proj_; }
    uint32_t revision() const { return rev_; }
    bool idle() const;  // no clients and transport stopped (OTA, flash work)
    void stopForMaintenance();  // stop transport, flush autosave

    // stats for tests
    uint32_t compiles() const { return compiles_; }

private:
    struct Client {
        int id;
        bool hello = false;
        std::vector<std::pair<uint8_t, uint8_t>> held;  // live notes (slot, key) to release on disconnect
        MidiParser midi;
    };
    struct Reply {
        bool ok = true;
        std::string code, msg;
        cJSON *result = nullptr;  // extra fields for the ack
        void fail(const char *c, const std::string &m)
        {
            ok = false;
            code = c;
            msg = m;
        }
    };
    using Handler = void (Session::*)(int client, const cJSON *msg, Reply &r);

    void dispatch(int client, const std::string &op, const cJSON *msg, Reply &r);
    Client *client(int id);
    void sendTo(int client, cJSON *msg, MsgClass cls = MsgClass::State);
    void broadcast(cJSON *msg, MsgClass cls = MsgClass::State);
    void patch(const char *path, cJSON *value);
    void changed();  // a mutation: new revision, autosave pending
    cJSON *snapshot();
    cJSON *welcome(int client);

    // engine sync
    bool post(const EngineCmd &c);
    void flushCommands();
    void recompilePlay();
    bool recompileMix(std::string *err);
    void pushTransportConfig();
    void pushLevels();
    void pushChannelLevel(const Channel &c, bool anySolo);
    void pushTrack(const MixerTrack &t, bool anySolo);
    void pushFxChain(int track);
    int slotOf(Id channel) const;
    int allocSlot(Id channel);
    PluginInstance *instantiate(PluginState &st, std::string *err);
    void installChannel(Channel &c);
    void removeChannelInstance(int slot);
    void teardown();
    void drainRetired();
    uint32_t fenceSerial_ = 0;
    bool resolveTarget(const AutomationTarget &t, PlayAuto &a);
    void selectPattern(Id id, bool queue);
    void liveNote(int client, Id channel, uint8_t key, float vel, bool on);
    void midiIn(int client, Id channel, const MidiMsg &m, double ts);
    void autosave(uint32_t nowMs, bool force);
    bool flashWriteAllowed(Reply &r);
    bool cpuAdmits(Reply &r, const char *what);
    cJSON *projectList();
    cJSON *sampleList();
    void broadcastSamples();
    void sampleStored(const std::string &name);  // channels using the name reload it; clients hear of it
    bool prepareModuleOpen(const std::string &name, uint32_t size, std::string *err);
    std::shared_ptr<const LoadedResource> loadResource(const std::string &name, bool asAudio, std::string *err, const PluginState *state = nullptr);
    std::shared_ptr<const LoadedResource> loadSoundFont(const std::string &name, const PluginState &state, std::string *err);
    bool prepareFontAutomation(const AutomationTarget &target, Reply &reply);
    void applyResources(PluginInstance *inst, PluginState &st);
    bool presetValues(PluginInstance *inst, const std::string &name, std::vector<float> &vals);
    void broadcastPresets();
    void drainRecorded(bool closeAll);

    // operations (docs/PROTOCOL.md)
    void opHello(int, const cJSON *, Reply &);
    void opPing(int, const cJSON *, Reply &);
    void opTransport(int, const cJSON *, Reply &);
    void opBpm(int, const cJSON *, Reply &);
    void opTimeSig(int, const cJSON *, Reply &);
    void opMode(int, const cJSON *, Reply &);
    void opSeek(int, const cJSON *, Reply &);
    void opLoop(int, const cJSON *, Reply &);
    void opMetronome(int, const cJSON *, Reply &);
    void opPatternAdd(int, const cJSON *, Reply &);
    void opPatternDup(int, const cJSON *, Reply &);
    void opPatternDel(int, const cJSON *, Reply &);
    void opPatternSet(int, const cJSON *, Reply &);
    void opPatternSelect(int, const cJSON *, Reply &);
    void opPatternMove(int, const cJSON *, Reply &);
    void opStepsSet(int, const cJSON *, Reply &);
    void opStepToggle(int, const cJSON *, Reply &);
    void opSeqMode(int, const cJSON *, Reply &);
    void opNotesAdd(int, const cJSON *, Reply &);
    void opNotesUpdate(int, const cJSON *, Reply &);
    void opNotesDel(int, const cJSON *, Reply &);
    bool prepareMidiFont(Id, const std::vector<MidiEvent> &, Reply &);
    void opMidiSet(int, const cJSON *, Reply &);
    void opNotesSet(int, const cJSON *, Reply &);
    void opChannelAdd(int, const cJSON *, Reply &);
    void opChannelDel(int, const cJSON *, Reply &);
    void opChannelSet(int, const cJSON *, Reply &);
    void opChannelMove(int, const cJSON *, Reply &);
    void opChannelPlugin(int, const cJSON *, Reply &);
    void opChannelParam(int, const cJSON *, Reply &);
    void opChannelPreset(int, const cJSON *, Reply &);
    void opTrackSet(int, const cJSON *, Reply &);
    void opTrackSend(int, const cJSON *, Reply &);
    void opFxAdd(int, const cJSON *, Reply &);
    void opFxDel(int, const cJSON *, Reply &);
    void opFxMove(int, const cJSON *, Reply &);
    void opFxSet(int, const cJSON *, Reply &);
    void opFxParam(int, const cJSON *, Reply &);
    void opFxPreset(int, const cJSON *, Reply &);
    void opClipAdd(int, const cJSON *, Reply &);
    void opClipUpdate(int, const cJSON *, Reply &);
    void opClipDel(int, const cJSON *, Reply &);
    void opClipSplit(int, const cJSON *, Reply &);
    void opClipsSet(int, const cJSON *, Reply &);
    void opPTrackSet(int, const cJSON *, Reply &);
    void opMarker(int, const cJSON *, Reply &);
    void opAutoAdd(int, const cJSON *, Reply &);
    void opAutoSet(int, const cJSON *, Reply &);
    void opAutoDel(int, const cJSON *, Reply &);
    void opNote(int, const cJSON *, Reply &);
    void opMidi(int, const cJSON *, Reply &);
    void opPanic(int, const cJSON *, Reply &);
    void opProjectNew(int, const cJSON *, Reply &);
    void opProjectSave(int, const cJSON *, Reply &);
    void opProjectLoad(int, const cJSON *, Reply &);
    void opProjectList(int, const cJSON *, Reply &);
    void opProjectDelete(int, const cJSON *, Reply &);
    void opProjectRename(int, const cJSON *, Reply &);
    void opDiagTone(int, const cJSON *, Reply &);
    void opChannelResource(int, const cJSON *, Reply &);
    void opSamplesList(int, const cJSON *, Reply &);
    void opSamplesDelete(int, const cJSON *, Reply &);
    void opSamplesKeepModule(int, const cJSON *, Reply &);
    void opSamplesPresets(int, const cJSON *, Reply &);
    void opSamplesProbe(int, const cJSON *, Reply &);
    void opPresetSave(int, const cJSON *, Reply &);
    void opPresetDelete(int, const cJSON *, Reply &);
    void opRecord(int, const cJSON *, Reply &);

    Engine &eng_;
    Config cfg_;
    SendFn send_;
    Project proj_;
    uint32_t rev_ = 1;
    std::vector<Client> clients_;
    Id slotChannel_[kEngineSlots] = {};
    PluginInstance *chanInst_[kEngineSlots] = {};
    PluginInstance *fxInst_[kEngineTracks][kEngineFx] = {};
    uint32_t playVersion_ = 0;
    uint32_t compiles_ = 0;
    int32_t nextLiveId_ = -1000;
    bool dirty_ = false;
    uint32_t lastChangeMs_ = 0, nowMs_ = 0, lastStatusMs_ = 0, lastDiagMs_ = 0;
    std::vector<EngineCmd> backlog_;  // commands the full queue refused, retried in tick()
    ClockFollower clock_;
    std::map<std::string, std::weak_ptr<const LoadedResource>> resCache_;
    // Free file buffers survive project replacement; referenced ones are never overwritten.
    std::vector<std::shared_ptr<LoadedResource::FileBuffer>> fileBuffers_;
    std::map<std::string, std::vector<Sf2Preset>> sf2Selections_;
    std::vector<std::string> sf2FullFonts_;
    std::vector<Id> sf2FullChannels_;
    // MIDI recording: armed?, the input grid (ticks, 0 = as played), notes held down so far
    bool record_ = false;
    int32_t recQuantize_ = 0;
    struct HeldRec {
        uint8_t slot, key;
        uint32_t pattern;
        int32_t start;
        float vel;
    };
    std::vector<HeldRec> recHeld_;
    uint32_t recCloseAt_ = 0;  // when recording or the transport stopped: close held notes then (ms)  // decoded samples shared by instances
    bool extClock_ = false;
    uint32_t lastExtTempoMs_ = 0;
};

}  // namespace daw
