// SPDX-License-Identifier: GPL-3.0-or-later
// The preset list of a SoundFont 2 file (its PHDR records), read in small pieces so a large font
// in the sample library is never loaded whole for it.
#pragma once
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace daw {

struct Sf2Preset {
    std::string name;  // printable ASCII, at most 20 characters
    uint16_t bank = 0, program = 0;
};

// read(offset, buf, len) returns false outside the file. Presets come sorted by bank, program.
using Sf2Reader = std::function<bool(uint32_t offset, void *buf, uint32_t len)>;
bool sf2Presets(const Sf2Reader &read, uint32_t size, std::vector<Sf2Preset> &out, std::string *err);

constexpr uint32_t kMaxSf2Presets = 1024;

}  // namespace daw
