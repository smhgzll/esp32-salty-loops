// SPDX-License-Identifier: GPL-3.0-or-later
// User presets on the device: one small JSON file per preset ({"plugin", "name", "params"}) in a
// SampleStore on the "presets" partition (the same power-safe store as the sample library),
// indexed in RAM at boot. File names are "<8 hex of the plugin id's CRC>_<preset name>", so two
// plugins can each have a preset called "Pad".
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "daw/sample_store.hpp"

struct cJSON;

namespace daw {

struct UserPreset {
    std::string plugin, name, file;
    std::vector<std::pair<std::string, float>> params;  // parameter key -> plain value
};

class PresetLibrary {
public:
    static constexpr size_t kMaxName = 30;
    explicit PresetLibrary(SampleStore &store) : store_(store) {}
    void mount();
    const std::vector<UserPreset> &list() const { return presets_; }
    const UserPreset *find(const std::string &plugin, const std::string &name) const;
    bool save(const std::string &plugin, const std::string &name, const std::vector<std::pair<std::string, float>> &params, std::string *err);
    bool remove(const std::string &plugin, const std::string &name);
    cJSON *json() const;  // [{"plugin", "name"}]

private:
    static std::string fileName(const std::string &plugin, const std::string &name);
    SampleStore &store_;
    std::vector<UserPreset> presets_;
};

}  // namespace daw
