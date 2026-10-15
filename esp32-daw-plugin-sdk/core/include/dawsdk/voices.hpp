// SPDX-License-Identifier: MIT
// esp32-daw-plugin-sdk: fixed-size voice pool with deterministic voice stealing.
//
// V must provide:  bool active() const;  float loudness() const;
// and the public fields managed here: noteId, key, held, age.
#pragma once

#include <stdint.h>

namespace dawsdk {
inline namespace v1 {

struct VoiceTag {
    int32_t noteId = -1;
    uint8_t key = 0;
    bool held = false;   // gate on (key down / note not ended yet)
    uint32_t age = 0;    // allocation order; larger is newer
};

template <class V, uint32_t N>
struct VoicePool {
    V voice[N];
    uint32_t limit = N;  // polyphony limit (<= N), may change at run time
    uint32_t clock = 0;
    uint32_t steals = 0;

    void setLimit(uint32_t n) { limit = n < 1 ? 1 : (n > N ? N : n); }

    // a free voice, or a stolen one: released voices before held ones, then the quietest of the
    // oldest half. `stolen` tells the caller to fade / hard-reset the voice.
    V *allocate(bool &stolen)
    {
        stolen = false;
        for (uint32_t i = 0; i < limit; i++)
            if (!voice[i].active())
                return tag(&voice[i]);
        V *best = nullptr;
        for (uint32_t i = 0; i < limit; i++) {  // oldest released voice
            if (!voice[i].held && (!best || voice[i].age < best->age))
                best = &voice[i];
        }
        if (!best) {
            for (uint32_t i = 0; i < limit; i++)  // oldest held voice
                if (!best || voice[i].age < best->age)
                    best = &voice[i];
        }
        stolen = true;
        steals++;
        return tag(best);
    }
    // the voice playing a note: by host note id when known, else the newest held voice on `key`
    V *find(int32_t noteId, uint8_t key)
    {
        V *best = nullptr;
        for (uint32_t i = 0; i < N; i++) {
            V &v = voice[i];
            if (!v.active() || !v.held)
                continue;
            if (noteId >= 0 ? v.noteId == noteId : v.key == key) {
                if (!best || v.age > best->age)
                    best = &v;
            }
        }
        if (!best && noteId >= 0)  // a host that mixes ids and live keys
            for (uint32_t i = 0; i < N; i++)
                if (voice[i].active() && voice[i].held && voice[i].noteId < 0 && voice[i].key == key)
                    return &voice[i];
        return best;
    }
    // the newest sounding voice (for slides)
    V *newest()
    {
        V *best = nullptr;
        for (uint32_t i = 0; i < N; i++)
            if (voice[i].active() && voice[i].held && (!best || voice[i].age > best->age))
                best = &voice[i];
        return best;
    }
    uint32_t activeCount() const
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < N; i++)
            n += voice[i].active() ? 1u : 0u;
        return n;
    }

private:
    V *tag(V *v)
    {
        v->age = ++clock;
        return v;
    }
};

}  // namespace v1
}  // namespace dawsdk
