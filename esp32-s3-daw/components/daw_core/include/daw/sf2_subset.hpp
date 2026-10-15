// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "daw/sf2_info.hpp"

namespace daw {
struct Sf2Span { uint32_t source, bytes; };
// A regular SF2 containing only the requested presets. Its PCM is copied straight from flash;
// the complete source font is never buffered. Shared/overlapping samples are copied once.
struct Sf2Subset {
    std::vector<uint8_t> prefix, suffix;
    std::vector<Sf2Span> samples;
    uint32_t pcmBytes = 0;
    uint32_t bytes() const { return (uint32_t)prefix.size() + pcmBytes + (uint32_t)suffix.size(); }
    bool copy(const Sf2Reader &read, uint8_t *out, size_t capacity, std::string *err) const;
};
bool sf2Subset(const Sf2Reader &read, uint32_t size, const std::vector<Sf2Preset> &presets,
               Sf2Subset &out, std::string *err);
}  // namespace daw
