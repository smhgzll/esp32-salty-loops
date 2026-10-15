// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/audio_file.hpp"

#include <string.h>

namespace daw {

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool decodeWav(const uint8_t *d, size_t n, DecodedAudio &out, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        return false;
    };
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
        return fail("not a RIFF / WAVE file");
    uint16_t fmt = 0, chans = 0, bits = 0, align = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    size_t pcmBytes = 0;
    out = DecodedAudio{};
    for (size_t p = 12; p + 8 <= n;) {
        const uint32_t len = le32(d + p + 4);
        const uint8_t *body = d + p + 8;
        const size_t avail = n - (p + 8) < len ? n - (p + 8) : len;
        if (!memcmp(d + p, "fmt ", 4) && avail >= 16) {
            fmt = le16(body);
            chans = le16(body + 2);
            rate = le32(body + 4);
            align = le16(body + 12);
            bits = le16(body + 14);
            if (fmt == 0xFFFE && avail >= 26)  // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the tag
                fmt = le16(body + 24);
        } else if (!memcmp(d + p, "data", 4)) {
            pcm = body;
            pcmBytes = avail;
        } else if (!memcmp(d + p, "smpl", 4) && avail >= 36) {
            const uint32_t root = le32(body + 12);
            out.rootKey = root <= 127 ? (int32_t)root : -1;
            if (le32(body + 28) >= 1 && avail >= 36 + 24) {  // the first loop: start, end (inclusive)
                out.loopStart = le32(body + 36 + 8);
                out.loopEnd = le32(body + 36 + 12) + 1;
            }
        }
        if ((uint64_t)p + 8 + len + (len & 1) > n)
            break;
        p += 8 + len + (len & 1);
    }
    if (!pcm || !rate || rate > 384000 || chans == 0 || chans > 8)
        return fail("no audio data, or an unsupported channel count / sample rate");
    const bool isFloat = fmt == 3 && bits == 32;
    const bool isPcm = fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!isFloat && !isPcm)
        return fail("unsupported sample format (PCM 8 / 16 / 24 / 32-bit or float 32)");
    const uint32_t bps = bits / 8;
    const uint32_t stride = align >= bps * chans ? align : bps * chans;
    const uint32_t frames = (uint32_t)(pcmBytes / stride);
    if (frames < 2)
        return fail("the file holds no audio");
    out.channels = chans > 2 ? 2 : chans;
    out.rate = rate;
    for (uint32_t c = 0; c < out.channels; c++)
        out.ch[c].resize(frames);
    for (uint32_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < out.channels; c++) {
            const uint8_t *s = pcm + (size_t)i * stride + c * bps;
            float v;
            if (isFloat) {
                const uint32_t u = le32(s);
                memcpy(&v, &u, 4);
                if (!(v == v) || v > 4.0f || v < -4.0f)  // NaN / garbage: silence it
                    v = 0;
            } else if (bits == 8)
                v = ((float)s[0] - 128.0f) * (1.0f / 128.0f);
            else if (bits == 16)
                v = (float)(int16_t)le16(s) * (1.0f / 32768.0f);
            else if (bits == 24)
                v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) * (1.0f / 8388608.0f);
            else
                v = (float)(int32_t)le32(s) * (1.0f / 2147483648.0f);
            out.ch[c][i] = v;
        }
    if (out.loopEnd > frames || out.loopStart >= out.loopEnd) {
        out.loopStart = 0;
        out.loopEnd = 0;
    }
    return true;
}

std::shared_ptr<LoadedResource> LoadedResource::audio(const std::string &name, DecodedAudio &&a)
{
    auto r = std::shared_ptr<LoadedResource>(new LoadedResource());
    r->name_ = name;
    r->audio_ = std::move(a);
    for (uint32_t c = 0; c < 2; c++)
        r->ptr_[c] = c < r->audio_.channels ? r->audio_.ch[c].data() : nullptr;
    r->res_.struct_size = sizeof(daw_resource_t);
    r->res_.type = DAW_RES_AUDIO;
    r->res_.name = r->name_.c_str();
    r->res_.channels = r->ptr_;
    r->res_.channel_count = r->audio_.channels;
    r->res_.frames = r->audio_.frames();
    r->res_.sample_rate = r->audio_.rate;
    r->res_.root_key = r->audio_.rootKey;
    r->res_.loop_start = r->audio_.loopStart;
    r->res_.loop_end = r->audio_.loopEnd;
    return r;
}

std::shared_ptr<LoadedResource> LoadedResource::file(const std::string &name, std::vector<uint8_t> &&bytes)
{
    auto r = std::shared_ptr<LoadedResource>(new LoadedResource());
    r->name_ = name;
    r->file_ = std::move(bytes);
    r->res_.struct_size = sizeof(daw_resource_t);
    r->res_.type = DAW_RES_FILE;
    r->res_.name = r->name_.c_str();
    r->res_.root_key = -1;
    r->res_.data = r->file_.data();
    r->res_.size = (uint32_t)r->file_.size();
    return r;
}

size_t LoadedResource::bytes() const
{
    return (audio_.ch[0].size() + audio_.ch[1].size()) * sizeof(float) + file_.size() + ownedSize_;
}

std::shared_ptr<LoadedResource> LoadedResource::file(const std::string &name, FileBytes bytes, uint32_t size)
{
    if (!bytes || !size)
        return nullptr;
    return file(name, std::make_shared<FileBuffer>(FileBuffer{std::move(bytes), size}), size);
}

std::shared_ptr<LoadedResource> LoadedResource::file(const std::string &name, std::shared_ptr<const FileBuffer> buffer, uint32_t size)
{
    if (!buffer || !buffer->data || !size || size > buffer->capacity)
        return nullptr;
    auto r = std::shared_ptr<LoadedResource>(new LoadedResource());
    r->name_ = name;
    r->ownedFile_ = std::move(buffer);
    r->ownedSize_ = size;
    r->res_.struct_size = sizeof(daw_resource_t);
    r->res_.type = DAW_RES_FILE;
    r->res_.name = r->name_.c_str();
    r->res_.root_key = -1;
    r->res_.data = r->ownedFile_->data.get();
    r->res_.size = size;
    return r;
}

}  // namespace daw
