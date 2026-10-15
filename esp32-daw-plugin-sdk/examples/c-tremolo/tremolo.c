/* SPDX-License-Identifier: MIT
 * Example: a tremolo effect written in plain C11 against the raw ABI (no C++ helper layer).
 * Shows the whole contract in one file: descriptor, host-allocated instance, parameters as
 * events, sample-accurate parameter changes, and clamping. */
#include <math.h>
#include <string.h>

#include "daw_plugin_api.h"

enum { P_RATE, P_DEPTH, P_COUNT };

static const daw_param_desc_t k_params[P_COUNT] = {
    {P_RATE, "rate", "Rate", "Tremolo", "Hz", DAW_PARAM_FLOAT, DAW_SCALE_LOG, 0.1f, 20.0f, 4.0f, DAW_PARAM_AUTOMATABLE, 0, NULL},
    {P_DEPTH, "depth", "Depth", "Tremolo", "%", DAW_PARAM_FLOAT, DAW_SCALE_LINEAR, 0.0f, 100.0f, 50.0f, DAW_PARAM_AUTOMATABLE, 0, NULL},
};
static const daw_preset_desc_t k_presets[] = {{"Default", "Basic", NULL}};

struct daw_plugin_instance {   /* the opaque type, defined by this plugin */
    const daw_host_api_t *host;
    float sample_rate;
    float values[P_COUNT];
    float phase;
};

static float clampf(float v, const daw_param_desc_t *d)
{
    if (!(v >= d->min)) return d->min;   /* also NaN */
    return v > d->max ? d->max : v;
}

static daw_plugin_instance_t *create(const daw_host_api_t *host, uint32_t sr, uint32_t max_block)
{
    (void)max_block;
    daw_plugin_instance_t *p = host->alloc(host->host_ctx, sizeof *p, 8, DAW_MEM_FAST | DAW_MEM_ZERO);
    if (!p) return NULL;
    p->host = host;
    p->sample_rate = (float)sr;
    for (int i = 0; i < P_COUNT; i++) p->values[i] = k_params[i].def;
    return p;
}
static void destroy(daw_plugin_instance_t *p) { p->host->free(p->host->host_ctx, p); }
static int32_t activate(daw_plugin_instance_t *p) { p->phase = 0; return DAW_OK; }
static void deactivate(daw_plugin_instance_t *p) { (void)p; }
static void set_param(daw_plugin_instance_t *p, uint32_t id, float v)
{
    if (id < P_COUNT) p->values[id] = clampf(v, &k_params[id]);
}
static float get_param(const daw_plugin_instance_t *p, uint32_t id) { return id < P_COUNT ? p->values[id] : 0.0f; }

static void run(daw_plugin_instance_t *p, const daw_process_t *c, uint32_t from, uint32_t to)
{
    const float inc = p->values[P_RATE] / p->sample_rate, depth = p->values[P_DEPTH] * 0.01f;
    for (uint32_t i = from; i < to; i++) {
        float lfo = 0.5f + 0.5f * sinf(6.2831853f * p->phase);
        float g = 1.0f - depth * lfo;
        c->outputs[0][i] = c->inputs[0][i] * g;   /* read before write: in-place safe */
        c->outputs[1][i] = c->inputs[1][i] * g;
        p->phase += inc;
        if (p->phase >= 1.0f) p->phase -= 1.0f;
    }
}

static void process(daw_plugin_instance_t *p, const daw_process_t *c)
{
    uint32_t pos = 0;
    for (uint32_t e = 0; e < c->event_count; e++) {
        const daw_event_t *ev = &c->events[e];
        if (ev->type != DAW_EV_PARAM) continue;
        uint32_t at = ev->offset < c->frames ? ev->offset : c->frames;
        run(p, c, pos, at);
        pos = at;
        set_param(p, ev->u.param.id, ev->u.param.value);
    }
    run(p, c, pos, c->frames);
}

static const daw_plugin_descriptor_t k_descriptor = {
    .struct_size = sizeof(daw_plugin_descriptor_t),
    .api_major = DAW_PLUGIN_API_MAJOR,
    .api_minor = DAW_PLUGIN_API_MINOR,
    .id = "org.smhgzll.example.tremolo",
    .name = "Tremolo (C example)",
    .vendor = "smhgzll",
    .version = "0.1.0",
    .license = "MIT",
    .description = "Amplitude tremolo written in plain C against the raw ABI",
    .category = "Modulation",
    .kind = DAW_PLUGIN_EFFECT,
    .state_version = 1,
    .capabilities = 0,
    .max_voices = 0,
    .param_count = P_COUNT,
    .params = k_params,
    .preset_count = 1,
    .presets = k_presets,
    .key_names = NULL,
    .mem_fast_bytes = sizeof(struct daw_plugin_instance),
    .mem_large_bytes = 0,
    .cpu_permille_base = 8,
    .cpu_permille_voice = 0,
    .create = create,
    .destroy = destroy,
    .activate = activate,
    .deactivate = deactivate,
    .process = process,
    .set_param = set_param,
    .get_param = get_param,
    .save_state = NULL,
    .load_state = NULL,
    .active_voices = NULL,
};

const daw_plugin_descriptor_t *daw_plugin_entry_example_tremolo(void) { return &k_descriptor; }
