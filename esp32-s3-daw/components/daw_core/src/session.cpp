// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/session.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "cJSON.h"
#include "daw/compile.hpp"
#include "daw/platform.hpp"
#include "daw/plugin_host.hpp"
#include "daw/project_json.hpp"
#include "daw/sf2_info.hpp"
#include "daw/sf2_subset.hpp"

namespace daw {

static const char *TAG = "session";

namespace {

const cJSON *item(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
bool has(const cJSON *o, const char *k) { return item(o, k) != nullptr; }
double num(const cJSON *o, const char *k, double def)
{
    const cJSON *v = item(o, k);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : def;
}
bool flag(const cJSON *o, const char *k, bool def)
{
    const cJSON *v = item(o, k);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    return def;
}
std::string str(const cJSON *o, const char *k, const char *def = "")
{
    const cJSON *v = item(o, k);
    return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string(def);
}
std::string name64(std::string s)
{
    if (s.size() > 64)
        s.resize(64);
    for (auto &c : s)
        if ((unsigned char)c < 0x20)
            c = ' ';
    return s;
}
float clampf(double v, float lo, float hi) { return (float)std::clamp(v, (double)lo, (double)hi); }

cJSON *patternListJson(const Project &p)
{
    cJSON *a = cJSON_CreateArray();
    for (const auto &pat : p.patterns) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", pat.id);
        cJSON_AddStringToObject(o, "name", pat.name.c_str());
        cJSON_AddStringToObject(o, "color", colorString(pat.color).c_str());
        cJSON_AddNumberToObject(o, "length", pat.lengthTicks);
        cJSON_AddNumberToObject(o, "effLength", p.patternLength(pat));
        cJSON_AddNumberToObject(o, "swing", round(pat.swing * 10000.0) / 10000.0);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

// Control-thread preset discovery also handles reset packets split across SMF F0/F7 events.
bool midiHasReset(const std::vector<MidiEvent> &events)
{
    uint8_t packet[256]; size_t used = 0; bool overflow = false;
    for (const auto &e : events) {
        if (e.data.empty() || (e.data[0] != 0xf0 && e.data[0] != 0xf7)) continue;
        if (e.data[0] == 0xf0) { used = 0; overflow = false; }
        for (size_t i = e.data[0] == 0xf7 ? 1 : 0; i < e.data.size(); ++i) {
            const uint8_t b = e.data[i]; if (b >= 0xf8) continue;
            if (used < sizeof packet) packet[used++] = b; else overflow = true;
            if (b != 0xf7) continue;
            if (!overflow && used >= 6 && packet[0] == 0xf0) {
                bool valid = true;
                for (size_t j = 1; j + 1 < used; ++j) valid &= packet[j] < 128;
                const bool gm = used == 6 && packet[1] == 0x7e && packet[3] == 9 && (packet[4] == 1 || packet[4] == 3);
                const bool xg = used == 9 && packet[1] == 0x43 && (packet[2] & 0xf0) == 0x10 && packet[3] == 0x4c &&
                    packet[4] == 0 && packet[5] == 0 && (packet[6] == 0x7e || packet[6] == 0x7f) && packet[7] == 0;
                const bool gs = used == 11 && packet[1] == 0x41 && packet[3] == 0x42 && packet[4] == 0x12 &&
                    packet[5] == 0x40 && packet[6] == 0 && packet[7] == 0x7f && packet[8] == 0 && packet[9] == 0x41;
                if (valid && (gm || xg || gs)) return true;
            }
            used = 0; overflow = false;
        }
    }
    return false;
}

}  // namespace

// ---- life cycle ---------------------------------------------------------------------------

static bool pluginKnown(const std::string &id) { return PluginRegistry::find(id) != nullptr; }

Session::Session(Engine &engine, const Config &cfg) : eng_(engine), cfg_(cfg)
{
    setPluginKnownCheck(pluginKnown);
    fenceSerial_ = eng_.status().fence.load(std::memory_order_relaxed);
}

Session::~Session()
{
    teardown();
    // the engine is stopped by now (the owner stops the audio task first): reclaim everything
    drainRetired();
}

void Session::teardown()
{
    for (int s = 0; s < kEngineSlots; s++) {
        if (chanInst_[s])
            post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)s, 0, 0, 0, {.p = nullptr}});
        chanInst_[s] = nullptr;
        slotChannel_[s] = 0;
    }
    for (int t = 0; t < kEngineTracks; t++) {
        bool any = false;
        for (int f = 0; f < kEngineFx; f++) {
            any |= fxInst_[t][f] != nullptr;
            fxInst_[t][f] = nullptr;
        }
        if (any) {
            auto *m = new FxChainMsg{};
            post(EngineCmd{EngineCmd::SetFxChain, (uint8_t)t, 0, 0, 0, {.p = m}});
        }
    }
}

void Session::start(Project p)
{
    sf2Selections_.clear();
    sf2FullFonts_.clear();
    sf2FullChannels_.clear();
    bool hadInstances = false;
    for (auto *inst : chanInst_) hadInstances |= inst != nullptr;
    for (auto &track : fxInst_)
        for (auto *inst : track) hadInstances |= inst != nullptr;
    post(EngineCmd{EngineCmd::Stop});
    post(EngineCmd{EngineCmd::NotesOff, 1});
    teardown();
    post(EngineCmd{EngineCmd::ResetLoad});
    // First boot can precede the audio task; there is no previous load to discard then.
    if (cfg_.waitForAudio && (hadInstances || eng_.status().blocks.load(std::memory_order_relaxed) != 0)) {
        // Old and new projects must not occupy internal RAM at the same time.
        // Acknowledge after rendering, so fades, worker joins, retirements and
        // the load-window reset are done before new editing requests arrive.
        const uint32_t fence = ++fenceSerial_;
        EngineCmd cmd{EngineCmd::Fence}; cmd.u32 = fence; post(cmd);
        const uint64_t deadline = platform::micros() + 500000;
        while (eng_.status().fence.load(std::memory_order_acquire) != fence) {
            flushCommands();
            cfg_.waitForAudio();
            drainRetired();
            if (platform::micros() >= deadline) {
                DAW_LOGW(TAG, "audio task did not finish project teardown within 500 ms");
                break;
            }
        }
        drainRetired();
    }
    proj_ = std::move(p);
    proj_.normalise();
    for (auto &c : proj_.channels) {
        allocSlot(c.id);
        installChannel(c);
    }
    for (auto &t : proj_.mixer) {
        for (size_t i = 0; i < t.fx.size() && i < (size_t)kEngineFx; i++) {
            std::string err;
            fxInst_[t.index][i] = t.fx[i].inst.missing ? nullptr : instantiate(t.fx[i].inst, &err);
            if (!err.empty())
                DAW_LOGW(TAG, "fx %u: %s", (unsigned)t.fx[i].id, err.c_str());
        }
        if (!t.fx.empty()) pushFxChain(t.index);
    }
    std::string err;
    if (!recompileMix(&err)) {  // a stored project with a routing cycle: route everything to master
        DAW_LOGW(TAG, "%s: routing reset", err.c_str());
        for (auto &t : proj_.mixer) {
            t.route = 0;
            t.sends.clear();
        }
        recompileMix(nullptr);
    }
    recompilePlay();
    pushTransportConfig();
    pushLevels();
    post(EngineCmd{EngineCmd::SetPattern, 0, 0, 0, (uint32_t)std::max(0, proj_.patternIndex(proj_.currentPattern))});
    rev_++;
    dirty_ = false;
    for (auto &c : clients_)
        if (c.hello)
            sendTo(c.id, welcome(c.id));
    DAW_LOGI(TAG, "project \"%s\": %u channels, %u patterns, %u clips", proj_.name.c_str(), (unsigned)proj_.channels.size(),
             (unsigned)proj_.patterns.size(), (unsigned)proj_.clips.size());
}

void Session::boot()
{
    if (cfg_.store) {
        std::string text, err;
        SlotInfo si;
        if (cfg_.store->load(0, text, &si, &err)) {
            Project p;
            if (projectFromText(text.data(), text.size(), p, &err)) {
                DAW_LOGI(TAG, "autosave restored (%u bytes)", (unsigned)text.size());
                start(std::move(p));
                return;
            }
            DAW_LOGW(TAG, "autosave unreadable: %s", err.c_str());
        }
    }
    start(defaultProject());
}

bool Session::post(const EngineCmd &c)
{
    if (backlog_.empty() && eng_.post(c))
        return true;
    if (backlog_.size() > 4096) {  // the audio thread is not running: drop live controls only
        DAW_LOGE(TAG, "engine command backlog overflow");
        return false;
    }
    backlog_.push_back(c);
    return false;
}

void Session::flushCommands()
{
    size_t i = 0;
    for (; i < backlog_.size(); ++i)
        if (!eng_.post(backlog_[i])) break;
    backlog_.erase(backlog_.begin(), backlog_.begin() + (long)i);
}

// MIDI recording: turn the notes the engine played (with their pattern ticks) into piano-roll notes
// of the pattern being played. closeAll: recording stops, notes still held end at the pattern's end.
void Session::drainRecorded(bool closeAll)
{
    RecEvent e;
    std::vector<uint32_t> touched;
    auto addNote = [&](const HeldRec &h, int32_t end) {
        if (h.pattern >= proj_.patterns.size() || !slotChannel_[h.slot])
            return;
        Pattern &p = proj_.patterns[h.pattern];
        Channel *ch = proj_.channel(slotChannel_[h.slot]);
        if (!ch)
            return;
        const int32_t plen = proj_.patternLength(p);
        ChannelSeq &s = p.get(ch->id);
        if (!s.pianoRoll) {  // a step row becomes notes first, so recording adds to what it played
            std::vector<Note> notes;
            stepsToNotes(s, ch->root, p.swing, notes);
            for (auto &n : notes) {
                n.id = s.nextNoteId++;
                s.notes.push_back(n);
            }
            s.pianoRoll = true;
        }
        if (s.notes.size() >= (size_t)kMaxNotesPerSeq)
            return;
        Note n;
        n.id = s.nextNoteId++;
        n.key = h.key;
        n.vel = std::clamp(h.vel, 0.05f, 1.0f);
        int32_t start = h.start;
        if (recQuantize_ > 0)
            start = (int32_t)lround((double)start / recQuantize_) * recQuantize_;
        if (start >= plen)
            start -= plen;  // quantized past the end: the loop's first beat
        int32_t len = end >= h.start ? end - h.start : plen - h.start + end;  // held across the loop end
        n.start = std::max<int32_t>(0, start);
        n.len = std::clamp<int32_t>(len, 30, kPPQ * 4 * 16);
        s.notes.push_back(n);
        s.sortNotes();
        if (std::find(touched.begin(), touched.end(), h.pattern) == touched.end())
            touched.push_back(h.pattern);
    };
    while (eng_.popRecorded(e)) {
        if (e.on) {
            recHeld_.push_back({e.slot, e.key, e.pattern, e.tick, e.vel});
            continue;
        }
        for (size_t i = 0; i < recHeld_.size(); i++)
            if (recHeld_[i].slot == e.slot && recHeld_[i].key == e.key) {
                addNote(recHeld_[i], e.tick);
                recHeld_.erase(recHeld_.begin() + (long)i);
                break;
            }
    }
    if (closeAll) {
        for (const auto &h : recHeld_)
            if (h.pattern < proj_.patterns.size())
                addNote(h, proj_.patternLength(proj_.patterns[h.pattern]));
        recHeld_.clear();
    }
    if (touched.empty())
        return;
    changed();
    recompilePlay();
    for (uint32_t pi : touched)
        patch("pattern", patternJson(proj_.patterns[pi], proj_.patternLength(proj_.patterns[pi])));
}

void Session::opRecord(int, const cJSON *m, Reply &r)
{
    if (has(m, "quantize"))
        recQuantize_ = std::clamp<int32_t>((int32_t)num(m, "quantize", 0), 0, kPPQ * 4);
    if (has(m, "on")) {
        const bool on = flag(m, "on", false);
        if (!on && record_)
            recCloseAt_ = nowMs_ + 150;
        record_ = on;
        post(EngineCmd{EngineCmd::SetRecord, (uint8_t)(on ? 1 : 0)});
    }
    r.result = cJSON_CreateObject();
    cJSON_AddBoolToObject(r.result, "on", record_);
    cJSON_AddNumberToObject(r.result, "quantize", recQuantize_);
}

void Session::drainRetired()
{
    Retired r;
    while (eng_.popRetired(r)) {
        switch (r.kind) {
        case Retired::Instance: PluginInstance::destroy(static_cast<PluginInstance *>(r.p)); break;
        case Retired::Play: freePlay(static_cast<const PlayModel *>(r.p)); break;
        case Retired::Mix: freeMix(static_cast<const MixModel *>(r.p)); break;
        case Retired::Chain: delete static_cast<FxChainMsg *>(r.p); break;
        }
    }
}

int Session::slotOf(Id channel) const
{
    for (int s = 0; s < kEngineSlots; s++)
        if (slotChannel_[s] == channel && channel)
            return s;
    return -1;
}

int Session::allocSlot(Id channel)
{
    int s = slotOf(channel);
    if (s >= 0)
        return s;
    for (s = 0; s < kEngineSlots; s++)
        if (!slotChannel_[s]) {
            slotChannel_[s] = channel;
            return s;
        }
    return -1;
}

PluginInstance *Session::instantiate(PluginState &st, std::string *err)
{
    st.loadError.clear();
    const auto failed = [&](const std::string &why) -> PluginInstance * {
        st.loadError = why;
        if (err) *err = why;
        return nullptr;
    };
    if (st.pluginId.empty())
        return nullptr;
    const daw_plugin_descriptor_t *d = PluginRegistry::find(st.pluginId);
    if (!d) {
        st.missing = true;
        if (err)
            *err = "plugin " + st.pluginId + " is not in this firmware";
        return nullptr;
    }
    platform::MemInfo mi = platform::memInfo();
    if (mi.psramTotal && mi.psramFree < d->mem_large_bytes + (2u << 20)) {
        fileBuffers_.erase(std::remove_if(fileBuffers_.begin(), fileBuffers_.end(),
                                         [](const auto &b) { return b.use_count() == 1; }), fileBuffers_.end());
        mi = platform::memInfo();
    }
    if (mi.internalTotal && mi.internalFree < d->mem_fast_bytes + 32 * 1024) {
        return failed("not enough internal RAM for " + st.pluginId);
    }
    // PSRAM also holds the project, protocol messages and the documents written from it: with too
    // little left the snapshot and the autosave fail (a 23-part GM song of SoundFont channels,
    // each with its own copy of the font's index, used up all but 1 MB)
    constexpr size_t kPsramReserve = 2u << 20;
    if (mi.psramTotal && mi.psramFree < d->mem_large_bytes + kPsramReserve) {
        return failed("not enough memory for another " + std::string(d->name) + " (" + std::to_string(mi.psramFree / 1024) +
                      " KB of PSRAM free): remove some channels");
    }
    std::string createError;
    PluginInstance *inst = PluginInstance::create(d, eng_.config().sampleRate, eng_.config().blockFrames, &createError);
    if (!inst)
        return failed(createError.empty() ? st.pluginId + ": create failed" : createError);
    inst->applyState(st);
    applyResources(inst, st);
    if (!inst->activate()) {
        PluginInstance::destroy(inst);
        return failed(st.pluginId + ": activate failed");
    }
    inst->captureState(st);
    return inst;
}

// ---- resources (ABI 1.1) ------------------------------------------------------------------------

std::shared_ptr<const LoadedResource> Session::loadResource(const std::string &name, bool asAudio, std::string *err, const PluginState *state)
{
    if (!asAudio && state && state->pluginId == "org.smhgzll.soundfont" && cfg_.samples) {
        const auto *info = cfg_.samples->find(name);
        if (info && info->type == SampleType::SoundFont)
            return loadSoundFont(name, *state, err);
    }
    const std::string key = (asAudio ? "a:" : "f:") + name;
    if (auto hit = resCache_[key].lock())
        return hit;
    if (!cfg_.samples) {
        if (err)
            *err = "no sample library on this target";
        return nullptr;
    }
    std::shared_ptr<const LoadedResource> r;
    if (asAudio) {
        std::vector<uint8_t> bytes;
        if (!cfg_.samples->read(name, bytes, err))
            return nullptr;
        DecodedAudio a;
        if (!decodeWav(bytes.data(), bytes.size(), a, err))
            return nullptr;
        r = LoadedResource::audio(name, std::move(a));
    } else {
        const SampleInfo *info = cfg_.samples->find(name);
        if (!info)
            return nullptr;
        const uint32_t size = info->size;
        std::shared_ptr<LoadedResource::FileBuffer> buffer;
        for (const auto &b : fileBuffers_)
            if (b.use_count() == 1 && b->capacity >= size && (!buffer || b->capacity < buffer->capacity))
                buffer = b;
        if (buffer) {
            if (!cfg_.samples->readInto(name, buffer->data.get(), buffer->capacity, err))
                return nullptr;
            r = LoadedResource::file(name, buffer, size);
            resCache_[key] = r;
            return r;
        }
        // No suitable spare: release unused buffers before allocating. For nearby font sizes,
        // reserve the larger one already in the library (at most 1 MiB extra). TimGM -> MT-32
        // then reuses the same allocation instead of needing a new 6.3 MB hole in a fragmented heap.
        fileBuffers_.erase(std::remove_if(fileBuffers_.begin(), fileBuffers_.end(),
                                         [](const auto &b) { return b.use_count() == 1; }), fileBuffers_.end());
        uint32_t capacity = size;
        if (info->type == SampleType::SoundFont)
            for (const auto &f : cfg_.samples->list())
                if (f.type == SampleType::SoundFont && f.size > capacity && f.size - size <= (1u << 20))
                    capacity = f.size;
        LoadedResource::FileBytes bytes{nullptr, platform::free};
        const uint64_t deadline = platform::micros() + 500000;
        for (;;) {
            const auto mi = platform::memInfo();
            if (!mi.psramTotal || mi.psramFree >= (size_t)capacity + (2u << 20))
                bytes.reset(static_cast<uint8_t *>(platform::alloc(capacity, 16, platform::Mem::Large)));
            if (!bytes && capacity != size) {
                // Headroom is optional; never reject a font merely because the larger spare
                // does not fit. A later larger font can still fail with a resources reply.
                capacity = size;
                if (!mi.psramTotal || mi.psramFree >= (size_t)size + (2u << 20))
                    bytes.reset(static_cast<uint8_t *>(platform::alloc(size, 16, platform::Mem::Large)));
            }
            if (bytes)
                break;
            // Old snapshots / retired play models can still occupy a large hole immediately
            // after project.new. Let the audio and HTTP tasks release them before refusing.
            if (!cfg_.waitForAudio || platform::micros() >= deadline) {
                if (err)
                    *err = "not enough contiguous PSRAM for " + name + " (" + std::to_string(size / 1024) +
                           " KB needed, " + std::to_string(mi.psramLargest / 1024) + " KB largest block, " +
                           std::to_string(mi.psramFree / 1024) + " KB free)";
                return nullptr;
            }
            cfg_.waitForAudio();
            drainRetired();
        }
        if (!cfg_.samples->readInto(name, bytes.get(), size, err))
            return nullptr;
        buffer = std::make_shared<LoadedResource::FileBuffer>(LoadedResource::FileBuffer{std::move(bytes), capacity});
        fileBuffers_.push_back(buffer);
        r = LoadedResource::file(name, buffer, size);
    }
    resCache_[key] = r;
    return r;
}

std::shared_ptr<const LoadedResource> Session::loadSoundFont(const std::string &name, const PluginState &state, std::string *err)
{
    bool full = std::find(sf2FullFonts_.begin(), sf2FullFonts_.end(), name) != sf2FullFonts_.end();
    for (const auto &a : proj_.automation) if (a.target.kind == AutomationTarget::ChannelParam &&
                                                (a.target.param == "bank" || a.target.param == "program")) {
        const auto *c = proj_.channel(a.target.channel);
        const auto *font = c ? c->inst.resource("soundfont") : nullptr;
        full |= font && *font == name;
    }
    if (full) return loadResource(name, false, err);
    const SampleInfo *info = cfg_.samples->find(name);
    if (!info) return nullptr;
    const uint32_t seq = info->seq, sourceSize = info->size;
    auto &selection = sf2Selections_[name];
    auto add = [&](const PluginState &st) {
        Sf2Preset p;
        for (const auto &kv : st.params) {
            if (kv.first == "bank") p.bank = (uint16_t)std::clamp((int)lroundf(kv.second), 0, 16383);
            if (kv.first == "program") p.program = (uint16_t)std::clamp((int)lroundf(kv.second), 0, 127);
        }
        selection.push_back(p);
    };
    add(state);
    for (const auto &c : proj_.channels) {
        const auto *font = c.inst.resource("soundfont");
        if (c.inst.pluginId != "org.smhgzll.soundfont" || !font || *font != name) continue;
        add(c.inst);
        for (const auto &pat : proj_.patterns) for (const auto &q : pat.seqs) if (q.channel == c.id) {
            int bank = 0;
            for (const auto &kv : c.inst.params) if (kv.first == "bank") bank = (int)kv.second;
            int msb = bank >> 7, lsb = bank & 127;
            if (midiHasReset(q.midi)) selection.push_back({"", (uint16_t)(bank == 128 ? 128 : 0), 0});
            for (const auto &e : q.midi) {
                if (e.data.size() < 2) continue;
                const auto hi = e.data[0] & 0xf0;
                if (hi == 0xb0 && e.data.size() == 3) {
                    if (e.data[1] == 0) msb = e.data[2];
                    else if (e.data[1] == 32) lsb = e.data[2];
                } else if (hi == 0xc0) {
                    const bool drums = (e.data[0] & 15) == 9 || bank == 128;
                    selection.push_back({"", (uint16_t)(drums ? 128 : msb * 128 + lsb), e.data[1]});
                    selection.push_back({"", (uint16_t)(drums ? 128 : 0), e.data[1]});
                }
            }
        }
    }
    std::sort(selection.begin(), selection.end(), [](const auto &a, const auto &b) { return a.bank < b.bank || (a.bank == b.bank && a.program < b.program); });
    selection.erase(std::unique(selection.begin(), selection.end(), [](const auto &a, const auto &b) { return a.bank == b.bank && a.program == b.program; }), selection.end());
    std::string key = "sf:" + name + ":" + std::to_string(seq);
    for (const auto &p : selection) key += ":" + std::to_string(p.bank) + "," + std::to_string(p.program);
    if (auto hit = resCache_[key].lock()) return hit;
    if (!cfg_.samples->verify(name, err)) return nullptr;
    Sf2Reader reader = [&](uint32_t off, void *buf, uint32_t len) { return cfg_.samples->readRange(name, off, buf, len); };
    Sf2Subset subset;
    if (!sf2Subset(reader, sourceSize, selection, subset, err)) return nullptr;
    const uint32_t size = subset.bytes();
    std::shared_ptr<LoadedResource::FileBuffer> buffer;
    // An old whole-font spare must not keep several megabytes reserved after switching to
    // selective loading. Modest spares still let project changes reuse allocations.
    for (const auto &b : fileBuffers_)
        if (b.use_count() == 1 && b->capacity >= size && b->capacity-size <= (1u << 20) && (!buffer || b->capacity < buffer->capacity)) buffer = b;
    fileBuffers_.erase(std::remove_if(fileBuffers_.begin(), fileBuffers_.end(),
                                     [&](const auto &b) { return b != buffer && b.use_count() == 1; }), fileBuffers_.end());
    if (!buffer) {
        const auto mi = platform::memInfo();
        LoadedResource::FileBytes bytes{nullptr, platform::free};
        if (!mi.psramTotal || mi.psramFree >= (size_t)size + (2u << 20))
            bytes.reset(static_cast<uint8_t *>(platform::alloc(size, 16, platform::Mem::Large)));
        if (!bytes) {
            if (err) *err = "not enough PSRAM for selected SoundFont samples (" + std::to_string(size/1024) + " KB needed)";
            return nullptr;
        }
        buffer = std::make_shared<LoadedResource::FileBuffer>(LoadedResource::FileBuffer{std::move(bytes), size});
        fileBuffers_.push_back(buffer);
    }
    if (!subset.copy(reader, buffer->data.get(), buffer->capacity, err)) return nullptr;
    auto resource = LoadedResource::file(name, buffer, size);
    resCache_[key] = resource;
    DAW_LOGI(TAG, "%s: %u presets, %u -> %u B, PCM %u B", name.c_str(), (unsigned)selection.size(), (unsigned)sourceSize, (unsigned)size, (unsigned)subset.pcmBytes);
    return resource;
}

// hand the instance the files its state names (before activate); unknown names stay listed as missing
void Session::applyResources(PluginInstance *inst, PluginState &st)
{
    st.missingResources.clear();
    const daw_plugin_descriptor_t *d = inst->desc();
    for (const auto &kv : st.resources) {
        const int slot = inst->resourceSlot(kv.first);
        if (slot < 0)
            continue;  // a slot this plugin version does not have: kept in the project, ignored
        const SampleInfo *info = cfg_.samples ? cfg_.samples->find(kv.second) : nullptr;
        const uint32_t types = d->resource_slots[slot].types;
        const bool audio = info && info->type == SampleType::Wav && (types & DAW_RES_MASK(DAW_RES_AUDIO));
        std::string err;
        auto res = info && (audio || (types & DAW_RES_MASK(DAW_RES_FILE))) ? loadResource(kv.second, audio, &err, &st) : nullptr;
        if (!res || !inst->setResource(slot, res)) {
            st.missingResources.push_back(kv.second);
            DAW_LOGW(TAG, "%s: resource %s unavailable %s", st.pluginId.c_str(), kv.second.c_str(), err.c_str());
        }
    }
}

cJSON *Session::sampleList()
{
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "list");
    if (cfg_.samples) {
        for (const auto &f : cfg_.samples->list()) {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "name", f.name.c_str());
            cJSON_AddNumberToObject(j, "size", f.size);
            cJSON_AddNumberToObject(j, "crc", f.crc);
            cJSON_AddStringToObject(j, "type", sampleTypeName(f.type));
            cJSON_AddItemToArray(a, j);
        }
        cJSON_AddNumberToObject(o, "total", cfg_.samples->totalBytes());
        cJSON_AddNumberToObject(o, "free", cfg_.samples->freeBytes());
        cJSON_AddNumberToObject(o, "largest", cfg_.samples->largestFree());
    }
    return o;
}

std::string Session::samplesJson()
{
    cJSON *o = sampleList();
    std::string t = jsonText(o, false);
    cJSON_Delete(o);
    return t;
}

void Session::broadcastSamples()
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "samples");
    cJSON_AddItemToObject(m, "library", sampleList());
    broadcast(m);
}

bool Session::prepareModuleOpen(const std::string &name, uint32_t size, std::string *err)
{
    if (cfg_.samples->putting()) {
        if (err) *err = "another sample upload is in progress";
        return false;
    }
    // Module opening keeps just the selected song, including when the library has room.
    // Saved / current projects do not pin older module files: the user can reimport them.
    std::vector<std::string> removeNames;
    for (const auto &f : cfg_.samples->list())
        if (f.type == SampleType::Module && f.name != name) removeNames.push_back(f.name);
    bool changed = false;
    auto remove = [&](const std::string &file) {
        if (!cfg_.samples->remove(file)) {
            if (err) *err = "flash write failed while clearing old modules";
            if (changed) broadcastSamples();
            return false;
        }
        resCache_.erase("f:" + file);
        changed = true;
        return true;
    };
    for (const auto &file : removeNames)
        if (!remove(file)) return false;
    // A changed file with the same name normally stages alongside its old copy. Modules are
    // disposable: if that staging copy blocks the replacement, clear it as well. WAV/SF2 stay.
    const SampleInfo *old = cfg_.samples->find(name);
    if (size && cfg_.samples->largestFree() < size && old && old->type == SampleType::Module && !remove(name))
        return false;
    if (changed) broadcastSamples();
    return true;
}

bool Session::importSample(const std::string &rawName, const uint8_t *data, size_t size, std::string *err, bool reclaimModules)
{
    if (!cfg_.samples) {
        if (err)
            *err = "no sample library on this target";
        return false;
    }
    if (cfg_.flashWritesStallAudio && eng_.status().playing.load() != 0) {
        if (err)
            *err = "stop playback first: writing flash would interrupt the audio on this device";
        return false;
    }
    if (size > cfg_.samples->totalBytes()) {
        if (err)
            *err = "file larger than the sample library";
        return false;
    }
    const SampleType type = sniffSample(data, size);
    if (type == SampleType::Wav) {  // refuse what the sampler could not play
        DecodedAudio a;
        if (!decodeWav(data, size, a, err))
            return false;
    }
    const std::string name = sanitizeSampleName(rawName);
    if (reclaimModules && type == SampleType::Module && !name.empty() && size &&
        !prepareModuleOpen(name, (uint32_t)size, err))
        return false;
    if (!cfg_.samples->put(name, data, (uint32_t)size, err))
        return false;
    sampleStored(name);
    return true;
}

bool Session::importSampleBegin(const std::string &rawName, size_t size, const uint8_t *head, size_t len, std::string *err, bool reclaimModules)
{
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        return false;
    };
    if (!cfg_.samples)
        return fail("no sample library on this target");
    if (cfg_.flashWritesStallAudio && eng_.status().playing.load() != 0)
        return fail("stop playback first: writing flash would interrupt the audio on this device");
    if (size > cfg_.samples->totalBytes())
        return fail("file larger than the sample library");
    const SampleType type = sniffSample(head, len);
    if (type != SampleType::SoundFont && type != SampleType::Module)  // WAV files are decoded and checked whole
        return fail("only SoundFont (sf2) and tracker module files are uploaded in pieces");
    const std::string name = sanitizeSampleName(rawName);
    if (reclaimModules && type == SampleType::Module && !name.empty() && size && len <= size &&
        !prepareModuleOpen(name, (uint32_t)size, err))
        return false;
    return cfg_.samples->beginPut(rawName, (uint32_t)size, head, (uint32_t)len, err);
}

bool Session::importSampleAppend(const uint8_t *data, size_t len, std::string *err)
{
    if (!cfg_.samples)
        return false;
    if (cfg_.flashWritesStallAudio && eng_.status().playing.load() != 0) {
        cfg_.samples->abortPut();
        if (err)
            *err = "playback started: the upload stopped (writing flash would interrupt the audio)";
        return false;
    }
    return cfg_.samples->appendPut(data, (uint32_t)len, err);
}

bool Session::importSampleFinish(std::string *err)
{
    std::string name;
    if (!cfg_.samples || !cfg_.samples->finishPut(&name, err))
        return false;
    sampleStored(name);
    return true;
}

void Session::importSampleAbort()
{
    if (cfg_.samples)
        cfg_.samples->abortPut();
}

void Session::sampleStored(const std::string &name)
{
    resCache_.erase("a:" + name);
    resCache_.erase("f:" + name);
    // channels that use this name pick up the new file
    bool any = false;
    for (auto &c : proj_.channels)
        if (!c.inst.missing)
            for (const auto &kv : c.inst.resources)
                if (kv.second == name) {
                    installChannel(c);
                    any = true;
                    break;
                }
    if (any)
        patch("channels", channelsJson(proj_));
    broadcastSamples();
}

void Session::opSamplesList(int, const cJSON *, Reply &r)
{
    r.result = cJSON_CreateObject();
    cJSON_AddItemToObject(r.result, "library", sampleList());
}

void Session::opSamplesKeepModule(int, const cJSON *m, Reply &r)
{
    const std::string name = str(m, "name");
    const SampleInfo *f = cfg_.samples ? cfg_.samples->find(name) : nullptr;
    if (!f || f->type != SampleType::Module) {
        r.fail("not_found", "no module '" + name + "' in the sample library");
        return;
    }
    if (!flashWriteAllowed(r)) return;
    std::string err;
    if (!prepareModuleOpen(name, 0, &err)) {
        r.fail("storage", err);
        return;
    }
    r.result = cJSON_CreateObject();
    cJSON_AddItemToObject(r.result, "library", sampleList());
}

// the presets of a SoundFont in the library (for the SoundFont player's preset menu)
void Session::opSamplesPresets(int, const cJSON *m, Reply &r)
{
    const std::string name = str(m, "name");
    const SampleInfo *f = cfg_.samples ? cfg_.samples->find(name) : nullptr;
    if (!f || f->type != SampleType::SoundFont) {
        r.fail("not_found", "no SoundFont '" + name + "' in the sample library");
        return;
    }
    std::vector<Sf2Preset> list;
    std::string err;
    SampleStore *store = cfg_.samples;
    if (!sf2Presets([&](uint32_t off, void *buf, uint32_t len) { return store->readRange(name, off, buf, len); }, f->size, list, &err)) {
        r.fail("bad_file", name + ": " + err);
        return;
    }
    r.result = cJSON_CreateObject();
    cJSON_AddStringToObject(r.result, "name", name.c_str());
    cJSON *a = cJSON_AddArrayToObject(r.result, "presets");
    for (const auto &p : list) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name.c_str());
        cJSON_AddNumberToObject(o, "bank", p.bank);
        cJSON_AddNumberToObject(o, "program", p.program);
        cJSON_AddItemToArray(a, o);
    }
}

// What a plugin reads from a library file: its read-only parameters after loading it (the
// Tracker plugin: a module's length, channels, suggested tempo, ...). The instance is temporary
// and never reaches the engine; the file's buffer stays cached for the channel that follows.
void Session::opSamplesProbe(int, const cJSON *m, Reply &r)
{
    const std::string name = str(m, "name"), id = str(m, "plugin");
    const SampleInfo *f = cfg_.samples ? cfg_.samples->find(name) : nullptr;
    if (!f) {
        r.fail("not_found", "no file '" + name + "' in the sample library");
        return;
    }
    const daw_plugin_descriptor_t *d = PluginRegistry::find(id);
    if (!d || !daw_plugin_has_resources(d)) {
        r.fail("bad_plugin", "no plugin '" + id + "' with resource slots");
        return;
    }
    const bool wav = f->type == SampleType::Wav;
    int slot = -1;
    bool audio = false;
    for (uint32_t i = 0; i < d->resource_slot_count && slot < 0; i++) {
        const uint32_t types = d->resource_slots[i].types;
        if (wav && (types & DAW_RES_MASK(DAW_RES_AUDIO)))
            slot = (int)i, audio = true;
        else if (types & DAW_RES_MASK(DAW_RES_FILE))
            slot = (int)i;
    }
    if (slot < 0) {
        r.fail("bad_plugin", std::string(d->name) + " takes no such file");
        return;
    }
    std::string err;
    PluginInstance *inst = PluginInstance::create(d, eng_.config().sampleRate, eng_.config().blockFrames, &err);
    if (!inst) {
        r.fail("resources", err.empty() ? "the plugin could not start" : err);
        return;
    }
    auto res = loadResource(name, audio, &err);
    const bool ok = res && inst->setResource(slot, res);
    if (ok) {
        r.result = cJSON_CreateObject();
        cJSON_AddStringToObject(r.result, "name", name.c_str());
        cJSON_AddStringToObject(r.result, "plugin", d->id);
        cJSON *v = cJSON_AddObjectToObject(r.result, "values");
        for (uint32_t i = 0; i < d->param_count; i++)
            if (d->params[i].flags & DAW_PARAM_READONLY)
                cJSON_AddNumberToObject(v, d->params[i].key, d->get_param(inst->handle(), d->params[i].id));
    }
    PluginInstance::destroy(inst);
    if (!ok)
        r.fail(res ? "bad_file" : "resources", res ? name + ": " + d->name + " cannot read this file" : err);
}

void Session::opSamplesDelete(int, const cJSON *m, Reply &r)
{
    const std::string name = str(m, "name");
    if (!flashWriteAllowed(r))
        return;
    if (!cfg_.samples || !cfg_.samples->remove(name)) {
        r.fail("not_found", "no sample '" + name + "'");
        return;
    }
    // playing instances keep their decoded copy; the next instantiation reports it missing
    resCache_.erase("a:" + name);
    resCache_.erase("f:" + name);
    broadcastSamples();
}

void Session::installChannel(Channel &c)
{
    const int slot = allocSlot(c.id);
    if (slot < 0)
        return;
    std::string err;
    PluginInstance *inst = c.inst.missing ? nullptr : instantiate(c.inst, &err);
    if (!err.empty())
        DAW_LOGW(TAG, "channel %s: %s", c.name.c_str(), err.c_str());
    chanInst_[slot] = inst;
    post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = inst}});
}

void Session::removeChannelInstance(int slot)
{
    if (slot < 0)
        return;
    post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = nullptr}});
    chanInst_[slot] = nullptr;
    slotChannel_[slot] = 0;
}

void Session::pushFxChain(int track)
{
    if (track < 0 || track >= kEngineTracks)
        return;
    auto *m = new FxChainMsg{};
    const MixerTrack &t = proj_.mixer[(size_t)track];
    for (int i = 0; i < kEngineFx; i++) {
        m->fx[i] = i < (int)t.fx.size() ? fxInst_[track][i] : nullptr;
        m->enabled[i] = i < (int)t.fx.size() ? t.fx[(size_t)i].enabled : false;
        m->mix[i] = i < (int)t.fx.size() ? t.fx[(size_t)i].mix : 0.0f;
    }
    post(EngineCmd{EngineCmd::SetFxChain, (uint8_t)track, 0, 0, 0, {.p = m}});
}

bool Session::resolveTarget(const AutomationTarget &t, PlayAuto &a)
{
    a.scale = DAW_SCALE_LINEAR;
    a.fx = 0;
    a.paramId = 0;
    auto param = [&](PluginInstance *inst) {
        if (!inst)
            return false;
        const int i = inst->paramIndex(t.param);
        if (i < 0)
            return false;
        const daw_param_desc_t &d = inst->desc()->params[i];
        a.paramId = d.id;
        a.min = d.min;
        a.max = d.max;
        a.scale = (uint8_t)d.scale;
        return true;
    };
    switch (t.kind) {
    case AutomationTarget::ChannelVol:
    case AutomationTarget::ChannelPan:
    case AutomationTarget::ChannelParam: {
        const int s = slotOf(t.channel);
        if (s < 0)
            return false;
        a.slot = (uint8_t)s;
        if (t.kind == AutomationTarget::ChannelVol) {
            a.kind = AT_CHAN_VOL;
            a.min = 0;
            a.max = 1.25f;
            return true;
        }
        if (t.kind == AutomationTarget::ChannelPan) {
            a.kind = AT_CHAN_PAN;
            a.min = -1;
            a.max = 1;
            return true;
        }
        a.kind = AT_CHAN_PARAM;
        return param(chanInst_[s]);
    }
    case AutomationTarget::TrackVol:
    case AutomationTarget::TrackPan:
    case AutomationTarget::SendLevel:
        if (t.track < 0 || t.track >= kEngineTracks)
            return false;
        a.slot = (uint8_t)t.track;
        a.kind = t.kind == AutomationTarget::TrackVol ? AT_TRACK_VOL : (t.kind == AutomationTarget::TrackPan ? AT_TRACK_PAN : AT_SEND);
        a.min = t.kind == AutomationTarget::TrackPan ? -1.0f : 0.0f;
        a.max = t.kind == AutomationTarget::TrackPan ? 1.0f : 1.25f;
        a.fx = (uint8_t)std::clamp(t.send, 0, kMaxSendsPerTrack - 1);
        return true;
    case AutomationTarget::FxParam:
    case AutomationTarget::FxMix: {
        int track = -1;
        FxSlot *f = proj_.fx(t.fx, &track);
        if (!f || track < 0)
            return false;
        const auto &v = proj_.mixer[(size_t)track].fx;
        const int idx = (int)(f - v.data());
        a.slot = (uint8_t)track;
        a.fx = (uint8_t)idx;
        if (t.kind == AutomationTarget::FxMix) {
            a.kind = AT_FX_MIX;
            a.min = 0;
            a.max = 1;
            return true;
        }
        a.kind = AT_FX_PARAM;
        return param(fxInst_[track][idx]);
    }
    case AutomationTarget::Tempo:
        a.kind = AT_TEMPO;
        a.min = (float)kMinBpm;
        a.max = (float)kMaxBpm;
        return true;
    default: return false;
    }
}

void Session::recompilePlay()
{
    PlayModel *m = compilePlay(
        proj_, [this](Id ch) { return slotOf(ch); }, [this](const AutomationTarget &t, PlayAuto &a) { return resolveTarget(t, a); },
        ++playVersion_);
    if (!m) {
        DAW_LOGE(TAG, "out of memory compiling the project");
        return;
    }
    compiles_++;
    post(EngineCmd{EngineCmd::SwapPlay, 0, 0, 0, 0, {.p = m}});
}

bool Session::recompileMix(std::string *err)
{
    MixModel *m = compileMix(proj_, err);
    if (!m)
        return false;
    post(EngineCmd{EngineCmd::SwapMix, 0, 0, 0, 0, {.p = m}});
    return true;
}

void Session::pushTransportConfig()
{
    EngineCmd b{EngineCmd::SetTempo};
    b.d = proj_.bpm;
    post(b);
    post(EngineCmd{EngineCmd::SetTimeSig, proj_.ts.num, proj_.ts.den});
    EngineCmd l{EngineCmd::SetLoop, (uint8_t)(proj_.loopOn ? 1 : 0)};
    l.i[0] = proj_.loopStart;
    l.i[1] = proj_.loopEnd;
    post(l);
    EngineCmd m{EngineCmd::SetMetronome, (uint8_t)(proj_.metronome ? 1 : 0)};
    m.f[0] = 0.6f;
    post(m);
    post(EngineCmd{EngineCmd::SetCountIn, (uint8_t)proj_.countInBars});
}

void Session::pushChannelLevel(const Channel &c, bool anySolo)
{
    const int s = slotOf(c.id);
    if (s < 0)
        return;
    EngineCmd e{EngineCmd::ChannelLevel, (uint8_t)s, (uint8_t)((c.mute || (anySolo && !c.solo)) ? 1 : 0), (uint8_t)c.track};
    e.f[0] = c.vol;
    e.f[1] = c.pan;
    post(e);
}

void Session::pushTrack(const MixerTrack &t, bool anySolo)
{
    EngineCmd e{EngineCmd::TrackLevel, (uint8_t)t.index, (uint8_t)((t.mute || (anySolo && t.index > 0 && !t.solo)) ? 1 : 0)};
    e.f[0] = t.vol;
    e.f[1] = t.pan;
    post(e);
    for (int s = 0; s < kMaxSendsPerTrack; s++) {
        EngineCmd sd{EngineCmd::SendLevel, (uint8_t)t.index, (uint8_t)s};
        sd.f[0] = s < (int)t.sends.size() ? t.sends[(size_t)s].level : 0.0f;
        post(sd);
    }
}

void Session::pushLevels()
{
    bool anySolo = false;
    for (const auto &c : proj_.channels)
        anySolo |= c.solo;
    for (const auto &c : proj_.channels)
        pushChannelLevel(c, anySolo);
    bool anyTrackSolo = false;
    for (const auto &t : proj_.mixer)
        anyTrackSolo |= t.index > 0 && t.solo;
    for (const auto &t : proj_.mixer)
        pushTrack(t, anyTrackSolo);
}

// ---- clients and messages -----------------------------------------------------------------

Session::Client *Session::client(int id)
{
    for (auto &c : clients_)
        if (c.id == id)
            return &c;
    return nullptr;
}

void Session::clientConnected(int id)
{
    if (!client(id)) {
        Client c;
        c.id = id;
        clients_.push_back(c);
    }
}

void Session::clientDisconnected(int id)
{
    Client *c = client(id);
    if (!c)
        return;
    // no stuck notes: release everything this client was holding
    for (auto &h : c->held) {
        EngineCmd e{EngineCmd::Note, h.first, h.second, 0};
        e.u32 = 0xFFFFFFFFu;  // match by key
        post(e);
    }
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [id](const Client &x) { return x.id == id; }), clients_.end());
}

void Session::sendTo(int client, cJSON *msg, MsgClass cls)
{
    if (send_)
        send_(client, jsonText(msg, false), cls);
    cJSON_Delete(msg);
}

void Session::broadcast(cJSON *msg, MsgClass cls)
{
    if (send_) {
        bool any = false;
        for (auto &c : clients_)
            any |= c.hello;
        if (any)
            send_(-1, jsonText(msg, false), cls);
    }
    cJSON_Delete(msg);
}

void Session::patch(const char *path, cJSON *value)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "patch");
    cJSON_AddNumberToObject(m, "rev", rev_);
    cJSON_AddStringToObject(m, "path", path);
    cJSON_AddItemToObject(m, "value", value);
    broadcast(m);
}

void Session::changed()
{
    rev_++;
    dirty_ = true;
    lastChangeMs_ = nowMs_;
}

cJSON *Session::snapshot()
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "meta", metaJson(proj_));
    cJSON_AddItemToObject(o, "channels", channelsJson(proj_));
    cJSON_AddItemToObject(o, "patterns", patternsJson(proj_));
    cJSON_AddItemToObject(o, "mixer", mixerJson(proj_));
    cJSON_AddItemToObject(o, "playlist", playlistJson(proj_));
    cJSON_AddItemToObject(o, "automation", automationJson(proj_));
    return o;
}

cJSON *Session::welcome(int client)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "welcome");
    cJSON_AddNumberToObject(m, "proto", kProtocolVersion);
    cJSON_AddNumberToObject(m, "client", client);
    cJSON_AddNumberToObject(m, "rev", rev_);
    cJSON *srv = cJSON_AddObjectToObject(m, "server");
    cJSON_AddStringToObject(srv, "name", cfg_.deviceName.c_str());
    cJSON_AddStringToObject(srv, "version", cfg_.firmware.c_str());
    cJSON_AddStringToObject(srv, "target", cfg_.target.c_str());
    cJSON_AddNumberToObject(srv, "sampleRate", eng_.config().sampleRate);
    cJSON_AddNumberToObject(srv, "blockFrames", eng_.config().blockFrames);
    cJSON_AddNumberToObject(srv, "ppq", kPPQ);
    cJSON *lim = cJSON_AddObjectToObject(srv, "limits");
    cJSON_AddNumberToObject(lim, "channels", kMaxChannels);
    cJSON_AddNumberToObject(lim, "patterns", kMaxPatterns);
    cJSON_AddNumberToObject(lim, "steps", kMaxSteps);
    cJSON_AddNumberToObject(lim, "notes", kMaxNotesPerSeq);
    cJSON_AddNumberToObject(lim, "midiEvents", kMaxMidiEvents);
    cJSON_AddNumberToObject(lim, "midiBytes", kMaxMidiBytes);
    cJSON_AddNumberToObject(lim, "mixerTracks", kMaxMixerTracks);
    cJSON_AddNumberToObject(lim, "fxPerTrack", kMaxFxPerTrack);
    cJSON_AddNumberToObject(lim, "sends", kMaxSendsPerTrack);
    cJSON_AddNumberToObject(lim, "clips", kMaxClips);
    cJSON_AddNumberToObject(lim, "projectSlots", cfg_.store ? cfg_.store->slots() - 1 : 0);
    cJSON_AddNumberToObject(lim, "projectBytes", cfg_.store ? cfg_.store->maxPayload() : 0);  // deflated
    cJSON_AddNumberToObject(lim, "projectDocBytes", cfg_.store ? ProjectStore::kMaxDocument : 0);
    cJSON_AddItemToObject(m, "project", snapshot());
    cJSON_AddItemToObject(m, "plugins", pluginCatalogJson());
    cJSON_AddItemToObject(m, "projects", projectList());
    cJSON_AddItemToObject(m, "samples", sampleList());
    cJSON_AddItemToObject(m, "userPresets", cfg_.presets ? cfg_.presets->json() : cJSON_CreateArray());
    return m;
}

void Session::resync(int client)
{
    if (Client *c = this->client(client); c && c->hello)
        sendTo(client, welcome(client));
}

void Session::handleText(int clientId, const char *data, size_t len)
{
    if (!client(clientId))
        clientConnected(clientId);
    Reply r;
    cJSON *msg = nullptr;
    double id = -1;
    std::string op;
    if (len > kMaxMessageBytes)
        r.fail("too_large", "message larger than " + std::to_string(kMaxMessageBytes) + " bytes");
    else if (!(msg = cJSON_ParseWithLength(data, len)) || !cJSON_IsObject(msg))
        r.fail("bad_json", "not a JSON object");
    else {
        op = str(msg, "t");
        id = num(msg, "id", -1);
        Client *c = client(clientId);
        if (op != "hello" && op != "ping" && c && !c->hello)
            r.fail("no_hello", "send hello first");
        else
            dispatch(clientId, op, msg, r);
    }
    if (id >= 0 || !r.ok) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "t", r.ok ? "ack" : "err");
        if (id >= 0)
            cJSON_AddNumberToObject(a, "id", id);
        cJSON_AddNumberToObject(a, "rev", rev_);
        if (!r.ok) {
            cJSON_AddStringToObject(a, "code", r.code.c_str());
            cJSON_AddStringToObject(a, "msg", r.msg.c_str());
            if (!op.empty())
                cJSON_AddStringToObject(a, "op", op.c_str());
        }
        if (r.result) {
            cJSON *f;
            cJSON_ArrayForEach(f, r.result) cJSON_AddItemReferenceToObject(a, f->string, f);
        }
        sendTo(clientId, a);
    }
    cJSON_Delete(r.result);
    cJSON_Delete(msg);
}

void Session::dispatch(int client, const std::string &op, const cJSON *m, Reply &r)
{
    static const struct {
        const char *name;
        Handler fn;
    } kOps[] = {
        {"hello", &Session::opHello},
        {"ping", &Session::opPing},
        {"play", &Session::opTransport},
        {"stop", &Session::opTransport},
        {"pause", &Session::opTransport},
        {"toggle", &Session::opTransport},
        {"bpm", &Session::opBpm},
        {"timesig", &Session::opTimeSig},
        {"mode", &Session::opMode},
        {"seek", &Session::opSeek},
        {"loop", &Session::opLoop},
        {"metronome", &Session::opMetronome},
        {"pattern.add", &Session::opPatternAdd},
        {"pattern.dup", &Session::opPatternDup},
        {"pattern.del", &Session::opPatternDel},
        {"pattern.set", &Session::opPatternSet},
        {"pattern.select", &Session::opPatternSelect},
        {"pattern.move", &Session::opPatternMove},
        {"steps.set", &Session::opStepsSet},
        {"step.toggle", &Session::opStepToggle},
        {"seq.mode", &Session::opSeqMode},
        {"notes.add", &Session::opNotesAdd},
        {"notes.update", &Session::opNotesUpdate},
        {"notes.del", &Session::opNotesDel},
        {"notes.set", &Session::opNotesSet},
        {"midi.set", &Session::opMidiSet},
        {"channel.add", &Session::opChannelAdd},
        {"channel.del", &Session::opChannelDel},
        {"channel.set", &Session::opChannelSet},
        {"channel.move", &Session::opChannelMove},
        {"channel.plugin", &Session::opChannelPlugin},
        {"channel.param", &Session::opChannelParam},
        {"channel.preset", &Session::opChannelPreset},
        {"track.set", &Session::opTrackSet},
        {"track.send", &Session::opTrackSend},
        {"fx.add", &Session::opFxAdd},
        {"fx.del", &Session::opFxDel},
        {"fx.move", &Session::opFxMove},
        {"fx.set", &Session::opFxSet},
        {"fx.param", &Session::opFxParam},
        {"fx.preset", &Session::opFxPreset},
        {"clip.add", &Session::opClipAdd},
        {"clip.update", &Session::opClipUpdate},
        {"clip.del", &Session::opClipDel},
        {"clip.split", &Session::opClipSplit},
        {"clips.set", &Session::opClipsSet},
        {"ptrack.set", &Session::opPTrackSet},
        {"marker", &Session::opMarker},
        {"auto.add", &Session::opAutoAdd},
        {"auto.set", &Session::opAutoSet},
        {"auto.del", &Session::opAutoDel},
        {"note", &Session::opNote},
        {"midi", &Session::opMidi},
        {"panic", &Session::opPanic},
        {"project.new", &Session::opProjectNew},
        {"project.save", &Session::opProjectSave},
        {"project.load", &Session::opProjectLoad},
        {"project.list", &Session::opProjectList},
        {"project.delete", &Session::opProjectDelete},
        {"project.rename", &Session::opProjectRename},
        {"diag.tone", &Session::opDiagTone},
        {"channel.resource", &Session::opChannelResource},
        {"samples.list", &Session::opSamplesList},
        {"samples.delete", &Session::opSamplesDelete},
        {"samples.keepModule", &Session::opSamplesKeepModule},
        {"samples.presets", &Session::opSamplesPresets},
        {"samples.probe", &Session::opSamplesProbe},
        {"preset.save", &Session::opPresetSave},
        {"preset.delete", &Session::opPresetDelete},
        {"record", &Session::opRecord},
    };
    for (const auto &o : kOps)
        if (op == o.name) {
            (this->*o.fn)(client, m, r);
            return;
        }
    r.fail("unknown_op", "unknown operation '" + op + "'");
}

// ---- lookups with errors --------------------------------------------------------------------

#define NEED_CHANNEL(var, key)                                        \
    Channel *var = proj_.channel((Id)num(m, key, 0));                 \
    if (!var) {                                                       \
        r.fail("not_found", std::string("no channel ") + key);        \
        return;                                                       \
    }
#define NEED_PATTERN(var, key)                                        \
    Pattern *var = proj_.pattern((Id)num(m, key, 0));                 \
    if (!var) {                                                       \
        r.fail("not_found", std::string("no pattern ") + key);        \
        return;                                                       \
    }

// ---- operations: session / transport ----------------------------------------------------------

void Session::opHello(int c, const cJSON *m, Reply &r)
{
    const int proto = (int)num(m, "proto", 0);
    if (proto != kProtocolVersion) {
        r.fail("proto", "protocol " + std::to_string(proto) + " not supported (server speaks " + std::to_string(kProtocolVersion) + ")");
        return;
    }
    Client *cl = client(c);
    cl->hello = true;
    sendTo(c, welcome(c));
}

void Session::opPing(int, const cJSON *m, Reply &r)
{
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "echo", num(m, "ts", 0));
    cJSON_AddNumberToObject(r.result, "uptimeMs", (double)(platform::micros() / 1000));
}

void Session::opTransport(int, const cJSON *m, Reply &)
{
    const std::string op = str(m, "t");
    const uint32_t st = eng_.status().playing.load();
    if (op == "play")
        post(EngineCmd{EngineCmd::Play});
    else if (op == "stop") {
        post(EngineCmd{EngineCmd::Stop});
        recCloseAt_ = nowMs_ + 150;  // then notes still held end at the pattern's end (the engine
    }                                // first plays the key releases already queued)
    else if (op == "pause")
        post(EngineCmd{EngineCmd::Pause});
    else if (op == "toggle")
        post(EngineCmd{(st == 1 || st == 3) ? EngineCmd::Pause : EngineCmd::Play});
}

void Session::opBpm(int c, const cJSON *m, Reply &r)
{
    if (!has(m, "bpm")) {
        r.fail("bad_arg", "bpm missing");
        return;
    }
    proj_.bpm = std::clamp(num(m, "bpm", proj_.bpm), kMinBpm, kMaxBpm);
    EngineCmd b{EngineCmd::SetTempo};
    b.d = proj_.bpm;
    post(b);
    changed();
    patch("meta", metaJson(proj_));
    (void)c;
}

void Session::opTimeSig(int, const cJSON *m, Reply &r)
{
    const int n = (int)num(m, "num", 4), d = (int)num(m, "den", 4);
    if (n < 1 || n > 16 || (d != 2 && d != 4 && d != 8 && d != 16)) {
        r.fail("bad_arg", "time signature must be 1..16 / 2, 4, 8 or 16");
        return;
    }
    proj_.ts.num = (uint8_t)n;
    proj_.ts.den = (uint8_t)d;
    post(EngineCmd{EngineCmd::SetTimeSig, (uint8_t)n, (uint8_t)d});
    changed();
    recompilePlay();  // automatic pattern lengths are whole bars
    patch("meta", metaJson(proj_));
    patch("patternList", patternListJson(proj_));
}

void Session::opMode(int, const cJSON *m, Reply &r)
{
    const std::string mode = str(m, "mode");
    if (mode != "pattern" && mode != "song") {
        r.fail("bad_arg", "mode is pattern or song");
        return;
    }
    post(EngineCmd{EngineCmd::SetMode, (uint8_t)(mode == "song" ? 1 : 0)});
}

void Session::opSeek(int, const cJSON *m, Reply &)
{
    EngineCmd s{EngineCmd::Seek};
    s.i[0] = (int32_t)std::max(0.0, num(m, "tick", 0));
    post(s);
}

void Session::opLoop(int, const cJSON *m, Reply &r)
{
    const int32_t a = (int32_t)num(m, "start", proj_.loopStart), b = (int32_t)num(m, "end", proj_.loopEnd);
    const bool on = flag(m, "on", proj_.loopOn);
    if (on && (a < 0 || b <= a)) {
        r.fail("bad_arg", "loop end must be after its start");
        return;
    }
    proj_.loopOn = on;
    proj_.loopStart = std::max<int32_t>(0, a);
    proj_.loopEnd = std::max<int32_t>(0, b);
    EngineCmd l{EngineCmd::SetLoop, (uint8_t)(on ? 1 : 0)};
    l.i[0] = proj_.loopStart;
    l.i[1] = proj_.loopEnd;
    post(l);
    changed();
    patch("meta", metaJson(proj_));
}

void Session::opMetronome(int, const cJSON *m, Reply &)
{
    proj_.metronome = flag(m, "on", proj_.metronome);
    proj_.countInBars = std::clamp((int)num(m, "countIn", proj_.countInBars), 0, 2);
    pushTransportConfig();
    changed();
    patch("meta", metaJson(proj_));
}

// ---- patterns ---------------------------------------------------------------------------------

void Session::selectPattern(Id id, bool queue)
{
    const int idx = proj_.patternIndex(id);
    if (idx < 0)
        return;
    proj_.currentPattern = id;
    post(EngineCmd{queue ? EngineCmd::QueuePattern : EngineCmd::SetPattern, 0, 0, 0, (uint32_t)idx});
    patch("meta", metaJson(proj_));
}

void Session::opPatternAdd(int, const cJSON *m, Reply &r)
{
    if ((int)proj_.patterns.size() >= kMaxPatterns) {
        r.fail("limit", "pattern limit reached");
        return;
    }
    Pattern p;
    p.name = name64(str(m, "name", ("Pattern " + std::to_string(proj_.patterns.size() + 1)).c_str()));
    p.color = parseColor(str(m, "color").c_str(), 0x5f8fff);
    if (has(m, "length")) {
        p.lengthTicks = (int32_t)num(m, "length", 0);
        if (p.lengthTicks < 0 || p.lengthTicks > kMaxSpanTicks) {
            r.fail("bad_arg", "length is 0 (auto) .. 1000 bars");
            return;
        }
    }
    p.id = proj_.newId();
    proj_.patterns.push_back(p);
    changed();
    recompilePlay();
    patch("patternList", patternListJson(proj_));
    patch("pattern", patternJson(proj_.patterns.back(), proj_.patternLength(proj_.patterns.back())));
    if (flag(m, "select", true))
        selectPattern(p.id, false);
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", p.id);
}

void Session::opPatternDup(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(src, "pattern");
    if ((int)proj_.patterns.size() >= kMaxPatterns) {
        r.fail("limit", "pattern limit reached");
        return;
    }
    Pattern p = *src;  // (src stays valid until the push_back below)
    p.id = proj_.newId();
    p.name = name64(p.name + " copy");
    const int at = proj_.patternIndex(src->id) + 1;
    proj_.patterns.insert(proj_.patterns.begin() + at, p);
    changed();
    recompilePlay();
    patch("patternList", patternListJson(proj_));
    patch("pattern", patternJson(proj_.patterns[(size_t)at], proj_.patternLength(proj_.patterns[(size_t)at])));
    selectPattern(p.id, false);
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", p.id);
}

void Session::opPatternDel(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    if (proj_.patterns.size() <= 1) {
        r.fail("bad_arg", "the last pattern cannot be deleted");
        return;
    }
    const Id id = p->id;
    const int idx = proj_.patternIndex(id);
    // clips that reference it go too (a pattern clip without its pattern has no meaning)
    proj_.clips.erase(std::remove_if(proj_.clips.begin(), proj_.clips.end(),
                                     [id](const Clip &c) { return c.kind == ClipKind::Pattern && c.ref == id; }),
                      proj_.clips.end());
    proj_.patterns.erase(proj_.patterns.begin() + idx);
    changed();
    recompilePlay();
    if (proj_.currentPattern == id)
        proj_.currentPattern = proj_.patterns[(size_t)std::min(idx, (int)proj_.patterns.size() - 1)].id;
    post(EngineCmd{EngineCmd::SetPattern, 0, 0, 0, (uint32_t)proj_.patternIndex(proj_.currentPattern)});
    patch("patternList", patternListJson(proj_));
    patch("playlist", playlistJson(proj_));
    patch("meta", metaJson(proj_));
}

void Session::opPatternSet(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    if (has(m, "name"))
        p->name = name64(str(m, "name"));
    if (has(m, "color"))
        p->color = parseColor(str(m, "color").c_str(), p->color);
    if (has(m, "length")) {
        const int32_t len = (int32_t)num(m, "length", 0);
        if (len < 0 || len > kMaxSpanTicks) {
            r.fail("bad_arg", "length is 0 (auto) .. 1000 bars");
            return;
        }
        p->lengthTicks = len;
    }
    if (has(m, "swing"))
        p->swing = clampf(num(m, "swing", 0), 0, 1);
    changed();
    recompilePlay();
    patch("patternList", patternListJson(proj_));
}

void Session::opPatternSelect(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    selectPattern(p->id, flag(m, "queue", true));
}

void Session::opPatternMove(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    const int from = proj_.patternIndex(p->id);
    const int to = std::clamp((int)num(m, "index", from), 0, (int)proj_.patterns.size() - 1);
    Pattern tmp = std::move(proj_.patterns[(size_t)from]);
    proj_.patterns.erase(proj_.patterns.begin() + from);
    proj_.patterns.insert(proj_.patterns.begin() + to, std::move(tmp));
    changed();
    recompilePlay();
    post(EngineCmd{EngineCmd::SetPattern, 0, 0, 0, (uint32_t)proj_.patternIndex(proj_.currentPattern)});
    patch("patternList", patternListJson(proj_));
}

// ---- steps / notes ----------------------------------------------------------------------------

void Session::opStepsSet(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    ChannelSeq &s = p->get(ch->id);
    const cJSON *steps = item(m, "steps");
    if (has(m, "count"))
        s.resizeSteps((int)num(m, "count", s.stepCount));
    if (cJSON_IsArray(steps)) {
        const int n = cJSON_GetArraySize(steps);
        if (n > kMaxSteps) {
            r.fail("limit", "at most 128 steps");
            return;
        }
        if (!has(m, "count"))
            s.resizeSteps(n);
        int i = 0;
        const cJSON *st;
        cJSON_ArrayForEach(st, steps)
        {
            Step x;
            stepFromJson(st, x);
            x.vel = clampf(x.vel, 0, 1);
            x.pan = clampf(x.pan, -1, 1);
            x.prob = clampf(x.prob, 0, 1);
            if (i < s.stepCount)
                s.steps[(size_t)i] = x;
            i++;
        }
    }
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

void Session::opStepToggle(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    ChannelSeq &s = p->get(ch->id);
    const int i = (int)num(m, "index", -1);
    if (i < 0 || i >= s.stepCount) {
        r.fail("bad_arg", "step index out of range");
        return;
    }
    Step &st = s.steps[(size_t)i];
    st.on = has(m, "on") ? flag(m, "on", !st.on) : !st.on;
    if (has(m, "vel"))
        st.vel = clampf(num(m, "vel", st.vel), 0, 1);
    if (has(m, "accent"))
        st.accent = flag(m, "accent", st.accent);
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

void Session::opSeqMode(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    ChannelSeq &s = p->get(ch->id);
    const bool pr = flag(m, "pianoRoll", !s.pianoRoll);
    if (pr && !s.pianoRoll && flag(m, "convert", true) && s.notes.empty()) {
        // the steps become notes (as when a step row is opened in the piano roll)
        std::vector<Note> notes;
        stepsToNotes(s, ch->root, p->swing, notes);
        for (auto &n : notes) {
            n.id = s.nextNoteId++;
            s.notes.push_back(n);
        }
        s.sortNotes();
    }
    s.pianoRoll = pr;
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

static bool validNote(const Note &n, std::string &why)
{
    if (n.start < 0 || n.start > kMaxSpanTicks) {
        why = "note start out of range";
        return false;
    }
    if (n.len < 1 || n.len > kMaxSpanTicks) {
        why = "note length out of range";
        return false;
    }
    return true;
}

static void clampNote(Note &n)
{
    n.vel = std::clamp(n.vel == n.vel ? n.vel : 0.78f, 0.0f, 1.0f);
    n.pan = std::clamp(n.pan == n.pan ? n.pan : 0.0f, -1.0f, 1.0f);
    n.pitch = std::clamp(n.pitch == n.pitch ? n.pitch : 0.0f, -1.0f, 1.0f);
    n.rel = std::clamp(n.rel == n.rel ? n.rel : 0.5f, 0.0f, 1.0f);
    n.prob = std::clamp(n.prob == n.prob ? n.prob : 1.0f, 0.0f, 1.0f);
}

void Session::opNotesAdd(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    ChannelSeq &s = p->get(ch->id);
    const cJSON *notes = item(m, "notes");
    if (!cJSON_IsArray(notes)) {
        r.fail("bad_arg", "notes must be an array");
        return;
    }
    if (s.notes.size() + (size_t)cJSON_GetArraySize(notes) > (size_t)kMaxNotesPerSeq) {
        r.fail("limit", "too many notes in this pattern / channel");
        return;
    }
    std::vector<Note> add;
    const cJSON *j;
    cJSON_ArrayForEach(j, notes)
    {
        Note n;
        std::string why;
        if (!noteFromJson(j, n) || !validNote(n, why)) {
            r.fail("bad_arg", why.empty() ? "invalid note" : why);
            return;
        }
        clampNote(n);
        n.id = s.nextNoteId++;
        add.push_back(n);
    }
    r.result = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(r.result, "ids");
    for (auto &n : add) {
        cJSON_AddItemToArray(ids, cJSON_CreateNumber(n.id));
        s.notes.push_back(n);
    }
    s.sortNotes();
    if (flag(m, "pianoRoll", true))
        s.pianoRoll = true;
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

void Session::opNotesUpdate(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    ChannelSeq *s = p->find(ch->id);
    const cJSON *notes = item(m, "notes");
    if (!s || !cJSON_IsArray(notes)) {
        r.fail("bad_arg", "nothing to update");
        return;
    }
    // validate everything before changing anything (an op applies completely or not at all)
    std::vector<std::pair<size_t, Note>> upd;
    const cJSON *j;
    cJSON_ArrayForEach(j, notes)
    {
        const Id id = (Id)num(j, "id", 0);
        auto it = std::find_if(s->notes.begin(), s->notes.end(), [id](const Note &n) { return n.id == id; });
        if (it == s->notes.end()) {
            r.fail("not_found", "note " + std::to_string(id) + " does not exist (stale edit)");
            return;
        }
        Note n = *it;
        noteFromJson(j, n);
        n.id = id;
        std::string why;
        if (!validNote(n, why)) {
            r.fail("bad_arg", why);
            return;
        }
        clampNote(n);
        upd.emplace_back((size_t)(it - s->notes.begin()), n);
    }
    for (auto &u : upd)
        s->notes[u.first] = u.second;
    s->sortNotes();
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

void Session::opNotesDel(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    ChannelSeq *s = p->find(ch->id);
    const cJSON *ids = item(m, "ids");
    if (!s || !cJSON_IsArray(ids)) {
        r.fail("bad_arg", "ids must be an array");
        return;
    }
    std::vector<Id> del;
    const cJSON *j;
    cJSON_ArrayForEach(j, ids) del.push_back((Id)(cJSON_IsNumber(j) ? j->valuedouble : 0));
    s->notes.erase(std::remove_if(s->notes.begin(), s->notes.end(),
                                  [&](const Note &n) { return std::find(del.begin(), del.end(), n.id) != del.end(); }),
                   s->notes.end());
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

bool Session::prepareMidiFont(Id channel, const std::vector<MidiEvent> &events, Reply &r)
{
    Channel *c = proj_.channel(channel);
    const auto *font = c ? c->inst.resource("soundfont") : nullptr;
    if (!c || !font || c->inst.pluginId != "org.smhgzll.soundfont") return true;
    const std::string name = *font;
    if (std::find(sf2FullFonts_.begin(), sf2FullFonts_.end(), name) != sf2FullFonts_.end()) return true;
    auto &selection = sf2Selections_[name];
    const auto before = selection;
    int bank = 0;
    for (const auto &kv : c->inst.params) if (kv.first == "bank") bank = (int)kv.second;
    int msb = bank >> 7, lsb = bank & 127;
    std::vector<Sf2Preset> needed;
    auto add = [&](int b, int program) {
        needed.push_back({"", (uint16_t)b, (uint16_t)program});
        if (std::none_of(selection.begin(), selection.end(), [&](const auto &p) { return p.bank == b && p.program == program; })) {
            selection.push_back({"", (uint16_t)b, (uint16_t)program});
        }
    };
    if (midiHasReset(events)) add(bank == 128 ? 128 : 0, 0);
    for (const auto &e : events) {
        if (e.data.size() < 2) continue;
        const auto hi = e.data[0] & 0xf0;
        if (hi == 0xb0 && e.data.size() == 3) {
            if (e.data[1] == 0) msb = e.data[2]; else if (e.data[1] == 32) lsb = e.data[2];
        } else if (hi == 0xc0) {
            const bool drums = (e.data[0] & 15) == 9 || bank == 128;
            add(drums ? 128 : msb * 128 + lsb, e.data[1]); add(drums ? 128 : 0, e.data[1]);
        }
    }
    // Another channel may have enlarged the shared plan since this instance was bound.
    // Inspect this instance's actual compact font, rather than mistaking a global cache hit
    // for proof that its old index contains a future program.
    const int currentSlot = slotOf(c->id);
    const auto *current = currentSlot >= 0 ? chanInst_[currentSlot] : nullptr;
    const auto *retained = current ? current->retainedResource(name) : nullptr;
    std::vector<Sf2Preset> available, bound;
    auto has = [](const auto &list, int b, int p) {
        return std::any_of(list.begin(), list.end(), [&](const auto &x) { return x.bank == b && x.program == p; });
    };
    const auto *info = cfg_.samples ? cfg_.samples->find(name) : nullptr;
    bool covered = needed.empty();
    if (info && retained && retained->resource()->type == DAW_RES_FILE) {
        const auto *resource = retained->resource();
        const auto *bytes = static_cast<const uint8_t *>(resource->data);
        const bool sourceOK = sf2Presets([&](uint32_t at, void *dst, uint32_t n) { return cfg_.samples->readRange(name, at, dst, n); }, info->size, available, nullptr);
        const bool boundOK = sf2Presets([&](uint32_t at, void *dst, uint32_t n) {
            if (uint64_t(at) + n > resource->size) return false;
            memcpy(dst, bytes + at, n); return true;
        }, resource->size, bound, nullptr);
        if (sourceOK && boundOK && !available.empty()) {
            covered = true;
            for (const auto &want : needed) {
                auto resolved = want;
                if (!has(available, resolved.bank, resolved.program)) {
                    if (resolved.bank == 128 && has(available, 128, 0)) { resolved.bank = 128; resolved.program = 0; }
                    else if (has(available, 0, resolved.program)) resolved.bank = 0;
                    else resolved = available.front();
                }
                covered &= has(bound, resolved.bank, resolved.program);
            }
        }
    }
    if (covered) return true;
    PluginState state = c->inst; std::string err;
    PluginInstance *next = instantiate(state, &err);
    if (!next || !state.missingResources.empty()) {
        PluginInstance::destroy(next); selection = before;
        r.fail("resources", err.empty() ? "not enough memory for MIDI program changes" : err); return false;
    }
    const int slot = slotOf(c->id);
    c->inst = std::move(state); chanInst_[slot] = next;
    post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = next}});
    return true;
}

void Session::opMidiSet(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    std::vector<MidiEvent> events;
    std::string why;
    if (!midiFromJson(item(m, "events"), events, &why)) { r.fail("bad_arg", why); return; }
    auto &seq = p->get(ch->id);
    size_t from = 0;
    if (flag(m, "append", false)) {
        const bool ordered = seq.midi.empty() || events.empty() || seq.midi.back().tick < events.front().tick ||
            (seq.midi.back().tick == events.front().tick && seq.midi.back().order <= events.front().order);
        if (ordered) from = seq.midi.size();
        size_t bytes = 0;
        for (const auto &e : seq.midi) bytes += e.data.size();
        for (const auto &e : events) bytes += e.data.size();
        if (seq.midi.size() + events.size() > kMaxMidiEvents || bytes > kMaxMidiBytes) { r.fail("limit", "MIDI lane is too large"); return; }
        events.insert(events.begin(), seq.midi.begin(), seq.midi.end());
        std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
            return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
        });
    }
    if (!prepareMidiFont(ch->id, events, r)) return;
    seq.midi = std::move(events);
    changed(); recompilePlay();
    // Indexed tail replacement is idempotent and avoids re-sending a growing multi-megabyte
    // lane after every upload chunk. An out-of-order append replaces the full sorted lane.
    cJSON *value = cJSON_CreateObject();
    cJSON_AddNumberToObject(value, "pattern", p->id); cJSON_AddNumberToObject(value, "channel", ch->id);
    cJSON_AddNumberToObject(value, "from", from); cJSON_AddNumberToObject(value, "total", seq.midi.size());
    cJSON_AddNumberToObject(value, "effLength", proj_.patternLength(*p));
    cJSON_AddItemToObject(value, "events", midiJson(seq.midi, from));
    patch("midi", value);
}

void Session::opNotesSet(int, const cJSON *m, Reply &r)
{
    NEED_PATTERN(p, "pattern");
    NEED_CHANNEL(ch, "channel");
    const cJSON *notes = item(m, "notes");
    if (!cJSON_IsArray(notes) || cJSON_GetArraySize(notes) > kMaxNotesPerSeq) {
        r.fail("bad_arg", "notes must be an array of at most 4096 notes");
        return;
    }
    ChannelSeq &s = p->get(ch->id);
    std::vector<Note> out;
    std::vector<Id> used;
    const cJSON *j;
    cJSON_ArrayForEach(j, notes)
    {
        Note n;
        std::string why;
        if (!noteFromJson(j, n) || !validNote(n, why)) {
            r.fail("bad_arg", why.empty() ? "invalid note" : why);
            return;
        }
        clampNote(n);
        out.push_back(n);
    }
    // keep client ids when they are unique and valid, assign new ones otherwise
    Id next = s.nextNoteId;
    for (auto &n : out) {
        if (n.id == 0 || n.id >= 0x80000000u || std::find(used.begin(), used.end(), n.id) != used.end())
            n.id = 0;
        else
            used.push_back(n.id);
        next = std::max(next, n.id + 1);
    }
    for (auto &n : out)
        if (n.id == 0)
            n.id = next++;
    std::vector<MidiEvent> midi;
    if (has(m, "midi")) {
        std::string why;
        if (!midiFromJson(item(m, "midi"), midi, &why)) { r.fail("bad_arg", why); return; }
    }
    if (has(m, "midi") && !prepareMidiFont(ch->id, midi, r)) return;
    s.notes = std::move(out);
    if (has(m, "midi")) s.midi = std::move(midi);
    s.nextNoteId = next;
    s.sortNotes();
    s.pianoRoll = flag(m, "pianoRoll", true);
    changed();
    recompilePlay();
    patch("pattern", patternJson(*p, proj_.patternLength(*p)));
}

// ---- channels ---------------------------------------------------------------------------------

void Session::opChannelAdd(int, const cJSON *m, Reply &r)
{
    if ((int)proj_.channels.size() >= kMaxChannels) {
        r.fail("limit", "channel limit reached (" + std::to_string(kMaxChannels) + ")");
        return;
    }
    const std::string plugin = str(m, "plugin");
    const daw_plugin_descriptor_t *d = PluginRegistry::find(plugin);
    if (!d || d->kind != DAW_PLUGIN_INSTRUMENT) {
        r.fail("bad_arg", "no instrument plugin '" + plugin + "' in this firmware");
        return;
    }
    if (!cpuAdmits(r, "instrument"))
        return;
    Channel c;
    c.id = proj_.newId();
    c.name = name64(str(m, "name", d->name));
    c.color = parseColor(str(m, "color").c_str(), 0xff9a3c);
    c.icon = name64(str(m, "icon"));
    c.inst.pluginId = plugin;
    c.inst.preset = name64(str(m, "preset"));
    // its own free mixer insert, like a new channel in a fresh project
    int track = (int)num(m, "track", -1);
    if (track < 0 || track >= kMaxMixerTracks) {
        track = 0;
        for (int t = 1; t < kMaxMixerTracks && !track; t++) {
            bool used = false;
            for (auto &x : proj_.channels)
                used |= x.track == t;
            if (!used)
                track = t;
        }
    }
    c.track = track;
    if (track > 0 && proj_.mixer[(size_t)track].name.rfind("Insert ", 0) == 0)
        proj_.mixer[(size_t)track].name = c.name;
    if (allocSlot(c.id) < 0) {
        r.fail("limit", "no free engine slot");
        return;
    }
    proj_.channels.push_back(c);
    Channel &ch = proj_.channels.back();
    installChannel(ch);
    if (!chanInst_[slotOf(ch.id)]) {
        removeChannelInstance(slotOf(ch.id));
        proj_.channels.pop_back();
        r.fail("resources", "the plugin could not be created (memory)");
        return;
    }
    // a step row in every pattern
    for (auto &p : proj_.patterns)
        p.get(ch.id);
    pushLevels();
    changed();
    recompilePlay();
    patch("channels", channelsJson(proj_));
    patch("mixer", mixerJson(proj_));
    for (auto &p : proj_.patterns)
        patch("pattern", patternJson(p, proj_.patternLength(p)));
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", c.id);
}

void Session::opChannelDel(int, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    const Id id = c->id;
    removeChannelInstance(slotOf(id));
    for (auto &p : proj_.patterns)
        p.seqs.erase(std::remove_if(p.seqs.begin(), p.seqs.end(), [id](const ChannelSeq &s) { return s.channel == id; }), p.seqs.end());
    for (auto &a : proj_.automation)
        if (a.target.channel == id && (a.target.kind == AutomationTarget::ChannelVol || a.target.kind == AutomationTarget::ChannelPan ||
                                       a.target.kind == AutomationTarget::ChannelParam))
            a.target.kind = AutomationTarget::None;
    proj_.channels.erase(proj_.channels.begin() + proj_.channelIndex(id));
    pushLevels();
    changed();
    recompilePlay();
    patch("channels", channelsJson(proj_));
    for (auto &p : proj_.patterns)
        patch("pattern", patternJson(p, proj_.patternLength(p)));
    patch("automation", automationJson(proj_));
}

void Session::opChannelSet(int client, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    bool structural = false, levels = false;
    if (has(m, "name")) {
        c->name = name64(str(m, "name"));
        structural = true;
    }
    if (has(m, "color")) {
        c->color = parseColor(str(m, "color").c_str(), c->color);
        structural = true;
    }
    if (has(m, "icon")) {
        c->icon = name64(str(m, "icon"));
        structural = true;
    }
    if (has(m, "root")) {
        c->root = (uint8_t)std::clamp((int)num(m, "root", 60), 0, 127);
        structural = true;
        recompilePlay();
    }
    if (has(m, "track")) {
        const int t = (int)num(m, "track", c->track);
        if (t < 0 || t >= kMaxMixerTracks) {
            r.fail("bad_arg", "no mixer track " + std::to_string(t));
            return;
        }
        c->track = t;
        structural = levels = true;
    }
    if (has(m, "mute")) {
        c->mute = flag(m, "mute", c->mute);
        structural = levels = true;
    }
    if (has(m, "solo")) {
        c->solo = flag(m, "solo", c->solo);
        structural = levels = true;
    }
    const bool fast = has(m, "vol") || has(m, "pan");
    if (has(m, "vol"))
        c->vol = clampf(num(m, "vol", c->vol), 0, 1.25f);
    if (has(m, "pan"))
        c->pan = clampf(num(m, "pan", c->pan), -1, 1);
    changed();
    if (levels)
        pushLevels();
    else if (fast) {
        bool anySolo = false;
        for (auto &x : proj_.channels)
            anySolo |= x.solo;
        pushChannelLevel(*c, anySolo);
    }
    if (structural)
        patch("channels", channelsJson(proj_));
    else if (fast) {  // continuous control: a small "set" instead of the whole list
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "t", "set");
        cJSON_AddNumberToObject(s, "rev", rev_);
        cJSON_AddStringToObject(s, "path", ("channel/" + std::to_string(c->id)).c_str());
        cJSON_AddNumberToObject(s, "vol", c->vol);
        cJSON_AddNumberToObject(s, "pan", c->pan);
        cJSON_AddNumberToObject(s, "by", client);
        broadcast(s);
    }
}

void Session::opChannelMove(int, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    const int from = proj_.channelIndex(c->id);
    const int to = std::clamp((int)num(m, "index", from), 0, (int)proj_.channels.size() - 1);
    Channel tmp = std::move(proj_.channels[(size_t)from]);
    proj_.channels.erase(proj_.channels.begin() + from);
    proj_.channels.insert(proj_.channels.begin() + to, std::move(tmp));
    changed();
    patch("channels", channelsJson(proj_));
}

void Session::opChannelPlugin(int, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    const std::string plugin = str(m, "plugin");
    const daw_plugin_descriptor_t *d = PluginRegistry::find(plugin);
    if (!d || d->kind != DAW_PLUGIN_INSTRUMENT) {
        r.fail("bad_arg", "no instrument plugin '" + plugin + "'");
        return;
    }
    PluginState st;
    st.pluginId = plugin;
    st.preset = name64(str(m, "preset"));
    std::string err;
    PluginInstance *inst = instantiate(st, &err);
    if (!inst) {
        r.fail("resources", err);
        return;
    }
    const int slot = allocSlot(c->id);
    c->inst = st;
    chanInst_[slot] = inst;
    post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = inst}});  // the engine fades the old one out
    changed();
    recompilePlay();  // automation targets of the old plugin are re-resolved
    patch("channels", channelsJson(proj_));
}

void Session::opChannelParam(int client, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    const int slot = slotOf(c->id);
    PluginInstance *inst = slot >= 0 ? chanInst_[slot] : nullptr;
    if (!inst) {
        r.fail("not_found", "channel has no active plugin");
        return;
    }
    const std::string key = str(m, "key");
    uint32_t pid;
    float v;
    if (!has(m, "v") || !inst->setParam(key, (float)num(m, "v", 0), &pid, &v)) {
        r.fail("bad_arg", "no parameter '" + key + "'");
        return;
    }
    if (c->inst.pluginId == "org.smhgzll.soundfont" && (key == "bank" || key == "program") && c->inst.resource("soundfont")) {
        PluginState st = c->inst;
        st.setParam(key, v);
        std::string err;
        PluginInstance *next = instantiate(st, &err);
        if (!next || !st.missingResources.empty()) {
            PluginInstance::destroy(next);
            for (const auto &kv : c->inst.params) if (kv.first == key) inst->setParam(key, kv.second, nullptr, nullptr);
            r.fail("resources", err.empty() ? "could not load the selected SoundFont instrument" : err);
            return;
        }
        c->inst = std::move(st);
        chanInst_[slot] = next;
        post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = next}});
        changed();
        patch("channels", channelsJson(proj_));
        return;
    }
    c->inst.setParam(key, v);
    EngineCmd e{EngineCmd::Param, 0, (uint8_t)slot, 0, pid};
    e.f[0] = v;
    post(e);
    changed();
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "t", "set");
    cJSON_AddNumberToObject(s, "rev", rev_);
    cJSON_AddStringToObject(s, "path", ("channel/" + std::to_string(c->id) + "/param").c_str());
    cJSON_AddStringToObject(s, "key", key.c_str());
    cJSON_AddNumberToObject(s, "v", v);
    cJSON_AddNumberToObject(s, "by", client);
    broadcast(s);
}

void Session::opChannelPreset(int, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    const int slot = slotOf(c->id);
    PluginInstance *inst = slot >= 0 ? chanInst_[slot] : nullptr;
    std::vector<float> vals;
    const std::string name = str(m, "name");
    if (!inst || !presetValues(inst, name, vals)) {
        r.fail("not_found", "no preset '" + name + "'");
        return;
    }
    const daw_plugin_descriptor_t *d = inst->desc();
    for (uint32_t i = 0; i < d->param_count; i++) {
        float v = inst->clampParam(i, vals[i]);
        if (v == inst->paramValue(i))
            continue;
        inst->noteParam(i, v);
        EngineCmd e{EngineCmd::Param, 0, (uint8_t)slot, 0, d->params[i].id};
        e.f[0] = v;
        post(e);
    }
    inst->captureState(c->inst);
    c->inst.preset = name;
    changed();
    patch("channels", channelsJson(proj_));
}

// ---- mixer --------------------------------------------------------------------------------------

void Session::opTrackSet(int client, const cJSON *m, Reply &r)
{
    const int idx = (int)num(m, "track", -1);
    if (idx < 0 || idx >= (int)proj_.mixer.size()) {
        r.fail("not_found", "no mixer track");
        return;
    }
    MixerTrack &t = proj_.mixer[(size_t)idx];
    bool structural = false;
    if (has(m, "name")) {
        t.name = name64(str(m, "name"));
        structural = true;
    }
    if (has(m, "color")) {
        t.color = parseColor(str(m, "color").c_str(), t.color);
        structural = true;
    }
    if (has(m, "route")) {
        const int old = t.route;
        const int to = (int)num(m, "route", 0);
        if (idx == 0 || to < 0 || to >= kMaxMixerTracks || to == idx) {
            r.fail("bad_arg", "invalid route");
            return;
        }
        t.route = to;
        std::string err;
        if (!recompileMix(&err)) {
            t.route = old;
            r.fail("cycle", err);
            return;
        }
        structural = true;
    }
    if (has(m, "mute"))
        t.mute = flag(m, "mute", t.mute), structural = true;
    if (has(m, "solo"))
        t.solo = flag(m, "solo", t.solo), structural = true;
    const bool fast = has(m, "vol") || has(m, "pan");
    if (has(m, "vol"))
        t.vol = clampf(num(m, "vol", t.vol), 0, 1.25f);
    if (has(m, "pan"))
        t.pan = clampf(num(m, "pan", t.pan), -1, 1);
    changed();
    pushLevels();
    if (structural)
        patch("track", trackJson(t));
    else if (fast) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "t", "set");
        cJSON_AddNumberToObject(s, "rev", rev_);
        cJSON_AddStringToObject(s, "path", ("track/" + std::to_string(idx)).c_str());
        cJSON_AddNumberToObject(s, "vol", t.vol);
        cJSON_AddNumberToObject(s, "pan", t.pan);
        cJSON_AddNumberToObject(s, "by", client);
        broadcast(s);
    }
}

void Session::opTrackSend(int, const cJSON *m, Reply &r)
{
    const int idx = (int)num(m, "track", -1), slot = (int)num(m, "slot", -1);
    if (idx <= 0 || idx >= (int)proj_.mixer.size() || slot < 0 || slot >= kMaxSendsPerTrack) {
        r.fail("bad_arg", "invalid track / send slot");
        return;
    }
    MixerTrack &t = proj_.mixer[(size_t)idx];
    std::vector<Send> old = t.sends;
    if ((int)t.sends.size() <= slot)
        t.sends.resize((size_t)slot + 1);
    Send &s = t.sends[(size_t)slot];
    if (has(m, "target"))
        s.target = (int)num(m, "target", s.target);
    if (has(m, "level"))
        s.level = clampf(num(m, "level", s.level), 0, 1.25f);
    if (s.target < 0 || s.target >= kMaxMixerTracks || s.target == idx) {
        t.sends = old;
        r.fail("bad_arg", "invalid send target");
        return;
    }
    if (has(m, "target")) {
        std::string err;
        if (!recompileMix(&err)) {
            t.sends = old;
            r.fail("cycle", err);
            return;
        }
    }
    changed();
    pushLevels();
    patch("track", trackJson(t));
}

void Session::opFxAdd(int, const cJSON *m, Reply &r)
{
    const int idx = (int)num(m, "track", -1);
    if (idx < 0 || idx >= (int)proj_.mixer.size()) {
        r.fail("not_found", "no mixer track");
        return;
    }
    MixerTrack &t = proj_.mixer[(size_t)idx];
    if ((int)t.fx.size() >= kMaxFxPerTrack) {
        r.fail("limit", "at most " + std::to_string(kMaxFxPerTrack) + " effects per track");
        return;
    }
    const std::string plugin = str(m, "plugin");
    const daw_plugin_descriptor_t *d = PluginRegistry::find(plugin);
    if (!d || d->kind != DAW_PLUGIN_EFFECT) {
        r.fail("bad_arg", "no effect plugin '" + plugin + "'");
        return;
    }
    if (!cpuAdmits(r, "effect"))
        return;
    FxSlot f;
    f.id = proj_.newId();
    f.inst.pluginId = plugin;
    f.inst.preset = name64(str(m, "preset"));
    std::string err;
    PluginInstance *inst = instantiate(f.inst, &err);
    if (!inst) {
        r.fail("resources", err);
        return;
    }
    const int at = std::clamp((int)num(m, "index", (double)t.fx.size()), 0, (int)t.fx.size());
    t.fx.insert(t.fx.begin() + at, f);
    for (int i = kEngineFx - 1; i > at; i--)
        fxInst_[idx][i] = fxInst_[idx][i - 1];
    fxInst_[idx][at] = inst;
    pushFxChain(idx);
    changed();
    recompilePlay();  // fx automation indices may have moved
    patch("track", trackJson(t));
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", f.id);
}

void Session::opFxDel(int, const cJSON *m, Reply &r)
{
    int track = -1;
    FxSlot *f = proj_.fx((Id)num(m, "fx", 0), &track);
    if (!f) {
        r.fail("not_found", "no such effect");
        return;
    }
    MixerTrack &t = proj_.mixer[(size_t)track];
    const int at = (int)(f - t.fx.data());
    t.fx.erase(t.fx.begin() + at);
    for (int i = at; i < kEngineFx - 1; i++)
        fxInst_[track][i] = fxInst_[track][i + 1];
    fxInst_[track][kEngineFx - 1] = nullptr;
    pushFxChain(track);  // the engine retires the removed instance
    changed();
    recompilePlay();
    patch("track", trackJson(t));
}

void Session::opFxMove(int, const cJSON *m, Reply &r)
{
    int track = -1;
    FxSlot *f = proj_.fx((Id)num(m, "fx", 0), &track);
    if (!f) {
        r.fail("not_found", "no such effect");
        return;
    }
    MixerTrack &t = proj_.mixer[(size_t)track];
    const int from = (int)(f - t.fx.data());
    const int to = std::clamp((int)num(m, "index", from), 0, (int)t.fx.size() - 1);
    FxSlot tmp = std::move(t.fx[(size_t)from]);
    PluginInstance *inst = fxInst_[track][from];
    t.fx.erase(t.fx.begin() + from);
    for (int i = from; i < kEngineFx - 1; i++)
        fxInst_[track][i] = fxInst_[track][i + 1];
    t.fx.insert(t.fx.begin() + to, std::move(tmp));
    for (int i = kEngineFx - 1; i > to; i--)
        fxInst_[track][i] = fxInst_[track][i - 1];
    fxInst_[track][to] = inst;
    pushFxChain(track);
    changed();
    recompilePlay();
    patch("track", trackJson(t));
}

void Session::opFxSet(int, const cJSON *m, Reply &r)
{
    int track = -1;
    FxSlot *f = proj_.fx((Id)num(m, "fx", 0), &track);
    if (!f) {
        r.fail("not_found", "no such effect");
        return;
    }
    if (has(m, "enabled")) {
        const bool en = flag(m, "enabled", f->enabled);
        if (en && !f->enabled && !cpuAdmits(r, "effect"))
            return;
        f->enabled = en;
    }
    if (has(m, "mix"))
        f->mix = clampf(num(m, "mix", f->mix), 0, 1);
    const int at = (int)(f - proj_.mixer[(size_t)track].fx.data());
    post(EngineCmd{EngineCmd::FxState, (uint8_t)track, (uint8_t)at, (uint8_t)(f->enabled ? 1 : 0), 0, {.f = {f->mix}}});
    changed();
    patch("track", trackJson(proj_.mixer[(size_t)track]));
}

void Session::opFxParam(int client, const cJSON *m, Reply &r)
{
    int track = -1;
    const Id fxId = (Id)num(m, "fx", 0);
    FxSlot *f = proj_.fx(fxId, &track);
    if (!f) {
        r.fail("not_found", "no such effect");
        return;
    }
    const int at = (int)(f - proj_.mixer[(size_t)track].fx.data());
    PluginInstance *inst = fxInst_[track][at];
    const std::string key = str(m, "key");
    uint32_t pid;
    float v;
    if (!inst || !has(m, "v") || !inst->setParam(key, (float)num(m, "v", 0), &pid, &v)) {
        r.fail("bad_arg", "no parameter '" + key + "'");
        return;
    }
    f->inst.setParam(key, v);
    EngineCmd e{EngineCmd::Param, 1, (uint8_t)track, (uint8_t)at, pid};
    e.f[0] = v;
    post(e);
    changed();
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "t", "set");
    cJSON_AddNumberToObject(s, "rev", rev_);
    cJSON_AddStringToObject(s, "path", ("fx/" + std::to_string(fxId) + "/param").c_str());
    cJSON_AddStringToObject(s, "key", key.c_str());
    cJSON_AddNumberToObject(s, "v", v);
    cJSON_AddNumberToObject(s, "by", client);
    broadcast(s);
}

void Session::opFxPreset(int, const cJSON *m, Reply &r)
{
    int track = -1;
    FxSlot *f = proj_.fx((Id)num(m, "fx", 0), &track);
    if (!f) {
        r.fail("not_found", "no such effect");
        return;
    }
    const int at = (int)(f - proj_.mixer[(size_t)track].fx.data());
    PluginInstance *inst = fxInst_[track][at];
    std::vector<float> vals;
    const std::string name = str(m, "name");
    if (!inst || !presetValues(inst, name, vals)) {
        r.fail("not_found", "no preset '" + name + "'");
        return;
    }
    const daw_plugin_descriptor_t *d = inst->desc();
    for (uint32_t i = 0; i < d->param_count; i++) {
        const float v = inst->clampParam(i, vals[i]);
        inst->noteParam(i, v);
        EngineCmd e{EngineCmd::Param, 1, (uint8_t)track, (uint8_t)at, d->params[i].id};
        e.f[0] = v;
        post(e);
    }
    inst->captureState(f->inst);
    f->inst.preset = name;
    changed();
    patch("track", trackJson(proj_.mixer[(size_t)track]));
}

// ---- playlist -------------------------------------------------------------------------------

static bool clipFromMsg(const cJSON *j, Clip &c, const Project &p, std::string &why)
{
    if (has(j, "track"))
        c.track = (int)num(j, "track", c.track);
    if (has(j, "start"))
        c.start = (int32_t)num(j, "start", c.start);
    if (has(j, "len"))
        c.len = (int32_t)num(j, "len", c.len);
    if (has(j, "offset"))
        c.offset = (int32_t)num(j, "offset", c.offset);
    if (has(j, "muted"))
        c.muted = flag(j, "muted", c.muted);
    if (c.track < 0 || c.track >= (int)p.tracks.size()) {
        why = "no playlist track " + std::to_string(c.track);
        return false;
    }
    if (c.start < 0 || c.start > kMaxSpanTicks || c.len < 1 || c.len > kMaxSpanTicks || c.offset < 0) {
        why = "clip position / length out of range";
        return false;
    }
    return true;
}

void Session::opClipAdd(int, const cJSON *m, Reply &r)
{
    if ((int)proj_.clips.size() >= kMaxClips) {
        r.fail("limit", "clip limit reached");
        return;
    }
    Clip c;
    c.id = proj_.newId();
    const std::string kind = str(m, "kind", "pattern");
    c.kind = kind == "automation" ? ClipKind::Automation : ClipKind::Pattern;
    c.ref = (Id)num(m, "ref", 0);
    if (c.kind == ClipKind::Pattern ? !proj_.pattern(c.ref) : !proj_.automationById(c.ref)) {
        r.fail("not_found", "the clip's pattern / automation does not exist");
        return;
    }
    c.len = c.kind == ClipKind::Pattern ? proj_.patternLength(*proj_.pattern(c.ref)) : proj_.automationById(c.ref)->lengthTicks;
    std::string why;
    if (!clipFromMsg(m, c, proj_, why)) {
        r.fail("bad_arg", why);
        return;
    }
    proj_.clips.push_back(c);
    changed();
    recompilePlay();
    patch("playlist", playlistJson(proj_));
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", c.id);
}

void Session::opClipUpdate(int, const cJSON *m, Reply &r)
{
    const cJSON *list = item(m, "clips");
    if (!cJSON_IsArray(list)) {
        r.fail("bad_arg", "clips must be an array");
        return;
    }
    std::vector<Clip> upd;
    const cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        Clip *c = proj_.clip((Id)num(j, "id", 0));
        if (!c) {
            r.fail("not_found", "clip " + std::to_string((Id)num(j, "id", 0)) + " does not exist (stale edit)");
            return;
        }
        Clip x = *c;
        std::string why;
        if (!clipFromMsg(j, x, proj_, why)) {
            r.fail("bad_arg", why);
            return;
        }
        upd.push_back(x);
    }
    for (auto &x : upd)
        *proj_.clip(x.id) = x;
    changed();
    recompilePlay();
    patch("playlist", playlistJson(proj_));
}

void Session::opClipDel(int, const cJSON *m, Reply &r)
{
    const cJSON *ids = item(m, "ids");
    if (!cJSON_IsArray(ids)) {
        r.fail("bad_arg", "ids must be an array");
        return;
    }
    std::vector<Id> del;
    const cJSON *j;
    cJSON_ArrayForEach(j, ids) del.push_back((Id)(cJSON_IsNumber(j) ? j->valuedouble : 0));
    proj_.clips.erase(std::remove_if(proj_.clips.begin(), proj_.clips.end(),
                                     [&](const Clip &c) { return std::find(del.begin(), del.end(), c.id) != del.end(); }),
                      proj_.clips.end());
    changed();
    recompilePlay();
    patch("playlist", playlistJson(proj_));
}

void Session::opClipSplit(int, const cJSON *m, Reply &r)
{
    Clip *c = proj_.clip((Id)num(m, "clip", 0));
    const int32_t at = (int32_t)num(m, "at", -1);
    if (!c) {
        r.fail("not_found", "no such clip");
        return;
    }
    if (at <= c->start || at >= c->start + c->len) {
        r.fail("bad_arg", "split point must be inside the clip");
        return;
    }
    if ((int)proj_.clips.size() >= kMaxClips) {
        r.fail("limit", "clip limit reached");
        return;
    }
    Clip right = *c;
    right.id = proj_.newId();
    right.start = at;
    right.len = c->start + c->len - at;
    right.offset = c->offset + (at - c->start);
    c->len = at - c->start;
    proj_.clips.push_back(right);
    changed();
    recompilePlay();
    patch("playlist", playlistJson(proj_));
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", right.id);
}

// replace every clip at once (undo / redo of playlist edits); validated completely before applying
void Session::opClipsSet(int, const cJSON *m, Reply &r)
{
    const cJSON *list = item(m, "clips");
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > kMaxClips) {
        r.fail("bad_arg", "clips must be an array of at most " + std::to_string(kMaxClips));
        return;
    }
    std::vector<Clip> out;
    std::vector<Id> used;
    const cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        Clip c;
        const std::string kind = str(j, "kind", "pattern");
        c.kind = kind == "automation" ? ClipKind::Automation : ClipKind::Pattern;
        c.ref = (Id)num(j, "ref", 0);
        c.id = (Id)num(j, "id", 0);
        if (c.kind == ClipKind::Pattern ? !proj_.pattern(c.ref) : !proj_.automationById(c.ref)) {
            r.fail("not_found", "a clip references a pattern / automation that does not exist");
            return;
        }
        std::string why;
        if (!clipFromMsg(j, c, proj_, why)) {
            r.fail("bad_arg", why);
            return;
        }
        if (c.id == 0 || std::find(used.begin(), used.end(), c.id) != used.end())
            c.id = proj_.newId();
        else
            proj_.nextId = std::max(proj_.nextId, c.id + 1);
        used.push_back(c.id);
        out.push_back(c);
    }
    proj_.clips = std::move(out);
    changed();
    recompilePlay();
    patch("playlist", playlistJson(proj_));
}

void Session::opPTrackSet(int, const cJSON *m, Reply &r)
{
    int idx = (int)num(m, "index", -1);
    if (flag(m, "add", false)) {
        if ((int)proj_.tracks.size() >= kMaxPlaylistTracks) {
            r.fail("limit", "playlist track limit reached");
            return;
        }
        PlaylistTrack t;
        t.name = "Track " + std::to_string(proj_.tracks.size() + 1);
        proj_.tracks.push_back(t);
        idx = (int)proj_.tracks.size() - 1;
    }
    if (idx < 0 || idx >= (int)proj_.tracks.size()) {
        r.fail("not_found", "no playlist track");
        return;
    }
    PlaylistTrack &t = proj_.tracks[(size_t)idx];
    if (has(m, "name"))
        t.name = name64(str(m, "name"));
    if (has(m, "color"))
        t.color = parseColor(str(m, "color").c_str(), t.color);
    if (has(m, "mute"))
        t.mute = flag(m, "mute", t.mute);
    if (has(m, "solo"))
        t.solo = flag(m, "solo", t.solo);
    changed();
    recompilePlay();
    patch("playlist", playlistJson(proj_));
}

void Session::opMarker(int, const cJSON *m, Reply &r)
{
    const std::string action = str(m, "action", "add");
    if (action == "add") {
        Marker mk;
        mk.id = proj_.newId();
        mk.tick = std::max<int32_t>(0, (int32_t)num(m, "tick", 0));
        mk.name = name64(str(m, "name", "Marker"));
        proj_.markers.push_back(mk);
        r.result = cJSON_CreateObject();
        cJSON_AddNumberToObject(r.result, "newId", mk.id);
    } else {
        const Id id = (Id)num(m, "marker", 0);
        auto it = std::find_if(proj_.markers.begin(), proj_.markers.end(), [id](const Marker &x) { return x.id == id; });
        if (it == proj_.markers.end()) {
            r.fail("not_found", "no such marker");
            return;
        }
        if (action == "del")
            proj_.markers.erase(it);
        else {
            if (has(m, "tick"))
                it->tick = std::max<int32_t>(0, (int32_t)num(m, "tick", it->tick));
            if (has(m, "name"))
                it->name = name64(str(m, "name"));
        }
    }
    changed();
    patch("playlist", playlistJson(proj_));
}

// ---- automation -------------------------------------------------------------------------------

static bool pointsFromMsg(const cJSON *pts, std::vector<AutoPoint> &out, std::string &why)
{
    if (!cJSON_IsArray(pts) || cJSON_GetArraySize(pts) > kMaxAutoPoints) {
        why = "points must be an array of at most 1024 [tick, value, tension]";
        return false;
    }
    out.clear();
    const cJSON *p;
    cJSON_ArrayForEach(p, pts)
    {
        AutoPoint a;
        a.tick = std::max<int32_t>(0, (int32_t)(cJSON_IsNumber(cJSON_GetArrayItem(p, 0)) ? cJSON_GetArrayItem(p, 0)->valuedouble : 0));
        const cJSON *v = cJSON_GetArrayItem(p, 1), *t = cJSON_GetArrayItem(p, 2);
        a.value = std::clamp(cJSON_IsNumber(v) && isfinite(v->valuedouble) ? (float)v->valuedouble : 0.5f, 0.0f, 1.0f);
        a.tension = std::clamp(cJSON_IsNumber(t) && isfinite(t->valuedouble) ? (float)t->valuedouble : 0.0f, -1.0f, 1.0f);
        out.push_back(a);
    }
    std::stable_sort(out.begin(), out.end(), [](const AutoPoint &a, const AutoPoint &b) { return a.tick < b.tick; });
    return true;
}

bool Session::prepareFontAutomation(const AutomationTarget &target, Reply &r)
{
    if (target.kind != AutomationTarget::ChannelParam || (target.param != "bank" && target.param != "program")) return true;
    auto *c = proj_.channel(target.channel);
    const auto *font = c ? c->inst.resource("soundfont") : nullptr;
    if (!c || c->inst.pluginId != "org.smhgzll.soundfont" || !font) return true;
    if (std::find(sf2FullChannels_.begin(), sf2FullChannels_.end(), c->id) != sf2FullChannels_.end()) return true;
    const std::string name = *font;
    const bool added = std::find(sf2FullFonts_.begin(), sf2FullFonts_.end(), name) == sf2FullFonts_.end();
    if (added) sf2FullFonts_.push_back(name);
    PluginState st = c->inst; std::string err;
    PluginInstance *next = instantiate(st, &err);
    if (!next || !st.missingResources.empty()) {
        PluginInstance::destroy(next); if (added) sf2FullFonts_.pop_back();
        r.fail("resources", err.empty() ? "not enough memory for SoundFont program automation" : err); return false;
    }
    const int slot = slotOf(c->id); c->inst = std::move(st); chanInst_[slot] = next;
    sf2FullChannels_.push_back(c->id);
    post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = next}});
    return true;
}

void Session::opAutoAdd(int, const cJSON *m, Reply &r)
{
    if ((int)proj_.automation.size() >= kMaxAutomation) {
        r.fail("limit", "automation clip limit reached");
        return;
    }
    Automation a;
    a.id = proj_.newId();
    a.name = name64(str(m, "name", "Automation"));
    targetFromJson(item(m, "target"), a.target);
    a.lengthTicks = std::clamp<int32_t>((int32_t)num(m, "length", kPPQ * 16), kPPQ, kPPQ * 4 * 256);
    std::string why;
    if (has(m, "points") && !pointsFromMsg(item(m, "points"), a.points, why)) {
        r.fail("bad_arg", why);
        return;
    }
    if (a.points.empty())
        a.points = {{0, 0.5f, 0}, {a.lengthTicks, 0.5f, 0}};
    if (!prepareFontAutomation(a.target, r)) return;
    proj_.automation.push_back(a);
    changed();
    recompilePlay();
    patch("automation", automationJson(proj_));
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "newId", a.id);
}

void Session::opAutoSet(int, const cJSON *m, Reply &r)
{
    Automation *a = proj_.automationById((Id)num(m, "auto", 0));
    if (!a) {
        r.fail("not_found", "no such automation clip");
        return;
    }
    std::string why;
    std::vector<AutoPoint> pts;
    if (has(m, "points") && !pointsFromMsg(item(m, "points"), pts, why)) {
        r.fail("bad_arg", why);
        return;
    }
    AutomationTarget target = a->target;
    if (has(m, "target")) targetFromJson(item(m, "target"), target);
    if (!prepareFontAutomation(target, r)) return;
    if (has(m, "points"))
        a->points = pts;
    if (has(m, "name"))
        a->name = name64(str(m, "name"));
    if (has(m, "target"))
        targetFromJson(item(m, "target"), a->target);
    if (has(m, "length"))
        a->lengthTicks = std::clamp<int32_t>((int32_t)num(m, "length", a->lengthTicks), kPPQ, kPPQ * 4 * 256);
    changed();
    recompilePlay();
    patch("automation", automationJson(proj_));
}

void Session::opAutoDel(int, const cJSON *m, Reply &r)
{
    const Id id = (Id)num(m, "auto", 0);
    if (!proj_.automationById(id)) {
        r.fail("not_found", "no such automation clip");
        return;
    }
    proj_.automation.erase(std::remove_if(proj_.automation.begin(), proj_.automation.end(), [id](const Automation &a) { return a.id == id; }),
                           proj_.automation.end());
    proj_.clips.erase(std::remove_if(proj_.clips.begin(), proj_.clips.end(),
                                     [id](const Clip &c) { return c.kind == ClipKind::Automation && c.ref == id; }),
                      proj_.clips.end());
    changed();
    recompilePlay();
    patch("automation", automationJson(proj_));
    patch("playlist", playlistJson(proj_));
}

// ---- live input -------------------------------------------------------------------------------

void Session::liveNote(int clientId, Id channel, uint8_t key, float vel, bool on)
{
    const int slot = slotOf(channel);
    if (slot < 0 || key > 127)
        return;
    Client *c = client(clientId);
    EngineCmd e{EngineCmd::Note, (uint8_t)slot, key, (uint8_t)(on ? 1 : 0)};
    e.f[0] = std::clamp(vel, 0.0f, 1.0f);
    e.u32 = 0xFFFFFFFFu;  // -1: the plugin matches note-offs by key
    post(e);
    if (c) {
        auto it = std::find(c->held.begin(), c->held.end(), std::make_pair((uint8_t)slot, key));
        if (on && it == c->held.end() && c->held.size() < 128)
            c->held.emplace_back((uint8_t)slot, key);
        else if (!on && it != c->held.end())
            c->held.erase(it);
    }
}

void Session::opNote(int c, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(ch, "channel");
    const int key = (int)num(m, "key", -1);
    if (key < 0 || key > 127) {
        r.fail("bad_arg", "key 0..127");
        return;
    }
    liveNote(c, ch->id, (uint8_t)key, (float)num(m, "vel", 0.8), flag(m, "on", true));
}

void Session::midiIn(int c, Id channel, const MidiMsg &msg, double ts)
{
    // without an explicit target channel, MIDI channel n plays channel-rack row n
    Id target = channel;
    if (!target && msg.status < 0xF0) {
        const size_t row = msg.channel();
        if (row < proj_.channels.size())
            target = proj_.channels[row].id;
    }
    const int slot = slotOf(target);
    switch (msg.type()) {
    case 0x90:
        if (msg.d2)
            liveNote(c, target, msg.d1 & 127, msg.d2 / 127.0f, true);
        else
            liveNote(c, target, msg.d1 & 127, 0.5f, false);
        break;
    case 0x80: liveNote(c, target, msg.d1 & 127, msg.d2 / 127.0f, false); break;
    case 0xB0:
        if (msg.d1 == 120 || msg.d1 >= 123) {
            if (slot >= 0) post(EngineCmd{EngineCmd::RawEvent, (uint8_t)slot,
                (uint8_t)(msg.d1 == 120 ? DAW_EV_ALL_SOUND_OFF : DAW_EV_ALL_NOTES_OFF), msg.d1, msg.channel()});
            break;
        }
        if (slot >= 0) {
            EngineCmd e{EngineCmd::RawEvent, (uint8_t)slot, (uint8_t)(msg.d1 == 64 ? DAW_EV_SUSTAIN : DAW_EV_CC), msg.d1, msg.channel()};
            e.f[0] = msg.d2 / 127.0f;
            post(e);
        }
        break;
    case 0xA0:
    case 0xD0:
        if (slot >= 0) {
            EngineCmd e{EngineCmd::RawEvent, (uint8_t)slot,
                (uint8_t)(msg.type() == 0xA0 ? DAW_EV_POLY_PRESSURE : DAW_EV_CHANNEL_PRESSURE), msg.d1, msg.channel()};
            e.f[0] = (msg.type() == 0xA0 ? msg.d2 : msg.d1) / 127.0f;
            post(e);
        }
        break;
    case 0xE0:
        if (slot >= 0) {
            EngineCmd e{EngineCmd::RawEvent, (uint8_t)slot, (uint8_t)DAW_EV_PITCH_BEND, 0, msg.channel()};
            e.f[0] = (float)(((msg.d2 << 7) | msg.d1) - 8192) / 8192.0f;
            post(e);
        }
        break;
    case 0xC0:
        if (slot >= 0) {
            EngineCmd e{EngineCmd::RawEvent, (uint8_t)slot, (uint8_t)DAW_EV_PROGRAM, msg.d1, msg.channel()};
            post(e);
        }
        break;
    case 0xF8:
        clock_.tick(ts);
        if (extClock_ && clock_.locked() && fabs(clock_.bpm() - proj_.bpm) > 0.25 && nowMs_ - lastExtTempoMs_ > 250) {
            proj_.bpm = std::clamp(round(clock_.bpm() * 10.0) / 10.0, kMinBpm, kMaxBpm);
            EngineCmd b{EngineCmd::SetTempo};
            b.d = proj_.bpm;
            post(b);
            lastExtTempoMs_ = nowMs_;
            patch("meta", metaJson(proj_));
        }
        break;
    case 0xFA:
        extClock_ = true;
        clock_.reset();
        post(EngineCmd{EngineCmd::Stop});
        post(EngineCmd{EngineCmd::Play});
        break;
    case 0xFB:
        extClock_ = true;
        post(EngineCmd{EngineCmd::Play});
        break;
    case 0xFC: post(EngineCmd{EngineCmd::Pause}); break;
    case 0xF2: {  // song position in 16ths
        EngineCmd s{EngineCmd::Seek};
        s.i[0] = ((msg.d2 << 7) | msg.d1) * kStepTicks;
        post(s);
        break;
    }
    default: break;
    }
}

void Session::opMidi(int c, const cJSON *m, Reply &r)
{
    const cJSON *data = item(m, "data");
    if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) > 1024) {
        r.fail("bad_arg", "data must be an array of at most 1024 MIDI bytes");
        return;
    }
    Client *cl = client(c);
    const Id channel = (Id)num(m, "channel", 0);
    const double ts = num(m, "ts", (double)(platform::micros() / 1000));
    const cJSON *b;
    MidiMsg msg;
    cJSON_ArrayForEach(b, data)
    {
        if (!cJSON_IsNumber(b))
            continue;
        if (cl->midi.feed((uint8_t)((int)b->valuedouble & 0xFF), msg))
            midiIn(c, channel, msg, ts);
    }
}

void Session::opPanic(int, const cJSON *, Reply &)
{
    post(EngineCmd{EngineCmd::NotesOff, 1});
    for (auto &c : clients_)
        c.held.clear();
}

// ---- projects ---------------------------------------------------------------------------------

cJSON *Session::projectList()
{
    cJSON *a = cJSON_CreateArray();
    if (!cfg_.store)
        return a;
    for (uint32_t s = 0; s < cfg_.store->slots(); s++) {
        SlotInfo si = cfg_.store->info(s);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", s);
        cJSON_AddBoolToObject(o, "used", si.valid);
        cJSON_AddBoolToObject(o, "autosave", s == 0);
        if (si.valid) {
            cJSON_AddStringToObject(o, "name", si.name.c_str());
            cJSON_AddNumberToObject(o, "bytes", si.bytes);
            cJSON_AddNumberToObject(o, "stored", si.stored);
            cJSON_AddNumberToObject(o, "time", si.time);
            cJSON_AddNumberToObject(o, "seq", si.seq);
        }
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

void Session::opProjectNew(int, const cJSON *m, Reply &)
{
    start(str(m, "template", "demo") == "empty" ? emptyProject() : defaultProject());
    changed();
}

bool Session::cpuAdmits(Reply &r, const char *what)
{
    const uint32_t load = eng_.status().cpuPermille.load();
    if (cfg_.cpuAdmitPermille && load > cfg_.cpuAdmitPermille) {
        r.fail("cpu", std::string("the audio engine is at ") + std::to_string(load / 10) + " % of its deadline (limit " +
                          std::to_string(cfg_.cpuAdmitPermille / 10) + " %): no " + what +
                          " can be added now; remove or disable one, or lower a polyphony");
        return false;
    }
    return true;
}

bool Session::flashWriteAllowed(Reply &r)
{
    if (cfg_.flashWritesStallAudio && eng_.status().playing.load() != 0) {
        r.fail("playing", "stop playback first: writing flash would interrupt the audio on this device");
        return false;
    }
    return true;
}

void Session::opProjectSave(int, const cJSON *m, Reply &r)
{
    if (!cfg_.store) {
        r.fail("no_storage", "no project storage on this target");
        return;
    }
    if (!flashWriteAllowed(r))
        return;
    const int slot = (int)num(m, "slot", -1);
    if (slot < 1 || slot >= (int)cfg_.store->slots()) {
        r.fail("bad_arg", "slot 1.." + std::to_string(cfg_.store->slots() - 1) + " (0 is the autosave)");
        return;
    }
    if (has(m, "name"))
        proj_.name = name64(str(m, "name"));
    std::string err;
    const std::string text = projectToText(proj_);
    if (text == "null") {  // cJSON ran out of memory: never store that over a project
        r.fail("storage", "out of memory writing the project");
        return;
    }
    if (!cfg_.store->save((uint32_t)slot, proj_.name, text, cfg_.unixTime ? cfg_.unixTime() : 0, &err)) {
        r.fail("storage", err);
        return;
    }
    autosave(nowMs_, true);
    patch("meta", metaJson(proj_));
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "projects");
    cJSON_AddItemToObject(msg, "list", projectList());
    broadcast(msg);
    r.result = cJSON_CreateObject();
    cJSON_AddNumberToObject(r.result, "bytes", (double)text.size());
}

void Session::opProjectLoad(int, const cJSON *m, Reply &r)
{
    if (!cfg_.store) {
        r.fail("no_storage", "no project storage on this target");
        return;
    }
    const int slot = (int)num(m, "slot", -1);
    std::string text, err;
    Project p;
    if (slot < 0 || slot >= (int)cfg_.store->slots() || !cfg_.store->load((uint32_t)slot, text, nullptr, &err) ||
        !projectFromText(text.data(), text.size(), p, &err)) {
        r.fail("storage", err.empty() ? "invalid slot" : err);
        return;
    }
    start(std::move(p));
    changed();  // (the autosave then holds the loaded project)
}

void Session::opProjectList(int c, const cJSON *, Reply &)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "projects");
    cJSON_AddItemToObject(msg, "list", projectList());
    sendTo(c, msg);
}

void Session::opProjectDelete(int, const cJSON *m, Reply &r)
{
    const int slot = (int)num(m, "slot", -1);
    if (!flashWriteAllowed(r))
        return;
    if (!cfg_.store || slot < 1 || slot >= (int)cfg_.store->slots() || !cfg_.store->remove((uint32_t)slot)) {
        r.fail("bad_arg", "invalid slot");
        return;
    }
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "projects");
    cJSON_AddItemToObject(msg, "list", projectList());
    broadcast(msg);
}

void Session::opProjectRename(int, const cJSON *m, Reply &)
{
    proj_.name = name64(str(m, "name", proj_.name.c_str()));
    if (has(m, "author"))
        proj_.author = name64(str(m, "author"));
    changed();
    patch("meta", metaJson(proj_));
}

void Session::opDiagTone(int, const cJSON *m, Reply &r)
{
    static const char *kinds[] = {"off", "sine440", "leftright", "lowlevel", "silence", "click"};
    const std::string k = str(m, "kind", "off");
    for (int i = 0; i < 6; i++)
        if (k == kinds[i]) {
            post(EngineCmd{EngineCmd::Diag, (uint8_t)i});
            return;
        }
    r.fail("bad_arg", "kind: off, sine440, leftright, lowlevel, silence, click");
}

// ---- periodic work -----------------------------------------------------------------------------

void Session::autosave(uint32_t nowMs, bool force)
{
    if (!cfg_.store || !dirty_)
        return;
    if (!force && (eng_.status().playing.load() != 0 || nowMs - lastChangeMs_ < cfg_.autosaveMs))
        return;  // flash writes stall the caches: never while the transport runs (docs/STORAGE.md)
    std::string err;
    const std::string text = projectToText(proj_);
    if (text == "null") {  // cJSON ran out of memory: keep the previous autosave
        DAW_LOGW(TAG, "autosave: out of memory writing the project");
        lastChangeMs_ = nowMs;
        return;
    }
    if (cfg_.store->save(0, proj_.name, text, cfg_.unixTime ? cfg_.unixTime() : 0, &err))
        dirty_ = false;
    else {
        DAW_LOGW(TAG, "autosave failed: %s", err.c_str());
        lastChangeMs_ = nowMs;  // retry later
    }
}

bool Session::idle() const
{
    bool any = false;
    for (auto &c : clients_)
        any |= c.hello;
    return !any && eng_.status().playing.load() == 0;
}

void Session::stopForMaintenance()
{
    post(EngineCmd{EngineCmd::Stop});
    post(EngineCmd{EngineCmd::NotesOff, 1});
    autosave(nowMs_, true);
}

void Session::tick(uint32_t nowMs)
{
    nowMs_ = nowMs;
    // commands the queue refused earlier, in order
    flushCommands();
    drainRetired();
    const bool close = recCloseAt_ && (int32_t)(nowMs - recCloseAt_) >= 0;
    drainRecorded(close);
    if (close)
        recCloseAt_ = 0;
    EngineStatus &st = eng_.status();
    // a queued pattern switch happened in the engine: follow it
    const uint32_t pi = st.patternIndex.load();
    if (pi < proj_.patterns.size() && proj_.patterns[pi].id != proj_.currentPattern && st.queuedPattern.load() < 0 &&
        backlog_.empty()) {
        proj_.currentPattern = proj_.patterns[pi].id;
        patch("meta", metaJson(proj_));
    }
    bool any = false;
    for (auto &c : clients_)
        any |= c.hello;
    if (any && nowMs - lastStatusMs_ >= 50) {
        lastStatusMs_ = nowMs;
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "t", "st");
        cJSON_AddNumberToObject(s, "rev", rev_);
        cJSON_AddNumberToObject(s, "play", st.playing.load());
        cJSON_AddStringToObject(s, "mode", st.mode.load() ? "song" : "pattern");
        cJSON_AddNumberToObject(s, "tick", st.tick.load());
        cJSON_AddNumberToObject(s, "pattern", pi < proj_.patterns.size() ? proj_.patterns[pi].id : 0);
        const int32_t q = st.queuedPattern.load();
        cJSON_AddNumberToObject(s, "queued", q >= 0 && q < (int32_t)proj_.patterns.size() ? proj_.patterns[(size_t)q].id : 0);
        cJSON_AddNumberToObject(s, "bpm", round(st.bpm.load() * 100.0) / 100.0);
        cJSON_AddNumberToObject(s, "cpu", st.cpuPermille.load() / 10.0);
        cJSON_AddNumberToObject(s, "voices", st.voices.load());
        cJSON_AddNumberToObject(s, "cpuSkips", st.cpuGuardDrops.load());
        cJSON_AddBoolToObject(s, "ext", extClock_);
        cJSON_AddBoolToObject(s, "rec", record_);
        cJSON *mt = cJSON_AddArrayToObject(s, "m");
        for (int t = 0; t < kEngineTracks; t++) {
            cJSON_AddItemToArray(mt, cJSON_CreateNumber(round(st.meterL[t].load() * 1000.0) / 1000.0));
            cJSON_AddItemToArray(mt, cJSON_CreateNumber(round(st.meterR[t].load() * 1000.0) / 1000.0));
        }
        cJSON *sm = cJSON_AddArrayToObject(s, "cm");  // channel meters in channel order
        for (const auto &c : proj_.channels) {
            const int sl = slotOf(c.id);
            cJSON_AddItemToArray(sm, cJSON_CreateNumber(sl >= 0 ? round(st.slotMeter[sl].load() * 1000.0) / 1000.0 : 0.0));
        }
        broadcast(s, MsgClass::Live);
    }
    if (any && nowMs - lastDiagMs_ >= 1000) {
        lastDiagMs_ = nowMs;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "t", "diag");
        cJSON_AddNumberToObject(d, "cpu", st.cpuPermille.load() / 10.0);
        cJSON_AddNumberToObject(d, "renderUsAvg", st.renderUsAvg.load());
        cJSON_AddNumberToObject(d, "renderUsMax", st.renderUsMax.load());
        cJSON_AddNumberToObject(d, "budgetUs", st.budgetUs.load());
        cJSON_AddNumberToObject(d, "overruns", st.overruns.load());
        cJSON_AddNumberToObject(d, "eventDrops", st.eventDrops.load());
        cJSON_AddNumberToObject(d, "noteDrops", st.noteDrops.load());
        cJSON_AddNumberToObject(d, "cpuGuardDrops", st.cpuGuardDrops.load());
        cJSON_AddNumberToObject(d, "load", st.loadPermille.load() / 10.0);
        cJSON_AddNumberToObject(d, "workerWait", st.workerWaitPermille.load() / 10.0);
        cJSON_AddNumberToObject(d, "workerBusy", st.workerBusyPermille.load() / 10.0);
        cJSON_AddNumberToObject(d, "nanResets", st.nanResets.load());
        cJSON_AddNumberToObject(d, "fxFailures", st.fxFailures.load());
        cJSON_AddNumberToObject(d, "cmdOverflow", st.cmdOverflow.load());
        cJSON_AddNumberToObject(d, "retireOverflow", st.retireOverflow.load());
        cJSON_AddNumberToObject(d, "voices", st.voices.load());
        cJSON_AddNumberToObject(d, "blocks", st.blocks.load());
        cJSON_AddNumberToObject(d, "backlog", (double)backlog_.size());
        size_t residentFiles = 0, spareFiles = 0;
        for (const auto &b : fileBuffers_)
            (b.use_count() == 1 ? spareFiles : residentFiles) += b->capacity;
        cJSON_AddNumberToObject(d, "fileBufferBytes", (double)residentFiles);
        cJSON_AddNumberToObject(d, "fileSpareBytes", (double)spareFiles);
        cJSON *pl = cJSON_AddArrayToObject(d, "plugins");
        auto add = [&](PluginInstance *p, const char *where, double id) {
            if (!p)
                return;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "where", where);
            cJSON_AddNumberToObject(o, "id", id);
            cJSON_AddStringToObject(o, "plugin", p->desc()->id);
            cJSON_AddNumberToObject(o, "cpu", p->cpuPermille.load() / 10.0);
            cJSON_AddNumberToObject(o, "voices", p->voices.load());
            cJSON_AddNumberToObject(o, "memFast", (double)p->memFast());
            cJSON_AddNumberToObject(o, "memLarge", (double)p->memLarge());
            cJSON_AddNumberToObject(o, "reports", p->reports.load());
            cJSON_AddNumberToObject(o, "parallelJobs", p->parallelJobs.load());
            cJSON_AddNumberToObject(o, "parallelDeclined", p->parallelDeclined.load());
            cJSON_AddItemToArray(pl, o);
        };
        for (const auto &c : proj_.channels)
            if (int s = slotOf(c.id); s >= 0)
                add(chanInst_[s], "channel", c.id);
        for (const auto &t : proj_.mixer)
            for (size_t f = 0; f < t.fx.size(); f++)
                add(fxInst_[t.index][f], "fx", t.fx[f].id);
        if (cfg_.platformDiag)
            cfg_.platformDiag(d);
        broadcast(d, MsgClass::Live);
    }
    autosave(nowMs, false);
}

std::string Session::exportProject() const { return projectToText(proj_); }

bool Session::importProject(const char *text, size_t len, std::string *err)
{
    Project p;
    if (!projectFromText(text, len, p, err))
        return false;
    start(std::move(p));
    changed();
    return true;
}

std::string Session::statusJson()
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", cfg_.deviceName.c_str());
    cJSON_AddStringToObject(o, "firmware", cfg_.firmware.c_str());
    cJSON_AddStringToObject(o, "target", cfg_.target.c_str());
    cJSON_AddNumberToObject(o, "proto", kProtocolVersion);
    cJSON_AddNumberToObject(o, "rev", rev_);
    cJSON_AddStringToObject(o, "project", proj_.name.c_str());
    cJSON_AddNumberToObject(o, "clients", (double)clients_.size());
    cJSON_AddNumberToObject(o, "playing", eng_.status().playing.load());
    cJSON_AddNumberToObject(o, "cpu", eng_.status().cpuPermille.load() / 10.0);
    cJSON_AddNumberToObject(o, "blocks", eng_.status().blocks.load());
    cJSON_AddNumberToObject(o, "overruns", eng_.status().overruns.load());
    cJSON_AddNumberToObject(o, "plugins", (double)PluginRegistry::all().size());
    if (cfg_.platformDiag)
        cfg_.platformDiag(o);
    return jsonText(o);
}

void Session::opChannelResource(int, const cJSON *m, Reply &r)
{
    NEED_CHANNEL(c, "channel");
    const std::string slotKey = str(m, "slot"), sample = str(m, "sample");
    const daw_plugin_descriptor_t *d = PluginRegistry::find(c->inst.pluginId);
    int resourceSlot = -1;
    if (d && daw_plugin_has_resources(d))
        for (uint32_t i = 0; i < d->resource_slot_count; i++)
            if (d->resource_slots[i].key && slotKey == d->resource_slots[i].key)
                resourceSlot = (int)i;
    if (resourceSlot < 0) {
        r.fail("bad_arg", "the channel's plugin has no resource slot '" + slotKey + "'");
        return;
    }
    if (!sample.empty() && (!cfg_.samples || !cfg_.samples->find(sample))) {
        r.fail("not_found", "no sample '" + sample + "' in the library");
        return;
    }
    PluginState st = c->inst;
    if (st.pluginId == "org.smhgzll.soundfont") {
        // An older browser binds first, then sends bank/program as separate operations, without
        // announcing the rest of its MIDI import. Keep its complete font available rather than
        // retaining a growing, different subset for every channel. New imports declare a plan.
        if (!sample.empty() && !has(m, "presets") && !has(m, "bank") && !has(m, "program") &&
            std::find(sf2FullFonts_.begin(), sf2FullFonts_.end(), sample) == sf2FullFonts_.end())
            sf2FullFonts_.push_back(sample);
        for (const char *key : {"bank", "program"}) if (has(m, key))
            st.setParam(key, std::clamp((float)num(m, key, 0), 0.0f, key[0] == 'b' ? 16383.0f : 127.0f));
        if (has(m, "presets")) {
            const cJSON *list = item(m, "presets");
            if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > (int)kMaxSf2Presets) {
                r.fail("bad_arg", "invalid SoundFont preset plan"); return;
            }
            auto &selection = sf2Selections_[sample];
            const cJSON *p;
            cJSON_ArrayForEach(p, list) {
                if (!cJSON_IsObject(p) || !has(p, "bank") || !has(p, "program")) { r.fail("bad_arg", "invalid SoundFont preset plan"); return; }
                Sf2Preset pick; pick.bank = (uint16_t)std::clamp((int)num(p, "bank", 0), 0, 16383); pick.program = (uint16_t)std::clamp((int)num(p, "program", 0), 0, 127);
                selection.push_back(pick);
            }
        }
    }
    std::shared_ptr<const LoadedResource> res;
    std::string err;
    if (!sample.empty()) {
        const SampleInfo *info = cfg_.samples->find(sample);
        const uint32_t types = d->resource_slots[resourceSlot].types;
        const bool audio = info->type == SampleType::Wav && (types & DAW_RES_MASK(DAW_RES_AUDIO));
        if (!(audio || (types & DAW_RES_MASK(DAW_RES_FILE))) || !(res = loadResource(sample, audio, &err, &st))) {
            r.fail("resources", err.empty() ? "unsupported sample type for this slot" : err);
            return;
        }
    }
    auto &rs = st.resources;
    rs.erase(std::remove_if(rs.begin(), rs.end(), [&](const std::pair<std::string, std::string> &kv) { return kv.first == slotKey; }), rs.end());
    if (!sample.empty())
        rs.emplace_back(slotKey, sample);
    PluginInstance *inst = instantiate(st, &err);
    if (!inst || !st.missingResources.empty()) {
        PluginInstance::destroy(inst);
        r.fail("resources", err.empty() ? "the plugin could not load sample '" + sample + "'" : err);
        return;
    }
    const int slot = allocSlot(c->id);
    c->inst = std::move(st);
    chanInst_[slot] = inst;
    post(EngineCmd{EngineCmd::SetInstrument, (uint8_t)slot, 0, 0, 0, {.p = inst}});
    changed();
    patch("channels", channelsJson(proj_));
}

// ---- user presets -------------------------------------------------------------------------------

// a factory preset of the plugin, else a user preset of that name (unknown keys ignored, missing
// keys keep their current value)
bool Session::presetValues(PluginInstance *inst, const std::string &name, std::vector<float> &vals)
{
    if (inst->presetValues(name, vals))
        return true;
    const UserPreset *u = cfg_.presets ? cfg_.presets->find(inst->desc()->id, name) : nullptr;
    if (!u)
        return false;
    const daw_plugin_descriptor_t *d = inst->desc();
    vals.resize(d->param_count);
    for (uint32_t i = 0; i < d->param_count; i++)
        vals[i] = inst->paramValue(i);
    for (const auto &kv : u->params) {
        const int i = inst->paramIndex(kv.first);
        if (i >= 0)
            vals[(size_t)i] = kv.second;
    }
    return true;
}

void Session::broadcastPresets()
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", "userPresets");
    cJSON_AddItemToObject(m, "list", cfg_.presets ? cfg_.presets->json() : cJSON_CreateArray());
    broadcast(m);
}

// {"channel": id} or {"fx": id}, "name": the current settings of that instance as a user preset
void Session::opPresetSave(int, const cJSON *m, Reply &r)
{
    if (!cfg_.presets) {
        r.fail("no_storage", "no preset storage on this target");
        return;
    }
    if (!flashWriteAllowed(r))
        return;
    PluginInstance *inst = nullptr;
    if (has(m, "channel")) {
        Channel *c = proj_.channel((Id)num(m, "channel", 0));
        const int slot = c ? slotOf(c->id) : -1;
        inst = slot >= 0 ? chanInst_[slot] : nullptr;
    } else {
        int track = -1;
        FxSlot *f = proj_.fx((Id)num(m, "fx", 0), &track);
        if (f)
            inst = fxInst_[track][(int)(f - proj_.mixer[(size_t)track].fx.data())];
    }
    if (!inst) {
        r.fail("not_found", "no such channel or effect (or its plugin is not loaded)");
        return;
    }
    const std::string name = str(m, "name");
    for (uint32_t i = 0; i < inst->desc()->preset_count; i++)
        if (name == inst->desc()->presets[i].name) {
            r.fail("bad_arg", "'" + name + "' is a factory preset of this plugin: choose another name");
            return;
        }
    std::vector<std::pair<std::string, float>> params;
    for (uint32_t i = 0; i < inst->desc()->param_count; i++)
        params.emplace_back(inst->desc()->params[i].key, inst->paramValue(i));
    std::string err;
    if (!cfg_.presets->save(inst->desc()->id, name, params, &err)) {
        r.fail("storage", err);
        return;
    }
    broadcastPresets();
}

void Session::opPresetDelete(int, const cJSON *m, Reply &r)
{
    if (!flashWriteAllowed(r))
        return;
    if (!cfg_.presets || !cfg_.presets->remove(str(m, "plugin"), str(m, "name"))) {
        r.fail("not_found", "no such user preset");
        return;
    }
    broadcastPresets();
}

}  // namespace daw
