// SPDX-License-Identifier: GPL-3.0-or-later
// Decoding sample files for plugins (ABI 1.1 resources) and the shared, reference-counted copies
// the session hands to plugin instances. Decoding runs on the control thread, never on the audio path.
#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "daw_plugin_api.h"

namespace daw {

struct DecodedAudio {
    std::vector<float> ch[2];
    uint32_t channels = 0, rate = 0;
    int32_t rootKey = -1;
    uint32_t loopStart = 0, loopEnd = 0;
    uint32_t frames() const { return channels ? (uint32_t)ch[0].size() : 0; }
};

// RIFF / WAVE: PCM 8, 16, 24, 32 bit or IEEE float 32, mono or stereo (WAVE_FORMAT_EXTENSIBLE too);
// root key and first loop from a 'smpl' chunk. Files with more channels keep the first two.
bool decodeWav(const uint8_t *data, size_t size, DecodedAudio &out, std::string *err);

// a resource as a plugin sees it, plus the memory behind it (immutable once built)
class LoadedResource {
public:
    using FileBytes = std::unique_ptr<uint8_t, void (*)(void *)>;
    struct FileBuffer {
        FileBytes data;
        uint32_t capacity;
    };
    static std::shared_ptr<LoadedResource> audio(const std::string &name, DecodedAudio &&a);
    static std::shared_ptr<LoadedResource> file(const std::string &name, std::vector<uint8_t> &&bytes);
    static std::shared_ptr<LoadedResource> file(const std::string &name, FileBytes bytes, uint32_t size);
    static std::shared_ptr<LoadedResource> file(const std::string &name, std::shared_ptr<const FileBuffer> buffer, uint32_t size);
    const daw_resource_t *resource() const { return &res_; }
    const std::string &name() const { return name_; }
    size_t bytes() const;

private:
    std::string name_;
    DecodedAudio audio_;
    std::vector<uint8_t> file_;
    std::shared_ptr<const FileBuffer> ownedFile_;
    uint32_t ownedSize_ = 0;
    const float *ptr_[2] = {nullptr, nullptr};
    daw_resource_t res_{};
};

}  // namespace daw
