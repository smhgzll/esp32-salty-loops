// SPDX-License-Identifier: MIT
// dawplug: the SDK's command-line test host.
//   dawplug list
//   dawplug info <plugin> [--arch A]                 descriptor as manifest JSON
//   dawplug render <plugin> [--preset N] [--param key=value]... [--resource slot=file]... [--seq scale|chords|drums|sustain] [--seconds S] [--out f.wav]
//   dawplug bench <plugin> [--preset N] [--param key=value]... [--resource slot=file]... [--voices N] [--seconds S] [--json]
//   dawplug validate <plugin|all>
// <plugin> is a full id (org.smhgzll.sine) or its last component (sine).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <memory>
#include <string>
#include <vector>

#include "dawsdk_host/host.hpp"
#include "validate.hpp"

using namespace dawhost;

static const char *arg(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return def;
}
static bool flag(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

static const daw_plugin_descriptor_t *need(const char *name)
{
    const daw_plugin_descriptor_t *d = name ? find(name) : nullptr;
    if (!d) {
        fprintf(stderr, "unknown plugin '%s' (dawplug list)\n", name ? name : "");
        exit(2);
    }
    return d;
}

static std::vector<TimedEvent> sequenceFor(const daw_plugin_descriptor_t *d, const std::string &seq, double seconds)
{
    if (seq == "drums" || (seq.empty() && (d->capabilities & DAW_CAP_DRUM_KIT)))
        return sequenceDrums((int)(seconds / 2) + 1, 120);
    if (seq == "chords" || (seq.empty() && (d->capabilities & DAW_CAP_POLYPHONIC)))
        return sequenceChords(60, (int)(seconds / 1.0), 1.0);
    if (seq == "sustain") {
        std::vector<TimedEvent> ev{{0.0, noteOn(48, 0.9f, 1)}, {seconds * 0.8, noteOff(48, 1)}};
        return ev;
    }
    return sequenceScale(48, (int)(seconds / 0.25), 0.25, 0.2, 0.85f);
}

static std::vector<float> effectInput(uint32_t sr, double seconds, int ch)
{
    // a saw chord with gaps: shows filters, delays and tails
    std::vector<float> x((size_t)(sr * seconds));
    double ph[3] = {0, 0.3, 0.6};
    const double f[3] = {110.0, 138.6, 164.8};
    for (size_t i = 0; i < x.size(); i++) {
        double t = (double)i / sr, s = 0;
        for (int k = 0; k < 3; k++) {
            ph[k] += f[k] * (1.0 + 0.002 * ch) / sr;
            ph[k] -= floor(ph[k]);
            s += 2 * ph[k] - 1;
        }
        double gate = fmod(t, 1.0) < 0.35 ? 1.0 : 0.0;
        x[i] = (float)(0.2 * s * gate);
    }
    return x;
}

struct Resources {
    std::vector<std::unique_ptr<AudioFile>> audio;
    std::vector<std::unique_ptr<FileResource>> files;
};
static int configure(Instance &in, const daw_plugin_descriptor_t *d, Resources &resources, int argc, char **argv)
{
    auto &audio = resources.audio;
    auto &files = resources.files;
    if (const char *p = arg(argc, argv, "--preset", nullptr))
        if (!in.loadPreset((uint32_t)atoi(p))) return 1;
    // --resource slot=file (repeatable, ABI 1.1): .wav decoded to audio, anything else as file bytes
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--resource") != 0)
            continue;
        const char *kv = argv[i + 1], *eq = strchr(kv, '=');
        int slot = -1;
        for (uint32_t k = 0; eq && daw_plugin_has_resources(d) && k < d->resource_slot_count; k++)
            if (strlen(d->resource_slots[k].key) == (size_t)(eq - kv) && !strncmp(d->resource_slots[k].key, kv, (size_t)(eq - kv)))
                slot = (int)k;
        if (slot < 0) {
            fprintf(stderr, "no resource slot %s\n", kv);
            return 1;
        }
        const std::string path = eq + 1;
        const daw_resource_t *res = nullptr;
        std::string err;
        if (path.size() > 4 && !strcasecmp(path.c_str() + path.size() - 4, ".wav")) {
            audio.push_back(std::make_unique<AudioFile>());
            if (!readWav(path, *audio.back(), &err)) {
                fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
                return 1;
            }
            res = audio.back()->resource();
        } else {
            files.push_back(std::make_unique<FileResource>());
            if (!readFile(path, *files.back())) {
                fprintf(stderr, "cannot read %s\n", path.c_str());
                return 1;
            }
            res = files.back()->resource();
        }
        if (in.setResource((uint32_t)slot, res) != DAW_OK) {
            fprintf(stderr, "the plugin refused %s for slot %s\n", path.c_str(), d->resource_slots[slot].key);
            return 1;
        }
    }
    // --param key=value (repeatable), after the preset
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--param") != 0)
            continue;
        const char *kv = argv[i + 1], *eq = strchr(kv, '=');
        bool found = false;
        for (uint32_t k = 0; eq && k < d->param_count; k++)
            if (strlen(d->params[k].key) == (size_t)(eq - kv) && !strncmp(d->params[k].key, kv, (size_t)(eq - kv))) {
                in.setParam(d->params[k].id, (float)atof(eq + 1));
                found = true;
            }
        if (!found)
            fprintf(stderr, "warning: no parameter %s\n", kv);
    }
    return 0;
}

static int cmdRender(int argc, char **argv)
{
    auto *d = need(argc > 2 ? argv[2] : nullptr);
    double seconds = atof(arg(argc, argv, "--seconds", "4"));
    std::string out = arg(argc, argv, "--out", "out.wav");
    Resources resources;  // resources outlive the instance
    Instance in(d);
    if (!in.ok()) {
        fprintf(stderr, "create failed\n");
        return 1;
    }
    if (configure(in, d, resources, argc, argv)) return 1;
    auto ev = sequenceFor(d, arg(argc, argv, "--seq", ""), seconds);
    RenderResult r;
    if (d->kind == DAW_PLUGIN_EFFECT) {
        auto l = effectInput(in.sampleRate(), seconds, 0), rr = effectInput(in.sampleRate(), seconds, 1);
        r = render(in, ev, seconds, 256, &l, &rr);
    } else {
        r = render(in, ev, seconds, 256);
    }
    if (!writeWav(out, r.l, r.r, in.sampleRate())) {
        fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    printf("%s: %.2f s, peak L %.3f R %.3f, rms L %.4f R %.4f, hash %016llx -> %s\n", d->id, seconds, peak(r.l), peak(r.r),
           rms(r.l), rms(r.r), (unsigned long long)hashPcm(r.l, r.r), out.c_str());
    return 0;
}

static int cmdBench(int argc, char **argv)
{
    auto *d = need(argc > 2 ? argv[2] : nullptr);
    int voices = atoi(arg(argc, argv, "--voices", d->kind == DAW_PLUGIN_EFFECT ? "0" : "4"));
    double seconds = atof(arg(argc, argv, "--seconds", "10"));
    Resources resources;  // resources outlive the instance
    Instance in(d);
    if (!in.ok() || configure(in, d, resources, argc, argv)) return 1;
    for (uint32_t k = 0; k < d->param_count; k++)
        if (!strcmp(d->params[k].key, "polyphony")) in.setParam(d->params[k].id, (float)voices);
    if (voices > (int)d->max_voices && d->kind == DAW_PLUGIN_INSTRUMENT)
        voices = (int)d->max_voices;
    std::vector<TimedEvent> ev;
    for (int v = 0; v < voices; v++) {
        uint8_t key = (d->capabilities & DAW_CAP_DRUM_KIT) ? (uint8_t)(36 + v) : (uint8_t)(48 + v * 7 % 24);
        ev.push_back({0.0, noteOn(key, 0.8f, v)});
        // drums retrigger every 100 ms so voices stay busy
        if (d->capabilities & DAW_CAP_DRUM_KIT)
            for (double t = 0.1; t < seconds; t += 0.1)
                ev.push_back({t, noteOn(key, 0.8f, v)});
    }
    RenderResult r;
    if (d->kind == DAW_PLUGIN_EFFECT) {
        auto l = effectInput(in.sampleRate(), seconds, 0), rr = effectInput(in.sampleRate(), seconds, 1);
        r = render(in, ev, seconds, 256, &l, &rr);
    } else {
        r = render(in, ev, seconds, 256);
    }
    double budget = 256.0 / in.sampleRate();
    double avg = r.processSeconds / r.blocks;
    if (flag(argc, argv, "--json"))
        printf("{\"plugin\":\"%s\",\"voices\":%d,\"blocks\":%u,\"avgBlockUs\":%.3f,\"maxBlockUs\":%.3f,"
               "\"hostRealtimePercent\":%.4f,\"target\":\"host\",\"peak\":%.6f,\"rms\":%.6f,\"memFast\":%zu,\"memLarge\":%zu,\"rtViolations\":%zu}\n",
               d->id, voices, r.blocks, avg * 1e6, r.maxBlockSeconds * 1e6, 100.0 * avg / budget, peak(r.l), rms(r.l), in.host().fastBytes, in.host().largeBytes, in.host().rtViolations);
    else
        printf("%-28s voices %2d  avg %8.2f us/block  max %8.2f us  %.3f %% of a 256-frame block (THIS HOST CPU, not an ESP32)\n",
               d->id, voices, avg * 1e6, r.maxBlockSeconds * 1e6, 100.0 * avg / budget);
    return 0;
}

static int cmdValidate(int argc, char **argv)
{
    std::vector<const daw_plugin_descriptor_t *> list;
    std::string which = argc > 2 ? argv[2] : "all";
    if (which == "all")
        list = registry();
    else
        list.push_back(need(which.c_str()));
    int failed = 0;
    for (auto *d : list) {
        auto res = validatePlugin(d);
        for (auto &w : res.warnings)
            printf("  warn  %s: %s\n", d->id, w.c_str());
        for (auto &f : res.failures)
            printf("  FAIL  %s: %s\n", d->id, f.c_str());
        printf("%s %s\n", res.failures.empty() ? "PASS" : "FAIL", d->id);
        failed += !res.failures.empty();
    }
    printf("validate: %zu plugins, %d failed\n", list.size(), failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dawplug list | info <plugin> | render <plugin> | bench <plugin> | validate <plugin|all>\n");
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "list") {
        for (auto *d : registry())
            printf("%-28s %-10s %-12s %s\n", d->id, d->version, d->kind == DAW_PLUGIN_INSTRUMENT ? "instrument" : "effect", d->name);
        return 0;
    }
    if (cmd == "info") {
        auto *d = need(argc > 2 ? argv[2] : nullptr);
        fputs(descriptorJson(d, arg(argc, argv, "--arch", nullptr)).c_str(), stdout);
        return 0;
    }
    if (cmd == "render")
        return cmdRender(argc, argv);
    if (cmd == "bench")
        return cmdBench(argc, argv);
    if (cmd == "validate")
        return cmdValidate(argc, argv);
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
