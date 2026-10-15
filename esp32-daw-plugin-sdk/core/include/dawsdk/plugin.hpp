// SPDX-License-Identifier: MIT
// esp32-daw-plugin-sdk: C++ adapter turning a plugin class into the C ABI (daw_plugin_api.h).
//
// A plugin class derives from dawsdk::Plugin<Derived, kNumParams> and implements:
//
//   int32_t init();                       // control thread: allocate through alloc<T>(), may fail
//   void    release();                    // control thread: free what init() allocated
//   void    reset();                      // control thread: clear DSP state (activate)
//   void    onParam(uint32_t id, float v);// a parameter changed (already stored in param(id))
//   void    onEvent(const daw_event_t &e);// [RT] notes / MIDI (params are handled by the base)
//   void    render(const daw_process_t &c, uint32_t start, uint32_t n); // [RT] frames start..start+n
//
// Optional: void onStart(const daw_process_t &c); // [RT] first block after activate(): snap smoothers to
//                                       //      the parameters set since then (no initial glide)
//           uint32_t voices() const;
//           int32_t setResource(uint32_t slot, const daw_resource_t *res); // control thread (ABI 1.1);
//                                       // a plugin with resource slots also lists them after
//                                       // DAWSDK_CALLBACKS: DAWSDK_RESOURCES(P, count, slots) int32_t saveState(void*,uint32_t) const;
//           int32_t loadState(const void*,uint32_t,uint32_t).
//
// Instances live in host memory (placement new); a plugin never calls global new/delete/malloc.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <new>

#include "daw_plugin_api.h"
#include "dawsdk/midi.hpp"

namespace dawsdk {
inline namespace v1 {

template <class Derived, uint32_t NumParams>
class Plugin {
public:
    static constexpr uint32_t kNumParams = NumParams;

    float sampleRate() const { return sampleRate_; }
    uint32_t maxBlock() const { return maxBlock_; }
    float param(uint32_t id) const { return id < NumParams ? params_[id] : 0.0f; }

    // memory from the host; FAST (internal RAM) or LARGE (PSRAM on the ESP32-S3)
    template <class T>
    T *alloc(size_t count, uint32_t flags = DAW_MEM_FAST)
    {
        void *p = host_->alloc(host_->host_ctx, sizeof(T) * count, alignof(T) < 4 ? 4 : alignof(T),
                               flags | DAW_MEM_ZERO);
        return static_cast<T *>(p);
    }
    void dealloc(void *p)
    {
        if (p)
            host_->free(host_->host_ctx, p);
    }
    void report(int32_t code) const
    {
        if (host_->report)
            host_->report(host_->host_ctx, code);
    }
    void log(int level, const char *msg) const
    {
        if (host_->log)
            host_->log(host_->host_ctx, level, msg);
    }

    // defaults for the optional hooks
    int32_t init() { return DAW_OK; }
    void release() {}
    void reset() {}
    void onParam(uint32_t, float) {}
    void onEvent(const daw_event_t &) {}
    void onStart(const daw_process_t &) {}
    uint32_t voices() const { return 0; }
    bool nativeMidi() const { return false; }
    int32_t setResource(uint32_t, const daw_resource_t *) { return DAW_ERR_UNSUPPORTED; }  // ABI 1.1

    // ---- the C ABI -----------------------------------------------------------------------
    static daw_plugin_instance_t *abiCreate(const daw_host_api_t *host, uint32_t sr, uint32_t maxBlock)
    {
        if (!host || !host->alloc || !host->free || sr == 0 || maxBlock == 0)
            return nullptr;
        void *mem = host->alloc(host->host_ctx, sizeof(Derived), alignof(Derived) < 4 ? 4 : alignof(Derived),
                                DAW_MEM_FAST | DAW_MEM_ZERO);
        if (!mem)
            return nullptr;
        Derived *p = new (mem) Derived();
        p->host_ = host;
        p->sampleRate_ = (float)sr;
        p->maxBlock_ = maxBlock;
        const daw_param_desc_t *d = Derived::descriptor()->params;
        for (uint32_t i = 0; i < NumParams; i++)
            p->params_[d[i].id] = d[i].def;
        if (p->init() != DAW_OK) {
            p->release();
            p->~Derived();
            host->free(host->host_ctx, mem);
            return nullptr;
        }
        for (uint32_t i = 0; i < NumParams; i++)
            p->onParam(d[i].id, p->params_[d[i].id]);
        return reinterpret_cast<daw_plugin_instance_t *>(p);
    }
    static void abiDestroy(daw_plugin_instance_t *inst)
    {
        if (!inst)
            return;
        Derived *p = self(inst);
        const daw_host_api_t *host = p->host_;
        p->release();
        p->~Derived();
        host->free(host->host_ctx, p);
    }
    static int32_t abiActivate(daw_plugin_instance_t *inst)
    {
        self(inst)->reset();
        self(inst)->started_ = false;
        self(inst)->midiActive_ = false;
        self(inst)->genericMidi_ = MidiControls{};
        return DAW_OK;
    }
    static void abiDeactivate(daw_plugin_instance_t *) {}
    static void abiProcess(daw_plugin_instance_t *inst, const daw_process_t *c)
    {
        Derived *p = self(inst);
        if (!p->started_) {
            p->started_ = true;
            p->onStart(*c);
        }
        uint32_t pos = 0;
        for (uint32_t i = 0; i < c->event_count; i++) {
            const daw_event_t &e = c->events[i];
            uint32_t at = e.offset < c->frames ? e.offset : c->frames - 1;
            if (at > pos) {
                p->renderMidi(*c, pos, at - pos);
                pos = at;
            }
            if (e.type == DAW_EV_PARAM)
                p->applyParam(e.u.param.id, e.u.param.value);
            else
                p->eventMidi(e);
        }
        if (pos < c->frames)
            p->renderMidi(*c, pos, c->frames - pos);
    }
    static void abiSetParam(daw_plugin_instance_t *inst, uint32_t id, float v) { self(inst)->applyParam(id, v); }
    static float abiGetParam(const daw_plugin_instance_t *inst, uint32_t id)
    {
        return reinterpret_cast<const Derived *>(inst)->param(id);
    }
    static int32_t abiSetResource(daw_plugin_instance_t *inst, uint32_t slot, const daw_resource_t *res)
    {
        return self(inst)->setResource(slot, res);
    }
    static uint32_t abiVoices(const daw_plugin_instance_t *inst)
    {
        return reinterpret_cast<const Derived *>(inst)->voices();
    }

    void applyParam(uint32_t id, float v)
    {
        if (id >= NumParams)
            return;
        const daw_param_desc_t &d = Derived::descriptor()->params[id];
        if (!(v >= d.min))  // also catches NaN
            v = d.min;
        if (v > d.max)
            v = d.max;
        if (d.type != DAW_PARAM_FLOAT)
            v = (float)(int32_t)(v + (v < 0 ? -0.5f : 0.5f));  // round half away from zero (also for negatives)
        params_[id] = v;
        static_cast<Derived *>(this)->onParam(id, v);
    }

private:
    MidiControls genericMidi_;
    bool midiActive_ = false;
    void renderMidi(const daw_process_t &c, uint32_t start, uint32_t n) {
        auto *p = static_cast<Derived *>(this);
        p->render(c, start, n);
        if (!midiActive_ || p->nativeMidi()) return;
        float l, r; genericMidi_.gains(l, r);
        if (l == 1 && r == 1) return;
        for (uint32_t i = start; i < start + n; ++i) { c.outputs[0][i] *= l; c.outputs[1][i] *= r; }
    }
    void sendMidiPitch(uint32_t offset) {
        auto *p = static_cast<Derived *>(this);
        if (!(Derived::descriptor()->capabilities & DAW_CAP_PITCH_BEND)) return;
        daw_event_t e{}; e.offset = offset; e.type = DAW_EV_CHANNEL_PITCH;
        e.u.midi.value = genericMidi_.wheel * genericMidi_.range + genericMidi_.tuning();
        if (e.u.midi.value < -256) e.u.midi.value = -256;
        if (e.u.midi.value > 256) e.u.midi.value = 256;
        p->onEvent(e);
    }
    void eventMidi(const daw_event_t &source) {
        auto *p = static_cast<Derived *>(this);
        if (p->nativeMidi()) { p->onEvent(source); return; }
        if (source.flags & DAW_NOTE_FLAG_MIDI) midiActive_ = true;
        if (!midiActive_) { p->onEvent(source); return; }
        daw_event_t e = source;
        if (e.type == DAW_EV_MIDI_RESET) {
            genericMidi_ = MidiControls{};
            daw_event_t off{}; off.type = DAW_EV_ALL_SOUND_OFF; p->onEvent(off);
            sendMidiPitch(e.offset); return;
        }
        if (e.type == DAW_EV_CC) {
            const int v = int(e.u.midi.value * 127 + .5f);
            if (genericMidi_.cc(e.u.midi.number, (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v))) sendMidiPitch(e.offset);
        } else if (e.type == DAW_EV_PITCH_BEND) {
            genericMidi_.wheel = e.u.midi.value; sendMidiPitch(e.offset); return;
        } else if (e.type == DAW_EV_MIDI_BYTES) {
            bool reset;
            if (midiDeviceControl(e.u.bytes.data, e.u.bytes.size, genericMidi_, reset)) {
                if (reset) { genericMidi_ = MidiControls{}; daw_event_t off{}; off.type = DAW_EV_ALL_SOUND_OFF; p->onEvent(off); }
                sendMidiPitch(e.offset);
            }
        } else if (e.type == DAW_EV_NOTE_ON && genericMidi_.soft >= 64) e.u.note.velocity *= .7f;
        p->onEvent(e);
    }

protected:
    const daw_host_api_t *host_ = nullptr;
    float sampleRate_ = 44100.0f;
    uint32_t maxBlock_ = 256;
    float params_[NumParams > 0 ? NumParams : 1] = {};
    bool started_ = false;

private:
    static Derived *self(daw_plugin_instance_t *i) { return reinterpret_cast<Derived *>(i); }
};

}  // namespace v1
}  // namespace dawsdk

// Fills the function pointers of a daw_plugin_descriptor_t for plugin class P (C++20 designated
// initializers, declaration order).
#define DAWSDK_CALLBACKS(P)                                                                       \
    .create = P::abiCreate, .destroy = P::abiDestroy, .activate = P::abiActivate,                \
    .deactivate = P::abiDeactivate, .process = P::abiProcess, .set_param = P::abiSetParam,         \
    .get_param = P::abiGetParam, .save_state = nullptr, .load_state = nullptr,                    \
    .active_voices = P::abiVoices

// ABI 1.1: resource slots (after DAWSDK_CALLBACKS(P) in the descriptor)
#define DAWSDK_RESOURCES(P, count, slots) .resource_slot_count = (count), .resource_slots = (slots), .set_resource = P::abiSetResource
