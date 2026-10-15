// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/preset_library.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "cJSON.h"
#include "daw/model.hpp"  // crc32

namespace daw {

std::string PresetLibrary::fileName(const std::string &plugin, const std::string &name)
{
    char h[10];
    snprintf(h, sizeof h, "%08x", (unsigned)crc32(plugin.data(), plugin.size()));
    return std::string(h) + "_" + sanitizeSampleName(name).substr(0, kMaxName);
}

void PresetLibrary::mount()
{
    store_.mount();
    presets_.clear();
    for (const auto &f : store_.list()) {
        if (f.type != SampleType::Preset)
            continue;
        std::vector<uint8_t> bytes;
        if (!store_.read(f.name, bytes, nullptr))
            continue;
        cJSON *j = cJSON_ParseWithLength((const char *)bytes.data(), bytes.size());
        const cJSON *pl = cJSON_GetObjectItemCaseSensitive(j, "plugin"), *nm = cJSON_GetObjectItemCaseSensitive(j, "name");
        if (cJSON_IsString(pl) && cJSON_IsString(nm)) {
            UserPreset p;
            p.plugin = pl->valuestring;
            p.name = nm->valuestring;
            p.file = f.name;
            const cJSON *kv;
            cJSON_ArrayForEach(kv, cJSON_GetObjectItemCaseSensitive(j, "params"))
            {
                if (cJSON_IsNumber(kv) && kv->string && isfinite(kv->valuedouble))
                    p.params.emplace_back(kv->string, (float)kv->valuedouble);
            }
            presets_.push_back(std::move(p));
        }
        cJSON_Delete(j);
    }
    std::sort(presets_.begin(), presets_.end(), [](const UserPreset &a, const UserPreset &b) { return a.plugin != b.plugin ? a.plugin < b.plugin : a.name < b.name; });
}

const UserPreset *PresetLibrary::find(const std::string &plugin, const std::string &name) const
{
    for (const auto &p : presets_)
        if (p.plugin == plugin && p.name == name)
            return &p;
    return nullptr;
}

bool PresetLibrary::save(const std::string &plugin, const std::string &rawName, const std::vector<std::pair<std::string, float>> &params, std::string *err)
{
    const std::string name = sanitizeSampleName(rawName).substr(0, kMaxName);
    if (name.empty() || plugin.empty()) {
        if (err)
            *err = "a preset needs a name";
        return false;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "plugin", plugin.c_str());
    cJSON_AddStringToObject(j, "name", name.c_str());
    cJSON *ps = cJSON_AddObjectToObject(j, "params");
    for (const auto &kv : params)
        cJSON_AddNumberToObject(ps, kv.first.c_str(), kv.second);
    char *t = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    const std::string file = fileName(plugin, name);
    const bool ok = t && store_.putRaw(file, SampleType::Preset, (const uint8_t *)t, (uint32_t)strlen(t), err);
    cJSON_free(t);
    if (!ok)
        return false;
    presets_.erase(std::remove_if(presets_.begin(), presets_.end(), [&](const UserPreset &p) { return p.plugin == plugin && p.name == name; }), presets_.end());
    presets_.push_back({plugin, name, file, params});
    std::sort(presets_.begin(), presets_.end(), [](const UserPreset &a, const UserPreset &b) { return a.plugin != b.plugin ? a.plugin < b.plugin : a.name < b.name; });
    return true;
}

bool PresetLibrary::remove(const std::string &plugin, const std::string &name)
{
    for (size_t i = 0; i < presets_.size(); i++)
        if (presets_[i].plugin == plugin && presets_[i].name == name) {
            if (!store_.remove(presets_[i].file))
                return false;
            presets_.erase(presets_.begin() + (long)i);
            return true;
        }
    return false;
}

cJSON *PresetLibrary::json() const
{
    cJSON *a = cJSON_CreateArray();
    for (const auto &p : presets_) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "plugin", p.plugin.c_str());
        cJSON_AddStringToObject(o, "name", p.name.c_str());
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

}  // namespace daw
