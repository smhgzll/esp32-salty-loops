// SPDX-License-Identifier: MIT
#include "dawsdk_host/host.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace dawhost {

namespace {

struct AllocHeader {
    size_t size;
    uint32_t flags;
    uint32_t magic;
    void *base;
};
constexpr uint32_t kMagic = 0xDA3A110Cu;

void *hostAlloc(void *ctx, size_t size, size_t align, uint32_t flags)
{
    auto *h = static_cast<HostServices *>(ctx);
    if (!h->allocAllowed)
        h->rtViolations++;
    if (align < alignof(AllocHeader))
        align = alignof(AllocHeader);
    if (align > 64 || (align & (align - 1)))
        return nullptr;
    size_t total = size + align + sizeof(AllocHeader);
    void *base = malloc(total);
    if (!base)
        return nullptr;
    uintptr_t p = (uintptr_t)base + sizeof(AllocHeader);
    p = (p + align - 1) & ~(uintptr_t)(align - 1);
    auto *hdr = reinterpret_cast<AllocHeader *>(p - sizeof(AllocHeader));
    hdr->size = size;
    hdr->flags = flags;
    hdr->magic = kMagic;
    hdr->base = base;
    if (flags & DAW_MEM_ZERO)
        memset((void *)p, 0, size);
    else
        memset((void *)p, 0xA5, size);  // catch reads of uninitialised memory in tests
    h->liveBlocks++;
    h->liveBytes += size;
    h->allocCalls++;
    if (flags & DAW_MEM_LARGE)
        h->largeBytes += size;
    else
        h->fastBytes += size;
    h->peakBytes = std::max(h->peakBytes, h->liveBytes);
    return (void *)p;
}

void hostFree(void *ctx, void *ptr)
{
    auto *h = static_cast<HostServices *>(ctx);
    if (!ptr)
        return;
    if (!h->allocAllowed)
        h->rtViolations++;
    auto *hdr = reinterpret_cast<AllocHeader *>((uintptr_t)ptr - sizeof(AllocHeader));
    if (hdr->magic != kMagic) {
        fprintf(stderr, "dawhost: free() of a pointer the host did not allocate: %p\n", ptr);
        abort();
    }
    hdr->magic = 0;
    h->liveBlocks--;
    h->liveBytes -= hdr->size;
    if (hdr->flags & DAW_MEM_LARGE)
        h->largeBytes -= hdr->size;
    else
        h->fastBytes -= hdr->size;
    free(hdr->base);
}

void hostLog(void *ctx, int level, const char *msg)
{
    auto *h = static_cast<HostServices *>(ctx);
    if (!h->allocAllowed)
        h->rtViolations++;
    h->logs.emplace_back(msg ? msg : "");
    (void)level;
}

void hostReport(void *ctx, int32_t code)
{
    auto *h = static_cast<HostServices *>(ctx);
    h->reports++;
    h->lastReport = code;
}

}  // namespace

HostServices::HostServices()
{
    api.struct_size = sizeof(daw_host_api_t);
    api.api_major = DAW_PLUGIN_API_MAJOR;
    api.api_minor = DAW_PLUGIN_API_MINOR;
    api.host_ctx = this;
    api.alloc = hostAlloc;
    api.free = hostFree;
    api.log = hostLog;
    api.report = hostReport;
}

const daw_plugin_descriptor_t *find(const std::string &name)
{
    for (auto *d : registry()) {
        std::string id = d->id;
        if (id == name)
            return d;
        auto dot = id.rfind('.');
        if (dot != std::string::npos && id.substr(dot + 1) == name)
            return d;
    }
    return nullptr;
}

static ResourceProvider s_resources = nullptr;
void setDefaultResources(ResourceProvider p) { s_resources = p; }

Instance::Instance(const daw_plugin_descriptor_t *d, uint32_t sampleRate, uint32_t maxBlock)
    : d_(d), sr_(sampleRate), maxBlock_(maxBlock)
{
    if (!daw_plugin_abi_compatible(d))
        return;
    inst_ = d->create(&host_.api, sampleRate, maxBlock);
    if (inst_ && s_resources && daw_plugin_has_resources(d))
        for (uint32_t s = 0; s < d->resource_slot_count; s++)
            if (const daw_resource_t *r = s_resources(d->resource_slots[s]))
                d->set_resource(inst_, s, r);
    if (inst_ && d->activate(inst_) != DAW_OK) {
        d->destroy(inst_);
        inst_ = nullptr;
    }
}

Instance::~Instance()
{
    if (inst_) {
        d_->deactivate(inst_);
        d_->destroy(inst_);
    }
}

int32_t Instance::setResource(uint32_t slot, const daw_resource_t *res)
{
    if (!inst_ || !daw_plugin_has_resources(d_) || slot >= d_->resource_slot_count)
        return DAW_ERR_UNSUPPORTED;
    d_->deactivate(inst_);
    const int32_t r = d_->set_resource(inst_, slot, res);
    d_->activate(inst_);
    return r;
}

void Instance::setParam(uint32_t id, float v)
{
    if (inst_)
        d_->set_param(inst_, id, v);
}

float Instance::getParam(uint32_t id) const { return inst_ ? d_->get_param(inst_, id) : 0.0f; }

bool Instance::loadPreset(uint32_t index)
{
    if (!inst_ || index >= d_->preset_count)
        return false;
    const daw_preset_desc_t &p = d_->presets[index];
    for (uint32_t i = 0; i < d_->param_count; i++)
        setParam(d_->params[i].id, p.values ? p.values[i] : d_->params[i].def);
    return true;
}

void Instance::process(uint32_t frames, const std::vector<daw_event_t> &events, float *outL, float *outR,
                       const float *inL, const float *inR)
{
    if (!inst_)
        return;
    const float *ins[2] = {inL, inR};
    float *outs[2] = {outL, outR};
    daw_process_t c{};
    c.struct_size = sizeof c;
    c.frames = frames;
    c.inputs = d_->kind == DAW_PLUGIN_EFFECT ? ins : nullptr;
    c.outputs = outs;
    c.events = events.empty() ? nullptr : events.data();
    c.event_count = (uint32_t)events.size();
    c.transport_flags = DAW_TRANSPORT_PLAYING;
    c.sample_pos = pos_;
    c.tempo_bpm = tempo_;
    c.song_pos_beats = beatsBase_ + (double)(pos_ - posBase_) / sr_ * (tempo_ / 60.0);
    host_.allocAllowed = false;
    d_->process(inst_, &c);
    host_.allocAllowed = true;
    pos_ += frames;
}

void Instance::setTempo(double bpm)
{
    beatsBase_ += (double)(pos_ - posBase_) / sr_ * (tempo_ / 60.0);
    posBase_ = pos_;
    tempo_ = bpm;
}

daw_event_t noteOn(uint8_t key, float vel, int32_t id, uint16_t flags)
{
    daw_event_t e{};
    e.type = DAW_EV_NOTE_ON;
    e.flags = flags;
    e.u.note.note_id = id;
    e.u.note.key = key;
    e.u.note.velocity = vel;
    return e;
}

daw_event_t noteOff(uint8_t key, int32_t id)
{
    daw_event_t e{};
    e.type = DAW_EV_NOTE_OFF;
    e.u.note.note_id = id;
    e.u.note.key = key;
    return e;
}

daw_event_t paramEv(uint32_t id, float v)
{
    daw_event_t e{};
    e.type = DAW_EV_PARAM;
    e.u.param.id = id;
    e.u.param.value = v;
    return e;
}

RenderResult render(Instance &inst, std::vector<TimedEvent> events, double seconds, uint32_t block,
                    const std::vector<float> *inL, const std::vector<float> *inR)
{
    RenderResult res;
    const uint32_t sr = inst.sampleRate();
    size_t total = (size_t)(seconds * sr);
    res.l.assign(total, 0.0f);
    res.r.assign(total, 0.0f);
    std::stable_sort(events.begin(), events.end(), [](const TimedEvent &a, const TimedEvent &b) { return a.time < b.time; });
    size_t ei = 0;
    std::vector<daw_event_t> blockEvents;
    std::vector<float> zero(block, 0.0f);
    for (size_t pos = 0; pos < total; pos += block) {
        uint32_t n = (uint32_t)std::min<size_t>(block, total - pos);
        blockEvents.clear();
        while (ei < events.size()) {
            size_t at = (size_t)llround(events[ei].time * sr);
            if (at >= pos + n)
                break;
            daw_event_t e = events[ei].ev;
            e.offset = at > pos ? (uint32_t)(at - pos) : 0;
            blockEvents.push_back(e);
            ei++;
        }
        const float *il = inL ? inL->data() + pos : zero.data();
        const float *ir = inR ? inR->data() + pos : zero.data();
        auto t0 = std::chrono::steady_clock::now();
        inst.process(n, blockEvents, res.l.data() + pos, res.r.data() + pos, il, ir);
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        res.processSeconds += dt;
        res.maxBlockSeconds = std::max(res.maxBlockSeconds, dt);
        res.blocks++;
    }
    return res;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool decodeWav(const uint8_t *d, size_t n, AudioFile &out, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        return false;
    };
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
        return fail("not a RIFF / WAVE file");
    uint16_t fmt = 0, chans = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    size_t pcmBytes = 0;
    for (size_t p = 12; p + 8 <= n;) {
        const uint32_t len = le32(d + p + 4);
        const uint8_t *body = d + p + 8;
        const size_t avail = n - (p + 8) < len ? n - (p + 8) : len;
        if (!memcmp(d + p, "fmt ", 4) && avail >= 16) {
            fmt = le16(body);
            chans = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (fmt == 0xFFFE && avail >= 26)  // WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes
                fmt = le16(body + 24);
        } else if (!memcmp(d + p, "data", 4)) {
            pcm = body;
            pcmBytes = avail;
        } else if (!memcmp(d + p, "smpl", 4) && avail >= 36) {
            out.rootKey = (int32_t)le32(body + 12);
            if (le32(body + 28) >= 1 && avail >= 36 + 24) {
                out.loopStart = le32(body + 36 + 8);
                out.loopEnd = le32(body + 36 + 12) + 1;
            }
        }
        p += 8 + len + (len & 1);
    }
    if (!pcm || !rate || (chans != 1 && chans != 2))
        return fail("no data chunk, or not mono / stereo");
    const bool isFloat = fmt == 3 && bits == 32, isPcm = fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!isFloat && !isPcm)
        return fail("unsupported sample format (PCM 8/16/24/32 or float 32)");
    const uint32_t bps = bits / 8, frames = (uint32_t)(pcmBytes / (bps * chans));
    out.channels = chans;
    out.rate = rate;
    for (uint32_t c = 0; c < chans; c++)
        out.ch[c].resize(frames);
    for (uint32_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < chans; c++) {
            const uint8_t *s = pcm + (i * chans + c) * bps;
            float v;
            if (isFloat) {
                uint32_t u = le32(s);
                memcpy(&v, &u, 4);
            } else if (bits == 8)
                v = ((float)s[0] - 128.0f) / 128.0f;
            else if (bits == 16)
                v = (float)(int16_t)le16(s) / 32768.0f;
            else if (bits == 24)
                v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
            else
                v = (float)(int32_t)le32(s) / 2147483648.0f;
            out.ch[c][i] = v;
        }
    if (out.loopEnd > frames)
        out.loopEnd = frames;
    return true;
}

bool readWav(const std::string &path, AudioFile &out, std::string *err)
{
    FileResource f;
    if (!readFile(path, f)) {
        if (err)
            *err = "cannot read " + path;
        return false;
    }
    out.name = path.substr(path.find_last_of('/') + 1);
    return decodeWav(f.bytes.data(), f.bytes.size(), out, err);
}

const daw_resource_t *AudioFile::resource()
{
    for (uint32_t c = 0; c < 2; c++)
        ptr[c] = c < channels ? ch[c].data() : nullptr;
    res = {};
    res.struct_size = sizeof res;
    res.type = DAW_RES_AUDIO;
    res.name = name.c_str();
    res.channels = ptr;
    res.channel_count = channels;
    res.frames = channels ? (uint32_t)ch[0].size() : 0;
    res.sample_rate = rate;
    res.root_key = rootKey;
    res.loop_start = loopStart;
    res.loop_end = loopEnd;
    return &res;
}

bool readFile(const std::string &path, FileResource &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        out.bytes.insert(out.bytes.end(), buf, buf + r);
    fclose(f);
    out.name = path.substr(path.find_last_of('/') + 1);
    return true;
}

const daw_resource_t *FileResource::resource()
{
    res = {};
    res.struct_size = sizeof res;
    res.type = DAW_RES_FILE;
    res.name = name.c_str();
    res.data = bytes.data();
    res.size = (uint32_t)bytes.size();
    res.root_key = -1;
    return &res;
}

std::vector<uint8_t> makeTestSoundFont(double freq, int rootKey, uint32_t rate, double seconds, int filterCents)
{
    std::vector<uint8_t> out;
    auto u16 = [](std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t)x), v.push_back((uint8_t)(x >> 8)); };
    auto u32 = [&](std::vector<uint8_t> &v, uint32_t x) { u16(v, x & 0xFFFF), u16(v, x >> 16); };
    auto name20 = [](std::vector<uint8_t> &v, const char *n) {
        char b[20] = {};
        strncpy(b, n, 19);
        v.insert(v.end(), b, b + 20);
    };
    auto chunk = [&](std::vector<uint8_t> &dst, const char *id, const std::vector<uint8_t> &body) {
        dst.insert(dst.end(), id, id + 4);
        u32(dst, (uint32_t)body.size());
        dst.insert(dst.end(), body.begin(), body.end());
        if (body.size() & 1)
            dst.push_back(0);
    };
    auto list = [&](std::vector<uint8_t> &dst, const char *type, const std::vector<uint8_t> &body) {
        std::vector<uint8_t> b(type, type + 4);
        b.insert(b.end(), body.begin(), body.end());
        chunk(dst, "LIST", b);
    };
    // INFO
    std::vector<uint8_t> info, ifil, name, eng;
    u16(ifil, 2), u16(ifil, 1);
    chunk(info, "ifil", ifil);
    const char *nm = "esp32-daw test font";
    name.assign(nm, nm + strlen(nm) + 1);
    chunk(info, "INAM", name);
    const char *e = "EMU8000";
    eng.assign(e, e + strlen(e) + 1);
    chunk(info, "isng", eng);
    // sdta: a whole number of periods so the loop is seamless, then 46 zero samples (SF2 rule)
    const uint32_t period = (uint32_t)lrint(rate / freq);
    const uint32_t frames = std::max<uint32_t>(period * 4, (uint32_t)(seconds * rate) / period * period);
    std::vector<uint8_t> smpl, sdta;
    for (uint32_t i = 0; i < frames; i++)
        u16(smpl, (uint16_t)(int16_t)lrint(16000 * sin(2 * M_PI * (double)i / period)));
    for (int i = 0; i < 46; i++)
        u16(smpl, 0);
    chunk(sdta, "smpl", smpl);
    // pdta
    std::vector<uint8_t> phdr, pbag, pmod(10, 0), pgen, inst, ibag, imod(10, 0), igen, shdr, pdta;
    name20(phdr, "Test Sine"), u16(phdr, 0), u16(phdr, 0), u16(phdr, 0), u32(phdr, 0), u32(phdr, 0), u32(phdr, 0);
    name20(phdr, "EOP"), u16(phdr, 0), u16(phdr, 0), u16(phdr, 1), u32(phdr, 0), u32(phdr, 0), u32(phdr, 0);
    u16(pbag, 0), u16(pbag, 0);  // preset zone 0: generators from 0
    u16(pbag, 1), u16(pbag, 0);  // terminal
    u16(pgen, 41), u16(pgen, 0);  // instrument 0
    u16(pgen, 0), u16(pgen, 0);   // terminal
    name20(inst, "Sine"), u16(inst, 0);
    name20(inst, "EOI"), u16(inst, 1);
    u16(ibag, 0), u16(ibag, 0);
    u16(ibag, filterCents > 0 ? 4 : 3), u16(ibag, 0);
    u16(igen, 54), u16(igen, 1);  // sampleModes: loop continuously
    u16(igen, 58), u16(igen, (uint32_t)rootKey);  // overridingRootKey
    if (filterCents > 0)
        u16(igen, 8), u16(igen, (uint32_t)filterCents);  // initialFilterFc
    u16(igen, 53), u16(igen, 0);  // sampleID (last)
    u16(igen, 0), u16(igen, 0);   // terminal
    name20(shdr, "sine"), u32(shdr, 0), u32(shdr, frames), u32(shdr, 0), u32(shdr, frames), u32(shdr, rate);
    shdr.push_back((uint8_t)rootKey), shdr.push_back(0), u16(shdr, 0), u16(shdr, 1);
    name20(shdr, "EOS"), u32(shdr, 0), u32(shdr, 0), u32(shdr, 0), u32(shdr, 0), u32(shdr, 0);
    shdr.push_back(0), shdr.push_back(0), u16(shdr, 0), u16(shdr, 0);
    chunk(pdta, "phdr", phdr), chunk(pdta, "pbag", pbag), chunk(pdta, "pmod", pmod), chunk(pdta, "pgen", pgen);
    chunk(pdta, "inst", inst), chunk(pdta, "ibag", ibag), chunk(pdta, "imod", imod), chunk(pdta, "igen", igen);
    chunk(pdta, "shdr", shdr);
    std::vector<uint8_t> body = {'s', 'f', 'b', 'k'};
    list(body, "INFO", info);
    list(body, "sdta", sdta);
    list(body, "pdta", pdta);
    chunk(out, "RIFF", body);
    return out;
}

bool writeWav(const std::string &path, const std::vector<float> &l, const std::vector<float> &r, uint32_t sr)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    uint32_t frames = (uint32_t)std::min(l.size(), r.size());
    uint32_t data = frames * 2 * 3;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, sr);
    put32(f, sr * 6);
    put16(f, 6);
    put16(f, 24);
    fwrite("data", 1, 4, f);
    put32(f, data);
    for (uint32_t i = 0; i < frames; i++) {
        for (float s : {l[i], r[i]}) {
            float c = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
            int32_t v = (int32_t)lrintf(c * 8388607.0f);
            uint8_t b[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
            fwrite(b, 1, 3, f);
        }
    }
    return fclose(f) == 0;
}

uint64_t hashPcm(const std::vector<float> &l, const std::vector<float> &r)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t n = std::min(l.size(), r.size());
    for (size_t i = 0; i < n; i++) {
        for (float s : {l[i], r[i]}) {
            float c = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
            uint32_t v = (uint32_t)(int32_t)lrintf(c * 8388607.0f) & 0xFFFFFFu;
            for (int k = 0; k < 3; k++) {
                h ^= (v >> (8 * k)) & 0xFF;
                h *= 0x100000001B3ull;
            }
        }
    }
    return h;
}

double rms(const std::vector<float> &x)
{
    double s = 0;
    for (float v : x)
        s += (double)v * v;
    return x.empty() ? 0 : sqrt(s / x.size());
}

float peak(const std::vector<float> &x)
{
    float p = 0;
    for (float v : x)
        p = std::max(p, fabsf(v));
    return p;
}

std::vector<TimedEvent> sequenceScale(uint8_t root, int notes, double step, double gate, float vel)
{
    static const int major[] = {0, 2, 4, 5, 7, 9, 11, 12};
    std::vector<TimedEvent> ev;
    for (int i = 0; i < notes; i++) {
        uint8_t k = (uint8_t)(root + major[i % 8] + 12 * (i / 8));
        ev.push_back({i * step, noteOn(k, vel, i)});
        ev.push_back({i * step + gate, noteOff(k, i)});
    }
    return ev;
}

std::vector<TimedEvent> sequenceChords(uint8_t root, int chords, double len)
{
    static const int prog[][3] = {{0, 4, 7}, {-3, 0, 4}, {-7, -3, 0}, {-5, -1, 2}};
    std::vector<TimedEvent> ev;
    int id = 0;
    for (int c = 0; c < chords; c++)
        for (int n = 0; n < 3; n++) {
            uint8_t k = (uint8_t)(root + prog[c % 4][n]);
            ev.push_back({c * len, noteOn(k, 0.8f, id)});
            ev.push_back({c * len + len * 0.9, noteOff(k, id)});
            id++;
        }
    return ev;
}

std::vector<TimedEvent> sequenceDrums(int bars, double bpm)
{
    std::vector<TimedEvent> ev;
    double s16 = 60.0 / bpm / 4.0;
    int id = 0;
    for (int b = 0; b < bars; b++)
        for (int s = 0; s < 16; s++) {
            double t = (b * 16 + s) * s16;
            auto hit = [&](uint8_t key, float vel) {
                ev.push_back({t, noteOn(key, vel, id)});
                ev.push_back({t + s16 * 0.5, noteOff(key, id)});
                id++;
            };
            if (s % 4 == 0)
                hit(36, 1.0f);
            if (s == 4 || s == 12)
                hit(38, 0.9f);
            if (s % 2 == 0)
                hit(42, s % 4 == 2 ? 0.8f : 0.5f);
            if (s == 14)
                hit(46, 0.7f);
            if (s == 12 && b % 2 == 1)
                hit(39, 0.8f);
        }
    return ev;
}

static std::string jsonEsc(const char *s)
{
    std::string o = "\"";
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else
            o += (char)c;
    }
    return o + "\"";
}

static std::string num(double v)
{
    char b[40];
    snprintf(b, sizeof b, "%.9g", v);
    return b;
}

std::string descriptorJson(const daw_plugin_descriptor_t *d, const char *arch)
{
    static const char *types[] = {"float", "int", "bool", "enum"};
    static const char *scales[] = {"linear", "log", "db", "bipolar"};
    static const struct {
        uint64_t bit;
        const char *name;
    } caps[] = {{DAW_CAP_POLYPHONIC, "polyphonic"}, {DAW_CAP_VELOCITY, "velocity"},       {DAW_CAP_SLIDE, "slide"},
                {DAW_CAP_ACCENT, "accent"},         {DAW_CAP_NOTE_PAN, "perNotePan"},     {DAW_CAP_NOTE_PITCH, "perNotePitch"},
                {DAW_CAP_NOTE_VOLUME, "perNoteVolume"}, {DAW_CAP_NOTE_CUTOFF, "perNoteCutoff"},
                {DAW_CAP_NOTE_RESONANCE, "perNoteResonance"}, {DAW_CAP_RELEASE_VEL, "releaseVelocity"},
                {DAW_CAP_PITCH_BEND, "pitchBend"}, {DAW_CAP_SUSTAIN, "sustain"},         {DAW_CAP_DRUM_KIT, "drumKit"},
                {DAW_CAP_TAIL, "tail"},             {DAW_CAP_MIDI_OUT, "midiOut"},        {DAW_CAP_STATE_BLOB, "stateBlob"},
                {DAW_CAP_MONO_LEGATO, "monoLegato"}};
    std::ostringstream o;
    o << "{\n  \"schema\": 1,\n  \"id\": " << jsonEsc(d->id) << ",\n  \"name\": " << jsonEsc(d->name)
      << ",\n  \"vendor\": " << jsonEsc(d->vendor) << ",\n  \"version\": " << jsonEsc(d->version)
      << ",\n  \"license\": " << jsonEsc(d->license) << ",\n  \"description\": " << jsonEsc(d->description)
      << ",\n  \"category\": " << jsonEsc(d->category)
      << ",\n  \"type\": \"" << (d->kind == DAW_PLUGIN_INSTRUMENT ? "instrument" : "effect") << "\""
      << ",\n  \"apiVersion\": " << d->api_major << ",\n  \"apiMinor\": " << d->api_minor
      << ",\n  \"stateVersion\": " << d->state_version << ",\n  \"maxVoices\": " << d->max_voices
      << ",\n  \"capabilities\": {";
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        o << (i ? ", " : "") << "\"" << caps[i].name << "\": " << ((d->capabilities & caps[i].bit) ? "true" : "false");
    o << "},\n  \"resources\": {\"memFastBytes\": " << d->mem_fast_bytes << ", \"memLargeBytes\": " << d->mem_large_bytes
      << ", \"cpuPermilleBase\": " << d->cpu_permille_base << ", \"cpuPermilleVoice\": " << d->cpu_permille_voice << "}";
    if (arch)
        o << ",\n  \"arch\": " << jsonEsc(arch);
    o << ",\n  \"parameters\": [";
    for (uint32_t i = 0; i < d->param_count; i++) {
        const daw_param_desc_t &p = d->params[i];
        o << (i ? "," : "") << "\n    {\"id\": " << p.id << ", \"key\": " << jsonEsc(p.key) << ", \"display\": " << jsonEsc(p.name)
          << ", \"group\": " << jsonEsc(p.group ? p.group : "") << ", \"unit\": " << jsonEsc(p.unit ? p.unit : "")
          << ", \"type\": \"" << types[p.type & 3] << "\", \"scale\": \"" << scales[p.scale & 3] << "\", \"min\": " << num(p.min)
          << ", \"max\": " << num(p.max) << ", \"default\": " << num(p.def)
          << ", \"automatable\": " << ((p.flags & DAW_PARAM_AUTOMATABLE) ? "true" : "false")
          << ", \"hidden\": " << ((p.flags & DAW_PARAM_HIDDEN) ? "true" : "false");
        if (p.type == DAW_PARAM_ENUM) {
            o << ", \"labels\": [";
            for (uint32_t k = 0; k < p.enum_count; k++)
                o << (k ? ", " : "") << jsonEsc(p.enum_labels[k]);
            o << "]";
        }
        o << "}";
    }
    o << "\n  ],\n  \"presets\": [";
    for (uint32_t i = 0; i < d->preset_count; i++)
        o << (i ? ", " : "") << "{\"name\": " << jsonEsc(d->presets[i].name) << ", \"category\": "
          << jsonEsc(d->presets[i].category ? d->presets[i].category : "") << "}";
    o << "]";
    if (d->key_names) {
        o << ",\n  \"keyNames\": {";
        bool first = true;
        for (int k = 0; k < 128; k++)
            if (d->key_names[k]) {
                o << (first ? "" : ", ") << "\"" << k << "\": " << jsonEsc(d->key_names[k]);
                first = false;
            }
        o << "}";
    }
    if (daw_plugin_has_resources(d)) {
        o << ",\n  \"resourceSlots\": [";
        for (uint32_t i = 0; i < d->resource_slot_count; i++) {
            const auto &r = d->resource_slots[i];
            o << (i ? ", " : "") << "{\"key\": " << jsonEsc(r.key) << ", \"name\": " << jsonEsc(r.name) << ", \"types\": [";
            bool first = true;
            if (r.types & DAW_RES_MASK(DAW_RES_AUDIO))
                o << "\"audio\"", first = false;
            if (r.types & DAW_RES_MASK(DAW_RES_FILE))
                o << (first ? "" : ", ") << "\"file\"";
            o << "], \"extensions\": " << jsonEsc(r.extensions ? r.extensions : "") << "}";
        }
        o << "]";
    }
    o << "\n}\n";
    return o.str();
}

}  // namespace dawhost
