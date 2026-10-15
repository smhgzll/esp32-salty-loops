// SPDX-License-Identifier: MIT
// esp32-daw-plugin-sdk: native Linux test host. Instantiates plugins through the C ABI exactly
// as the DAW does (allocation through the host API, events with sample offsets), and adds the
// checks a plugin developer needs: allocation accounting, leak detection, timing, WAV output.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "daw_plugin_api.h"

namespace dawhost {

// all plugins linked into this executable (generated registry)
const std::vector<const daw_plugin_descriptor_t *> &registry();
const daw_plugin_descriptor_t *find(const std::string &idOrShortName);

// host services with allocation accounting
struct HostServices {
    daw_host_api_t api{};
    size_t liveBlocks = 0, liveBytes = 0, peakBytes = 0;
    size_t fastBytes = 0, largeBytes = 0;  // currently allocated per class
    size_t allocCalls = 0;
    bool allocAllowed = true;              // false while process() runs: an allocation is a bug
    size_t rtViolations = 0;
    uint32_t reports = 0;
    int32_t lastReport = 0;
    std::vector<std::string> logs;
    HostServices();
};

struct Block {
    std::vector<float> l, r;
};

// ABI 1.1: resources every new Instance gets before activation (validation, renders); null: none
using ResourceProvider = const daw_resource_t *(*)(const daw_resource_slot_desc_t &slot);
void setDefaultResources(ResourceProvider p);

class Instance {
public:
    Instance(const daw_plugin_descriptor_t *d, uint32_t sampleRate = 44100, uint32_t maxBlock = 256);
    ~Instance();
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;

    bool ok() const { return inst_ != nullptr; }
    const daw_plugin_descriptor_t *desc() const { return d_; }
    HostServices &host() { return host_; }

    void setParam(uint32_t id, float v);
    float getParam(uint32_t id) const;
    bool loadPreset(uint32_t index);
    // ABI 1.1: deactivate, set_resource, activate. `res` must outlive the instance (or the next set)
    int32_t setResource(uint32_t slot, const daw_resource_t *res);

    // render `frames` (<= maxBlock) frames with these events; input used by effects
    void process(uint32_t frames, const std::vector<daw_event_t> &events, float *outL, float *outR,
                 const float *inL = nullptr, const float *inR = nullptr);
    uint64_t samplePos() const { return pos_; }
    uint32_t voices() const { return inst_ && d_->active_voices ? d_->active_voices(inst_) : 0; }
    uint32_t sampleRate() const { return sr_; }
    uint32_t maxBlock() const { return maxBlock_; }
    // the transport tempo process() reports from now on (default 120); the song position
    // continues from where it is
    void setTempo(double bpm);

private:
    const daw_plugin_descriptor_t *d_;
    daw_plugin_instance_t *inst_ = nullptr;
    HostServices host_;
    uint32_t sr_, maxBlock_;
    uint64_t pos_ = 0;
    double tempo_ = 120.0, beatsBase_ = 0;
    uint64_t posBase_ = 0;
};

// a note / param timeline in seconds, rendered block by block
struct TimedEvent {
    double time;
    daw_event_t ev;
};
daw_event_t noteOn(uint8_t key, float vel, int32_t id = -1, uint16_t flags = 0);
daw_event_t noteOff(uint8_t key, int32_t id = -1);
daw_event_t paramEv(uint32_t id, float v);

struct RenderResult {
    std::vector<float> l, r;
    double processSeconds = 0;  // wall time spent in process()
    uint32_t blocks = 0;
    double maxBlockSeconds = 0;
};
RenderResult render(Instance &inst, std::vector<TimedEvent> events, double seconds, uint32_t block = 256,
                    const std::vector<float> *inL = nullptr, const std::vector<float> *inR = nullptr);

// decoded audio file (PCM 8/16/24/32-bit, IEEE float 32), as a daw_resource_t of type AUDIO
struct AudioFile {
    std::vector<float> ch[2];
    uint32_t channels = 0, rate = 0;
    int32_t rootKey = -1;
    uint32_t loopStart = 0, loopEnd = 0;
    std::string name;
    const float *ptr[2] = {nullptr, nullptr};
    daw_resource_t res{};
    const daw_resource_t *resource();  // valid while this object lives and is not modified
};
bool readWav(const std::string &path, AudioFile &out, std::string *err = nullptr);
bool decodeWav(const uint8_t *data, size_t size, AudioFile &out, std::string *err = nullptr);
// raw file bytes as a daw_resource_t of type FILE
struct FileResource {
    std::vector<uint8_t> bytes;
    std::string name;
    daw_resource_t res{};
    const daw_resource_t *resource();
};
bool readFile(const std::string &path, FileResource &out);
// a minimal SoundFont 2 for tests: one preset (bank 0, program 0, "Test Sine") -> one instrument ->
// one looped 16-bit sine sample of `freq` Hz recorded at `rootKey`
// filterCents > 0: the instrument zone sets initialFilterFc (absolute cents), so voices run the low-pass
std::vector<uint8_t> makeTestSoundFont(double freq = 261.6256, int rootKey = 60, uint32_t rate = 44100, double seconds = 0.5, int filterCents = 0);
// a minimal tracker module ("mod", "xm", "s3m" or "it"), the same song in each: 4 channels, one
// looped 32-frame saw, order 0 = pattern 0 (speed 6, 125 BPM, 64 rows: 7.68 s), order 1 =
// pattern 1 (150 BPM, a break after row 32: 3.3 s); channel 0 plays every 4 rows, channel 1
// every 8 rows from row 2, channels 2 / 3 only carry the speed / tempo / break effects.
// "it-dense": an IT of 16 channels playing four different samples at once (3.84 s, see modgen.cpp)
std::vector<uint8_t> makeTestModule(const std::string &format = "mod");

bool writeWav(const std::string &path, const std::vector<float> &l, const std::vector<float> &r, uint32_t sr);
uint64_t hashPcm(const std::vector<float> &l, const std::vector<float> &r);  // 24-bit quantised FNV-1a
double rms(const std::vector<float> &x);
float peak(const std::vector<float> &x);

// test sequences
std::vector<TimedEvent> sequenceScale(uint8_t root, int notes, double step, double gate, float vel);
std::vector<TimedEvent> sequenceChords(uint8_t root, int chords, double len);
std::vector<TimedEvent> sequenceDrums(int bars, double bpm);

std::string descriptorJson(const daw_plugin_descriptor_t *d, const char *arch);

}  // namespace dawhost
