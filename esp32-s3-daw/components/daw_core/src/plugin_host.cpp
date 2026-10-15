// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/plugin_host.hpp"

#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "daw/platform.hpp"

extern "C" {
// generated registry (plugins/generated/daw_plugin_registry.c)
extern const daw_plugin_entry_fn daw_plugin_entries[];
extern const size_t daw_plugin_entry_count;
}

namespace daw {

static const char *TAG = "plugins";

bool PluginRegistry::compatible(const daw_plugin_descriptor_t *d)
{
    if (!daw_plugin_abi_compatible(d) || !d->id || !d->create || !d->process)
        return false;
    if (d->kind != DAW_PLUGIN_INSTRUMENT && d->kind != DAW_PLUGIN_EFFECT)
        return false;
    for (uint32_t i = 0; i < d->param_count; i++)
        if (!d->params[i].key || !(d->params[i].min < d->params[i].max))
            return false;
    return true;
}

const std::vector<const daw_plugin_descriptor_t *> &PluginRegistry::all()
{
    static std::vector<const daw_plugin_descriptor_t *> list = [] {
        std::vector<const daw_plugin_descriptor_t *> v;
        for (size_t i = 0; i < daw_plugin_entry_count; i++) {
            const daw_plugin_descriptor_t *d = daw_plugin_entries[i]();
            if (compatible(d))
                v.push_back(d);
            else
                DAW_LOGE(TAG, "plugin entry %u rejected: incompatible ABI or descriptor", (unsigned)i);
        }
        return v;
    }();
    return list;
}

const daw_plugin_descriptor_t *PluginRegistry::find(const std::string &id)
{
    for (auto *d : all())
        if (id == d->id)
            return d;
    return nullptr;
}

// ---- host services ------------------------------------------------------------------------

namespace {
struct Hdr {
    void *base;
    uint32_t size;
    uint32_t flags;
};
}  // namespace

void *PluginInstance::hostAlloc(void *ctx, size_t size, size_t align, uint32_t flags)
{
    auto *self = static_cast<PluginInstance *>(ctx);
    if (align < 8)
        align = 8;
    if (align > 64 || (align & (align - 1)) || size > 64u * 1024u * 1024u)
        return nullptr;
    const size_t total = size + align + sizeof(Hdr);
    bool fast = !(flags & DAW_MEM_LARGE);
    // Dynamic plugin allocations must preserve the same internal reserve as admission.
    // In particular, a temporary Tracker probe must not consume a second 23 KB voice array
    // from RAM already needed by Wi-Fi / DMA; the existing LARGE fallback handles it.
    bool reserve = false;
    if (fast) {
        const auto memory = platform::memInfo();
        reserve = memory.internalTotal && memory.internalFree < total + 32 * 1024;
    }
    void *base = reserve ? nullptr : platform::alloc(total, 8, fast ? platform::Mem::Fast : platform::Mem::Large);
    if (!base && fast) {
        base = platform::alloc(total, 8, platform::Mem::Large);
        if (base) {
            DAW_LOGW(TAG, "%s: %u B of FAST memory not available, using LARGE (slower)", self->desc_->id, (unsigned)size);
            fast = false;
        }
    }
    if (!base)
        return nullptr;
    uintptr_t p = ((uintptr_t)base + sizeof(Hdr) + align - 1) & ~(uintptr_t)(align - 1);
    Hdr *h = reinterpret_cast<Hdr *>(p - sizeof(Hdr));
    h->base = base;
    h->size = (uint32_t)size;
    h->flags = fast ? 1u : 2u;
    if (flags & DAW_MEM_ZERO)
        memset((void *)p, 0, size);
    (fast ? self->memFast_ : self->memLarge_) += size;
    return (void *)p;
}

void PluginInstance::hostFree(void *ctx, void *ptr)
{
    if (!ptr)
        return;
    auto *self = static_cast<PluginInstance *>(ctx);
    Hdr *h = reinterpret_cast<Hdr *>((uintptr_t)ptr - sizeof(Hdr));
    (h->flags == 1 ? self->memFast_ : self->memLarge_) -= h->size;
    platform::free(h->base);
}

void PluginInstance::hostLog(void *ctx, int level, const char *msg)
{
    auto *self = static_cast<PluginInstance *>(ctx);
    platform::log(level <= DAW_LOG_ERROR ? platform::Log::Error : level == DAW_LOG_WARN ? platform::Log::Warn : platform::Log::Info,
                  self->desc_->id, "%s", msg ? msg : "");
}

void PluginInstance::hostReport(void *ctx, int32_t code)
{
    auto *self = static_cast<PluginInstance *>(ctx);
    self->reports.fetch_add(1, std::memory_order_relaxed);
    self->lastReport.store(code, std::memory_order_relaxed);
}

namespace {
PluginInstance::ParallelService parallelService = nullptr;
void *parallelContext = nullptr;
}
void PluginInstance::setParallelService(ParallelService service, void *context)
{
    parallelService = service;
    parallelContext = context;
}
int32_t PluginInstance::hostParallel(void *ctx, void (*local)(void *), void *localArg,
                                      void (*remote)(void *), void *remoteArg)
{
    auto *self = static_cast<PluginInstance *>(ctx);
    const int32_t ran = parallelService ? parallelService(parallelContext, local, localArg, remote, remoteArg) : 0;
    (ran ? self->parallelJobs : self->parallelDeclined).fetch_add(1, std::memory_order_relaxed);
    return ran;
}

// ---- instances ----------------------------------------------------------------------------

PluginInstance *PluginInstance::create(const daw_plugin_descriptor_t *d, uint32_t sampleRate, uint32_t maxBlock, std::string *err)
{
    if (!d || !PluginRegistry::compatible(d)) {
        if (err)
            *err = "incompatible plugin";
        return nullptr;
    }
    auto *p = new PluginInstance();
    p->desc_ = d;
    p->api_.struct_size = sizeof(daw_host_api_t);
    p->api_.api_major = DAW_PLUGIN_API_MAJOR;
    p->api_.api_minor = DAW_PLUGIN_API_MINOR;
    p->api_.host_ctx = p;
    p->api_.alloc = hostAlloc;
    p->api_.free = hostFree;
    p->api_.log = hostLog;
    p->api_.report = hostReport;
    p->api_.parallel = hostParallel;
    p->values_.resize(d->param_count);
    for (uint32_t i = 0; i < d->param_count; i++)
        p->values_[i] = d->params[i].def;
    p->inst_ = d->create(&p->api_, sampleRate, maxBlock);
    if (!p->inst_) {
        if (err)
            *err = std::string(d->id) + ": create failed (out of memory?)";
        delete p;
        return nullptr;
    }
    for (uint32_t i = 0; i < d->param_count; i++)
        p->values_[i] = d->get_param(p->inst_, d->params[i].id);
    return p;
}

void PluginInstance::destroy(PluginInstance *p)
{
    if (!p)
        return;
    if (p->inst_) {
        p->desc_->deactivate(p->inst_);
        p->desc_->destroy(p->inst_);
    }
    if (p->memFast_ || p->memLarge_)
        DAW_LOGW(TAG, "%s leaked %u B fast, %u B large", p->desc_->id, (unsigned)p->memFast_.load(), (unsigned)p->memLarge_.load());
    delete p;
}

int PluginInstance::paramIndex(const std::string &key) const
{
    for (uint32_t i = 0; i < desc_->param_count; i++)
        if (key == desc_->params[i].key)
            return (int)i;
    return -1;
}

float PluginInstance::clampParam(uint32_t index, float v) const
{
    const daw_param_desc_t &p = desc_->params[index];
    if (!(v >= p.min))
        v = p.min;
    if (v > p.max)
        v = p.max;
    if (p.type != DAW_PARAM_FLOAT)
        v = roundf(v);
    return v;
}

bool PluginInstance::setParam(const std::string &key, float v, uint32_t *idOut, float *clamped)
{
    int i = paramIndex(key);
    if (i < 0)
        return false;
    v = clampParam((uint32_t)i, v);
    if (idOut)
        *idOut = desc_->params[i].id;
    if (clamped)
        *clamped = v;
    values_[(size_t)i] = v;
    return true;
}

void PluginInstance::setParamById(uint32_t id, float v)
{
    for (uint32_t i = 0; i < desc_->param_count; i++)
        if (desc_->params[i].id == id) {
            v = clampParam(i, v);
            values_[i] = v;
            desc_->set_param(inst_, id, v);
            return;
        }
}

bool PluginInstance::presetValues(const std::string &name, std::vector<float> &out) const
{
    for (uint32_t i = 0; i < desc_->preset_count; i++) {
        const daw_preset_desc_t &pr = desc_->presets[i];
        if (name == pr.name) {
            out.resize(desc_->param_count);
            for (uint32_t k = 0; k < desc_->param_count; k++)
                out[k] = pr.values ? pr.values[k] : desc_->params[k].def;
            return true;
        }
    }
    return false;
}

void PluginInstance::applyState(const PluginState &st)
{
    std::vector<float> preset;
    if (!st.preset.empty() && presetValues(st.preset, preset))
        for (uint32_t k = 0; k < desc_->param_count; k++)
            setParamById(desc_->params[k].id, preset[k]);
    for (const auto &kv : st.params) {
        int i = paramIndex(kv.first);
        if (i >= 0 && !(desc_->params[i].flags & DAW_PARAM_READONLY))  // read-only: the plugin's own
            setParamById(desc_->params[i].id, kv.second);
    }
    // opaque state: base64 decoded by the session before this call is not needed for the bundled
    // plugins (none declares DAW_CAP_STATE_BLOB yet); kept verbatim in the project
}

int PluginInstance::resourceSlot(const std::string &key) const
{
    if (!daw_plugin_has_resources(desc_))
        return -1;
    for (uint32_t i = 0; i < desc_->resource_slot_count; i++)
        if (desc_->resource_slots[i].key && key == desc_->resource_slots[i].key)
            return (int)i;
    return -1;
}

bool PluginInstance::setResource(int slot, std::shared_ptr<const LoadedResource> res)
{
    if (slot < 0 || !daw_plugin_has_resources(desc_) || (uint32_t)slot >= desc_->resource_slot_count)
        return false;
    if (desc_->set_resource(inst_, (uint32_t)slot, res ? res->resource() : nullptr) != DAW_OK)
        return false;
    if (res)
        keep_.push_back(std::move(res));  // alive until this instance is destroyed
    return true;
}

const LoadedResource *PluginInstance::retainedResource(const std::string &name) const
{
    for (auto it = keep_.rbegin(); it != keep_.rend(); ++it) if ((*it)->name() == name) return it->get();
    return nullptr;
}

void PluginInstance::captureState(PluginState &st) const
{
    st.pluginId = desc_->id;
    st.version = desc_->version;
    st.params.clear();
    for (uint32_t i = 0; i < desc_->param_count; i++)
        if (!(desc_->params[i].flags & DAW_PARAM_READONLY))
            st.params.emplace_back(desc_->params[i].key, values_[i]);
    st.missing = false;
    st.rawJson.clear();
}

// ---- catalog ------------------------------------------------------------------------------

cJSON *pluginDescriptorJson(const daw_plugin_descriptor_t *d)
{
    static const char *types[] = {"float", "int", "bool", "enum"};
    static const char *scales[] = {"linear", "log", "db", "bipolar"};
    static const struct {
        uint64_t bit;
        const char *name;
    } caps[] = {{DAW_CAP_POLYPHONIC, "polyphonic"},   {DAW_CAP_VELOCITY, "velocity"},        {DAW_CAP_SLIDE, "slide"},
                {DAW_CAP_ACCENT, "accent"},           {DAW_CAP_NOTE_PAN, "perNotePan"},      {DAW_CAP_NOTE_PITCH, "perNotePitch"},
                {DAW_CAP_NOTE_VOLUME, "perNoteVolume"}, {DAW_CAP_NOTE_CUTOFF, "perNoteCutoff"}, {DAW_CAP_RELEASE_VEL, "releaseVelocity"},
                {DAW_CAP_PITCH_BEND, "pitchBend"},    {DAW_CAP_DRUM_KIT, "drumKit"},         {DAW_CAP_TAIL, "tail"},
                {DAW_CAP_MONO_LEGATO, "monoLegato"}, {DAW_CAP_MIDI_STREAM, "midiStream"}, {DAW_CAP_NOTE_CHASE, "noteChase"}};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", d->id);
    cJSON_AddStringToObject(o, "name", d->name);
    cJSON_AddStringToObject(o, "vendor", d->vendor ? d->vendor : "");
    cJSON_AddStringToObject(o, "version", d->version ? d->version : "");
    cJSON_AddStringToObject(o, "license", d->license ? d->license : "");
    cJSON_AddStringToObject(o, "description", d->description ? d->description : "");
    cJSON_AddStringToObject(o, "category", d->category ? d->category : "");
    cJSON_AddStringToObject(o, "type", d->kind == DAW_PLUGIN_INSTRUMENT ? "instrument" : "effect");
    cJSON_AddNumberToObject(o, "maxVoices", d->max_voices);
    cJSON *c = cJSON_AddObjectToObject(o, "capabilities");
    for (const auto &cap : caps)
        cJSON_AddBoolToObject(c, cap.name, (d->capabilities & cap.bit) != 0);
    cJSON *r = cJSON_AddObjectToObject(o, "resources");
    cJSON_AddNumberToObject(r, "memFastBytes", d->mem_fast_bytes);
    cJSON_AddNumberToObject(r, "memLargeBytes", d->mem_large_bytes);
    cJSON_AddNumberToObject(r, "cpuPermilleBase", d->cpu_permille_base);
    cJSON_AddNumberToObject(r, "cpuPermilleVoice", d->cpu_permille_voice);
    cJSON *ps = cJSON_AddArrayToObject(o, "params");
    for (uint32_t i = 0; i < d->param_count; i++) {
        const daw_param_desc_t &p = d->params[i];
        if (p.flags & DAW_PARAM_HIDDEN)
            continue;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", p.id);
        cJSON_AddStringToObject(j, "key", p.key);
        cJSON_AddStringToObject(j, "name", p.name);
        cJSON_AddStringToObject(j, "group", p.group ? p.group : "");
        cJSON_AddStringToObject(j, "unit", p.unit ? p.unit : "");
        cJSON_AddStringToObject(j, "type", types[p.type & 3]);
        cJSON_AddStringToObject(j, "scale", scales[p.scale & 3]);
        cJSON_AddNumberToObject(j, "min", p.min);
        cJSON_AddNumberToObject(j, "max", p.max);
        cJSON_AddNumberToObject(j, "def", p.def);
        cJSON_AddBoolToObject(j, "auto", (p.flags & DAW_PARAM_AUTOMATABLE) != 0);
        if (p.type == DAW_PARAM_ENUM && p.enum_labels) {
            cJSON *l = cJSON_AddArrayToObject(j, "labels");
            for (uint32_t k = 0; k < p.enum_count; k++)
                cJSON_AddItemToArray(l, cJSON_CreateString(p.enum_labels[k] ? p.enum_labels[k] : ""));
        }
        cJSON_AddItemToArray(ps, j);
    }
    cJSON *pr = cJSON_AddArrayToObject(o, "presets");
    for (uint32_t i = 0; i < d->preset_count; i++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", d->presets[i].name);
        cJSON_AddStringToObject(j, "category", d->presets[i].category ? d->presets[i].category : "");
        cJSON_AddItemToArray(pr, j);
    }
    if (d->key_names) {
        cJSON *k = cJSON_AddObjectToObject(o, "keyNames");
        for (int i = 0; i < 128; i++)
            if (d->key_names[i]) {
                char b[8];
                snprintf(b, sizeof b, "%d", i);
                cJSON_AddStringToObject(k, b, d->key_names[i]);
            }
    }
    if (daw_plugin_has_resources(d)) {
        cJSON *rs = cJSON_AddArrayToObject(o, "resourceSlots");
        for (uint32_t i = 0; i < d->resource_slot_count; i++) {
            const auto &s = d->resource_slots[i];
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "key", s.key ? s.key : "");
            cJSON_AddStringToObject(j, "name", s.name ? s.name : "");
            cJSON_AddBoolToObject(j, "audio", (s.types & DAW_RES_MASK(DAW_RES_AUDIO)) != 0);
            cJSON_AddBoolToObject(j, "file", (s.types & DAW_RES_MASK(DAW_RES_FILE)) != 0);
            cJSON_AddStringToObject(j, "extensions", s.extensions ? s.extensions : "");
            cJSON_AddItemToArray(rs, j);
        }
    }
    return o;
}

cJSON *pluginCatalogJson()
{
    cJSON *a = cJSON_CreateArray();
    for (auto *d : PluginRegistry::all())
        cJSON_AddItemToArray(a, pluginDescriptorJson(d));
    return a;
}

}  // namespace daw
