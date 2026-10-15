// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/project_json.hpp"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "cJSON.h"
#include "daw/platform.hpp"

namespace daw {

namespace {

double num(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : def;
}
bool flag(const cJSON *o, const char *k, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    return def;
}
std::string str(const cJSON *o, const char *k, const char *def = "")
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string(def);
}
std::string clampName(std::string s, size_t max = 64)
{
    if (s.size() > max)
        s.resize(max);
    for (auto &c : s)
        if ((unsigned char)c < 0x20)
            c = ' ';
    return s;
}
double at(const cJSON *a, int i, double def)
{
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : def;
}
cJSON *numArray(std::initializer_list<double> v)
{
    cJSON *a = cJSON_CreateArray();
    for (double x : v)
        cJSON_AddItemToArray(a, cJSON_CreateNumber(x));
    return a;
}
// floats with 4 decimals keep the files small and stable
double r4(float v) { return round((double)v * 10000.0) / 10000.0; }

// A number as cJSON 1.7 prints it (print_number), for arrays written as JSON text: a cJSON number
// node costs ~50 B of PSRAM, and a song of 5,000 notes as a tree did not fit next to its
// SoundFont channels (the snapshot came out as null).
void appendNumber(std::string &o, double d)
{
    char b[32];
    const int vi = d >= (double)INT_MAX ? INT_MAX : d <= (double)INT_MIN ? INT_MIN : (int)d;
    if (isnan(d) || isinf(d))
        snprintf(b, sizeof b, "null");
    else if (d == (double)vi)
        snprintf(b, sizeof b, "%d", vi);
    else {
        snprintf(b, sizeof b, "%1.15g", d);
        double test = 0;
        if (sscanf(b, "%lg", &test) != 1 || !(fabs(test - d) <= std::max(fabs(test), fabs(d)) * DBL_EPSILON))
            snprintf(b, sizeof b, "%1.17g", d);
    }
    o += b;
}
void appendArray(std::string &o, const double *v, int n)
{
    o += '[';
    for (int i = 0; i < n; i++) {
        if (i)
            o += ',';
        appendNumber(o, v[i]);
    }
    o += ']';
}
bool defaultStep(const Step &x) { return !x.on && x.vel == 0.78f && x.pan == 0.0f && x.key == 0 && !x.accent && x.prob == 1.0f; }

const char *kTargetKinds[] = {"none", "channelVol", "channelPan", "channelParam", "trackVol", "trackPan", "fxParam", "fxMix", "sendLevel", "tempo"};

}  // namespace

std::string colorString(uint32_t rgb)
{
    char b[8];
    snprintf(b, sizeof b, "#%06x", (unsigned)(rgb & 0xFFFFFF));
    return b;
}

uint32_t parseColor(const char *s, uint32_t def)
{
    if (!s || s[0] != '#' || strlen(s) != 7)
        return def;
    char *end = nullptr;
    unsigned long v = strtoul(s + 1, &end, 16);
    return end && *end == 0 ? (uint32_t)v : def;
}

std::string jsonText(cJSON *j, bool free)
{
    char *t = cJSON_PrintUnformatted(j);
    std::string s = t ? t : "null";
    cJSON_free(t);
    if (free)
        cJSON_Delete(j);
    return s;
}

cJSON *pluginStateJson(const PluginState &s)
{
    if (s.missing && !s.rawJson.empty()) {
        cJSON *raw = cJSON_Parse(s.rawJson.c_str());
        if (raw)
            return raw;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", s.pluginId.c_str());
    cJSON_AddStringToObject(o, "version", s.version.c_str());
    cJSON *ps = cJSON_AddObjectToObject(o, "params");
    for (const auto &kv : s.params)
        cJSON_AddNumberToObject(ps, kv.first.c_str(), r4(kv.second));
    if (!s.preset.empty())
        cJSON_AddStringToObject(o, "preset", s.preset.c_str());
    if (!s.blob.empty())
        cJSON_AddStringToObject(o, "blob", s.blob.c_str());
    if (!s.resources.empty()) {
        cJSON *rs = cJSON_AddObjectToObject(o, "resources");
        for (const auto &kv : s.resources)
            cJSON_AddStringToObject(rs, kv.first.c_str(), kv.second.c_str());
    }
    if (!s.missingResources.empty()) {  // runtime information for the browser; ignored when loading
        cJSON *mr = cJSON_AddArrayToObject(o, "missingResources");
        for (const auto &m : s.missingResources)
            cJSON_AddItemToArray(mr, cJSON_CreateString(m.c_str()));
    }
    if (!s.loadError.empty())
        cJSON_AddStringToObject(o, "loadError", s.loadError.c_str());
    return o;
}

static void pluginStateFromJson(const cJSON *o, PluginState &s, bool (*known)(const std::string &))
{
    s = PluginState();
    if (!cJSON_IsObject(o))
        return;
    s.pluginId = str(o, "id");
    s.version = str(o, "version");
    s.preset = str(o, "preset");
    s.blob = str(o, "blob");
    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(o, "params");
    const cJSON *kv;
    cJSON_ArrayForEach(kv, ps)
    {
        if (cJSON_IsNumber(kv) && kv->string && isfinite(kv->valuedouble))
            s.params.emplace_back(kv->string, (float)kv->valuedouble);
    }
    cJSON_ArrayForEach(kv, cJSON_GetObjectItemCaseSensitive(o, "resources"))
    {
        if (cJSON_IsString(kv) && kv->string && *kv->valuestring && s.resources.size() < 8)
            s.resources.emplace_back(kv->string, std::string(kv->valuestring).substr(0, 64));
    }
    if (!s.pluginId.empty() && known && !known(s.pluginId)) {
        s.missing = true;
        char *t = cJSON_PrintUnformatted(o);
        s.rawJson = t ? t : "";
        cJSON_free(t);
    }
}

// set by the session: does this firmware have the plugin? (missing ones are preserved verbatim)
static bool (*g_known)(const std::string &) = nullptr;
void setPluginKnownCheck(bool (*fn)(const std::string &)) { g_known = fn; }

cJSON *metaJson(const Project &p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", p.name.c_str());
    cJSON_AddStringToObject(o, "author", p.author.c_str());
    cJSON_AddNumberToObject(o, "bpm", round(p.bpm * 1000.0) / 1000.0);
    cJSON_AddItemToObject(o, "timeSig", numArray({(double)p.ts.num, (double)p.ts.den}));
    cJSON *l = cJSON_AddObjectToObject(o, "loop");
    cJSON_AddBoolToObject(l, "on", p.loopOn);
    cJSON_AddNumberToObject(l, "start", p.loopStart);
    cJSON_AddNumberToObject(l, "end", p.loopEnd);
    cJSON_AddBoolToObject(o, "metronome", p.metronome);
    cJSON_AddNumberToObject(o, "countIn", p.countInBars);
    cJSON_AddNumberToObject(o, "currentPattern", p.currentPattern);
    cJSON_AddNumberToObject(o, "ppq", kPPQ);
    return o;
}

cJSON *channelJson(const Channel &c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", c.id);
    cJSON_AddStringToObject(o, "name", c.name.c_str());
    cJSON_AddStringToObject(o, "color", colorString(c.color).c_str());
    cJSON_AddStringToObject(o, "icon", c.icon.c_str());
    cJSON_AddNumberToObject(o, "vol", r4(c.vol));
    cJSON_AddNumberToObject(o, "pan", r4(c.pan));
    cJSON_AddBoolToObject(o, "mute", c.mute);
    cJSON_AddBoolToObject(o, "solo", c.solo);
    cJSON_AddNumberToObject(o, "track", c.track);
    cJSON_AddNumberToObject(o, "root", c.root);
    cJSON_AddItemToObject(o, "plugin", pluginStateJson(c.inst));
    if (c.inst.missing)
        cJSON_AddBoolToObject(o, "missing", true);
    return o;
}

cJSON *channelsJson(const Project &p)
{
    cJSON *a = cJSON_CreateArray();
    for (const auto &c : p.channels)
        cJSON_AddItemToArray(a, channelJson(c));
    return a;
}

cJSON *patternJson(const Pattern &pat, int32_t length)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", pat.id);
    cJSON_AddStringToObject(o, "name", pat.name.c_str());
    cJSON_AddStringToObject(o, "color", colorString(pat.color).c_str());
    cJSON_AddNumberToObject(o, "length", pat.lengthTicks);
    if (length >= 0)
        cJSON_AddNumberToObject(o, "effLength", length);  // effective length (protocol only)
    cJSON_AddNumberToObject(o, "swing", r4(pat.swing));
    // Compact, for large imported songs (every pattern holds a row per channel): a channel's row
    // with nothing in it is left out (Pattern::get makes it again); steps are left out when all
    // are off with default values; notes leave out trailing default fields. The reader takes
    // all three (the browser fills them in: web/src/app/protocol.ts). Steps and notes are written
    // as JSON text, not as cJSON nodes.
    cJSON *seqs = cJSON_AddArrayToObject(o, "seqs");
    std::string text;
    for (const auto &s : pat.seqs) {
        const int count = std::min((int)s.stepCount, (int)s.steps.size());
        const bool plainSteps = std::all_of(s.steps.begin(), s.steps.begin() + count, defaultStep);
        if (s.midi.empty() && s.notes.empty() && plainSteps && s.stepCount == 16 && !s.pianoRoll && s.nextNoteId <= 1)
            continue;
        cJSON *q = cJSON_CreateObject();
        cJSON_AddNumberToObject(q, "channel", s.channel);
        cJSON_AddNumberToObject(q, "stepCount", s.stepCount);
        cJSON_AddBoolToObject(q, "pianoRoll", s.pianoRoll);
        cJSON_AddNumberToObject(q, "nextNoteId", s.nextNoteId);
        // steps: [on, vel, pan, key, accent, prob]; trailing all-off steps are still listed
        if (!plainSteps) {
            text = "[";
            for (int i = 0; i < count; i++) {
                const Step &x = s.steps[(size_t)i];
                const double v[6] = {x.on ? 1.0 : 0.0, r4(x.vel), r4(x.pan), (double)x.key, x.accent ? 1.0 : 0.0, r4(x.prob)};
                if (i)
                    text += ',';
                appendArray(text, v, 6);
            }
            text += ']';
            cJSON_AddItemToObject(q, "steps", cJSON_CreateRaw(text.c_str()));
        }
        // notes: [id, start, len, key, vel, pan, pitch, rel, prob, flags(1 slide, 2 accent)], at least
        // to vel, then up to the last field that is not its default (0, 0, 0.5, 1, 0)
        static const double kNoteDefaults[12] = {0, 0, 0, 0, 0, 0, 0, 0.5, 1, 0, 0, 0};
        text = "[";
        for (size_t k = 0; k < s.notes.size(); k++) {
            const Note &n = s.notes[k];
            const double v[12] = {(double)n.id, (double)n.start, (double)n.len, (double)n.key, r4(n.vel), r4(n.pan), r4(n.pitch),
                                  r4(n.rel), r4(n.prob), (double)((n.slide ? 1 : 0) | (n.accent ? 2 : 0)), (double)n.midiOnOrder, (double)n.midiOffOrder};
            int len = 12;
            while (len > 5 && v[len - 1] == kNoteDefaults[len - 1])
                len--;
            if (k)
                text += ',';
            appendArray(text, v, len);
        }
        text += ']';
        cJSON_AddItemToObject(q, "notes", cJSON_CreateRaw(text.c_str()));
        if (!s.midi.empty()) cJSON_AddItemToObject(q, "midi", midiJson(s.midi));
        cJSON_AddItemToArray(seqs, q);
    }
    return o;
}

cJSON *patternsJson(const Project &p)
{
    cJSON *a = cJSON_CreateArray();
    for (const auto &pat : p.patterns)
        cJSON_AddItemToArray(a, patternJson(pat, p.patternLength(pat)));
    return a;
}

cJSON *trackJson(const MixerTrack &t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "index", t.index);
    cJSON_AddStringToObject(o, "name", t.name.c_str());
    cJSON_AddStringToObject(o, "color", colorString(t.color).c_str());
    cJSON_AddNumberToObject(o, "vol", r4(t.vol));
    cJSON_AddNumberToObject(o, "pan", r4(t.pan));
    cJSON_AddBoolToObject(o, "mute", t.mute);
    cJSON_AddBoolToObject(o, "solo", t.solo);
    cJSON_AddNumberToObject(o, "route", t.route);
    cJSON *ss = cJSON_AddArrayToObject(o, "sends");
    for (const auto &s : t.sends) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "target", s.target);
        cJSON_AddNumberToObject(x, "level", r4(s.level));
        cJSON_AddItemToArray(ss, x);
    }
    cJSON *fx = cJSON_AddArrayToObject(o, "fx");
    for (const auto &f : t.fx) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "id", f.id);
        cJSON_AddBoolToObject(x, "enabled", f.enabled);
        cJSON_AddNumberToObject(x, "mix", r4(f.mix));
        cJSON_AddItemToObject(x, "plugin", pluginStateJson(f.inst));
        if (f.inst.missing)
            cJSON_AddBoolToObject(x, "missing", true);
        cJSON_AddItemToArray(fx, x);
    }
    return o;
}

cJSON *mixerJson(const Project &p)
{
    cJSON *a = cJSON_CreateArray();
    for (const auto &t : p.mixer)
        cJSON_AddItemToArray(a, trackJson(t));
    return a;
}

cJSON *playlistJson(const Project &p)
{
    static const char *kinds[] = {"pattern", "automation", "audio"};
    cJSON *o = cJSON_CreateObject();
    cJSON *ts = cJSON_AddArrayToObject(o, "tracks");
    for (const auto &t : p.tracks) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "name", t.name.c_str());
        cJSON_AddStringToObject(x, "color", colorString(t.color).c_str());
        cJSON_AddBoolToObject(x, "mute", t.mute);
        cJSON_AddBoolToObject(x, "solo", t.solo);
        cJSON_AddItemToArray(ts, x);
    }
    cJSON *cs = cJSON_AddArrayToObject(o, "clips");
    for (const auto &c : p.clips) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "id", c.id);
        cJSON_AddStringToObject(x, "kind", kinds[(int)c.kind % 3]);
        cJSON_AddNumberToObject(x, "track", c.track);
        cJSON_AddNumberToObject(x, "start", c.start);
        cJSON_AddNumberToObject(x, "len", c.len);
        cJSON_AddNumberToObject(x, "offset", c.offset);
        cJSON_AddNumberToObject(x, "ref", c.ref);
        if (c.muted)
            cJSON_AddBoolToObject(x, "muted", true);
        cJSON_AddItemToArray(cs, x);
    }
    cJSON *ms = cJSON_AddArrayToObject(o, "markers");
    for (const auto &m : p.markers) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "id", m.id);
        cJSON_AddNumberToObject(x, "tick", m.tick);
        cJSON_AddStringToObject(x, "name", m.name.c_str());
        cJSON_AddItemToArray(ms, x);
    }
    return o;
}

cJSON *targetJson(const AutomationTarget &t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", kTargetKinds[t.kind % 10]);
    cJSON_AddNumberToObject(o, "channel", t.channel);
    cJSON_AddNumberToObject(o, "track", t.track);
    cJSON_AddNumberToObject(o, "fx", t.fx);
    cJSON_AddNumberToObject(o, "send", t.send);
    cJSON_AddStringToObject(o, "param", t.param.c_str());
    return o;
}

bool targetFromJson(const cJSON *o, AutomationTarget &t)
{
    if (!cJSON_IsObject(o))
        return false;
    std::string k = str(o, "kind", "none");
    t = AutomationTarget();
    for (int i = 0; i < 10; i++)
        if (k == kTargetKinds[i])
            t.kind = (AutomationTarget::Kind)i;
    t.channel = (Id)num(o, "channel", 0);
    t.track = (int)num(o, "track", 0);
    t.fx = (Id)num(o, "fx", 0);
    t.send = (int)num(o, "send", 0);
    t.param = clampName(str(o, "param"), 48);
    return true;
}

cJSON *automationJson(const Project &p)
{
    cJSON *a = cJSON_CreateArray();
    for (const auto &au : p.automation) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", au.id);
        cJSON_AddStringToObject(o, "name", au.name.c_str());
        cJSON_AddStringToObject(o, "color", colorString(au.color).c_str());
        cJSON_AddNumberToObject(o, "length", au.lengthTicks);
        cJSON_AddItemToObject(o, "target", targetJson(au.target));
        cJSON *pts = cJSON_AddArrayToObject(o, "points");
        for (const auto &pt : au.points)
            cJSON_AddItemToArray(pts, numArray({(double)pt.tick, r4(pt.value), r4(pt.tension)}));
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

cJSON *projectToJson(const Project &p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "format", kProjectFormat);
    bool midi = false;
    for (const auto &pattern : p.patterns) for (const auto &s : pattern.seqs) {
        midi |= !s.midi.empty();
        for (const auto &n : s.notes) midi |= n.midiOnOrder != 0 || n.midiOffOrder != 0;
    }
    // Keep legacy exports byte-compatible; MIDI projects require schema 2 so older firmware
    // refuses them instead of silently losing their expression on a save.
    cJSON_AddNumberToObject(o, "schema", midi ? 2 : 1);
    cJSON *app = cJSON_AddObjectToObject(o, "app");
    cJSON_AddStringToObject(app, "name", "esp32-s3-daw");
    cJSON_AddStringToObject(app, "version", "0.1.0");
    cJSON_AddItemToObject(o, "meta", metaJson(p));
    cJSON_AddNumberToObject(o, "nextId", p.nextId);
    cJSON_AddItemToObject(o, "channels", channelsJson(p));
    cJSON *pats = cJSON_AddArrayToObject(o, "patterns");
    for (const auto &pat : p.patterns)
        cJSON_AddItemToArray(pats, patternJson(pat, -1));
    cJSON_AddItemToObject(o, "mixer", mixerJson(p));
    cJSON_AddItemToObject(o, "playlist", playlistJson(p));
    cJSON_AddItemToObject(o, "automation", automationJson(p));
    cJSON *smp = cJSON_AddArrayToObject(o, "samples");
    for (const auto &s : p.samples) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "id", s.id);
        cJSON_AddStringToObject(x, "name", s.name.c_str());
        cJSON_AddStringToObject(x, "key", s.key.c_str());
        cJSON_AddNumberToObject(x, "frames", s.frames);
        cJSON_AddItemToArray(smp, x);
    }
    return o;
}

std::string projectToText(const Project &p)
{
    // "null" (as when printing fails) when a node could not be made: the text would lack it
    const uint32_t failures = platform::jsonAllocFailures();
    std::string s = jsonText(projectToJson(p));
    return platform::jsonAllocFailures() == failures ? s : "null";
}

cJSON *midiJson(const std::vector<MidiEvent> &events, size_t from)
{
    std::string text = "[";
    bool first = true;
    for (size_t i = from; i < events.size(); ++i) {
        const auto &e = events[i];
        if (!first) text += ',';
        first = false;
        text += '['; appendNumber(text, e.tick); text += ','; appendNumber(text, e.order);
        for (uint8_t b : e.data) { text += ','; appendNumber(text, b); }
        text += ']';
    }
    text += ']';
    return cJSON_CreateRaw(text.c_str());
}

bool midiFromJson(const cJSON *j, std::vector<MidiEvent> &out, std::string *err)
{
    auto fail = [&] { if (err) *err = "invalid MIDI events (expected [tick, order, status, data...])"; return false; };
    if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) > (int)kMaxMidiEvents) return fail();
    std::vector<MidiEvent> events;
    size_t bytes = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, j) {
        if (!cJSON_IsArray(row)) return fail();
        MidiEvent e;
        int i = 0;
        const cJSON *v;
        cJSON_ArrayForEach(v, row) {
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || floor(v->valuedouble) != v->valuedouble) return fail();
            const double n = v->valuedouble;
            if (i == 0) { if (n < 0 || n >= kMaxSpanTicks) return fail(); e.tick = (int32_t)n; }
            else if (i == 1) { if (n < 0 || n > 4294967294.0) return fail(); e.order = (uint32_t)n; }
            else { if (n < 0 || n > 255 || ++bytes > kMaxMidiBytes) return fail(); e.data.push_back((uint8_t)n); }
            ++i;
        }
        if (e.data.empty() || e.data[0] < 0x80) return fail();
        const uint8_t st = e.data[0], hi = st & 0xf0;
        if (st < 0xf0) {
            // Notes remain editable notes, never a second, hidden source of note-ons.
            if (hi == 0x80 || hi == 0x90) return fail();
            if (e.data.size() != (hi == 0xc0 || hi == 0xd0 ? 2u : 3u)) return fail();
            for (size_t k = 1; k < e.data.size(); ++k) if (e.data[k] > 127) return fail();
        } else if (st != 0xf0 && st != 0xf7 && !(st == 0xff && e.data.size() > 1)) {
            const size_t len = st == 0xf2 ? 3 : st == 0xf1 || st == 0xf3 ? 2 : 1;
            if (e.data.size() != len) return fail();
            for (size_t k = 1; k < len; ++k) if (e.data[k] > 127) return fail();
        }
        events.push_back(std::move(e));
    }
    std::stable_sort(events.begin(), events.end(), [](const MidiEvent &a, const MidiEvent &b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    out = std::move(events);
    return true;
}

bool noteFromJson(const cJSON *j, Note &n)
{
    if (cJSON_IsArray(j)) {
        if (cJSON_GetArraySize(j) < 4)
            return false;
        n.id = (Id)at(j, 0, 0);
        n.start = (int32_t)at(j, 1, 0);
        n.len = (int32_t)at(j, 2, kStepTicks);
        n.key = (uint8_t)std::min(127.0, std::max(0.0, at(j, 3, 60)));
        n.vel = (float)at(j, 4, 0.78);
        n.pan = (float)at(j, 5, 0);
        n.pitch = (float)at(j, 6, 0);
        n.rel = (float)at(j, 7, 0.5);
        n.prob = (float)at(j, 8, 1);
        int f = (int)at(j, 9, 0);
        n.slide = f & 1;
        n.accent = f & 2;
        n.midiOnOrder = (uint32_t)std::clamp(at(j, 10, 0), 0.0, 4294967294.0);
        n.midiOffOrder = (uint32_t)std::clamp(at(j, 11, 0), 0.0, 4294967294.0);
        return true;
    }
    if (!cJSON_IsObject(j))
        return false;
    n.id = (Id)num(j, "id", n.id);
    n.start = (int32_t)num(j, "start", n.start);
    n.len = (int32_t)num(j, "len", n.len);
    n.key = (uint8_t)std::min(127.0, std::max(0.0, num(j, "key", n.key)));
    n.vel = (float)num(j, "vel", n.vel);
    n.pan = (float)num(j, "pan", n.pan);
    n.pitch = (float)num(j, "pitch", n.pitch);
    n.rel = (float)num(j, "rel", n.rel);
    n.prob = (float)num(j, "prob", n.prob);
    n.slide = flag(j, "slide", n.slide);
    n.accent = flag(j, "accent", n.accent);
    n.midiOnOrder = (uint32_t)std::clamp(num(j, "midiOnOrder", 0), 0.0, 4294967294.0);
    n.midiOffOrder = (uint32_t)std::clamp(num(j, "midiOffOrder", 0), 0.0, 4294967294.0);
    return true;
}

bool stepFromJson(const cJSON *j, Step &s)
{
    if (cJSON_IsArray(j)) {
        s.on = at(j, 0, 0) != 0;
        s.vel = (float)at(j, 1, 0.78);
        s.pan = (float)at(j, 2, 0);
        s.key = (uint8_t)std::min(127.0, std::max(0.0, at(j, 3, 0)));
        s.accent = at(j, 4, 0) != 0;
        s.prob = (float)at(j, 5, 1);
        return true;
    }
    if (!cJSON_IsObject(j))
        return false;
    s.on = flag(j, "on", s.on);
    s.vel = (float)num(j, "vel", s.vel);
    s.pan = (float)num(j, "pan", s.pan);
    s.key = (uint8_t)std::min(127.0, std::max(0.0, num(j, "key", s.key)));
    s.accent = flag(j, "accent", s.accent);
    s.prob = (float)num(j, "prob", s.prob);
    return true;
}

// Schema 2 adds optional MIDI events/order. Schema 1 has the same defaults when absent.
static bool migrate(cJSON *doc, int from, std::string *err)
{
    (void)doc;
    if (from == 1 || from == Project::kSchema)
        return true;
    if (err)
        *err = "no migration from schema " + std::to_string(from);
    return false;
}

bool projectFromJson(const cJSON *jin, Project &out, std::string *err)
{
    if (!cJSON_IsObject(jin)) {
        if (err)
            *err = "not a JSON object";
        return false;
    }
    if (str(jin, "format") != kProjectFormat) {
        if (err)
            *err = "not an esp32-daw project (format field)";
        return false;
    }
    const int schema = (int)num(jin, "schema", 0);
    if (schema < 1 || schema > Project::kSchema) {
        if (err)
            *err = schema > Project::kSchema ? "project made by a newer version (schema " + std::to_string(schema) + ")"
                                             : "invalid schema";
        return false;
    }
    cJSON *doc = cJSON_Duplicate(jin, true);
    if (!doc) {
        if (err) *err = "not enough memory reading the project";
        return false;
    }
    if (!migrate(doc, schema, err)) {
        cJSON_Delete(doc);
        return false;
    }
    const cJSON *j = doc;
    Project p;
    p.channels.clear();
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(j, "meta");
    p.name = clampName(str(meta, "name", "Untitled"));
    p.author = clampName(str(meta, "author"));
    p.bpm = num(meta, "bpm", 120);
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(meta, "timeSig");
    p.ts.num = (uint8_t)at(ts, 0, 4);
    p.ts.den = (uint8_t)at(ts, 1, 4);
    const cJSON *loop = cJSON_GetObjectItemCaseSensitive(meta, "loop");
    p.loopOn = flag(loop, "on", false);
    p.loopStart = (int32_t)num(loop, "start", 0);
    p.loopEnd = (int32_t)num(loop, "end", 0);
    p.metronome = flag(meta, "metronome", false);
    p.countInBars = (int)num(meta, "countIn", 0);
    p.currentPattern = (Id)num(meta, "currentPattern", 0);
    p.nextId = (Id)num(j, "nextId", 1);

    const cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(j, "channels"))
    {
        Channel c;
        c.id = (Id)num(x, "id", 0);
        c.name = clampName(str(x, "name", "Channel"));
        c.color = parseColor(str(x, "color").c_str(), 0xff9a3c);
        c.icon = clampName(str(x, "icon"), 24);
        c.vol = (float)num(x, "vol", 0.78);
        c.pan = (float)num(x, "pan", 0);
        c.mute = flag(x, "mute", false);
        c.solo = flag(x, "solo", false);
        c.track = (int)num(x, "track", 0);
        c.root = (uint8_t)num(x, "root", 60);
        pluginStateFromJson(cJSON_GetObjectItemCaseSensitive(x, "plugin"), c.inst, g_known);
        if (c.id == 0)
            continue;
        p.channels.push_back(std::move(c));
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(j, "patterns"))
    {
        Pattern pat;
        pat.id = (Id)num(x, "id", 0);
        pat.name = clampName(str(x, "name", "Pattern"));
        pat.color = parseColor(str(x, "color").c_str(), 0x5f8fff);
        pat.lengthTicks = (int32_t)num(x, "length", 0);
        pat.swing = (float)num(x, "swing", 0);
        const cJSON *q;
        cJSON_ArrayForEach(q, cJSON_GetObjectItemCaseSensitive(x, "seqs"))
        {
            ChannelSeq s;
            s.channel = (Id)num(q, "channel", 0);
            s.pianoRoll = flag(q, "pianoRoll", false);
            s.nextNoteId = (Id)num(q, "nextNoteId", 1);
            s.resizeSteps((int)num(q, "stepCount", 16));
            int i = 0;
            const cJSON *st;
            cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(q, "steps"))
            {
                if (i >= s.stepCount)
                    break;
                stepFromJson(st, s.steps[(size_t)i++]);
            }
            const cJSON *nn;
            cJSON_ArrayForEach(nn, cJSON_GetObjectItemCaseSensitive(q, "notes"))
            {
                Note n;
                if (noteFromJson(nn, n) && s.notes.size() < (size_t)kMaxNotesPerSeq)
                    s.notes.push_back(n);
            }
            if (const cJSON *midi = cJSON_GetObjectItemCaseSensitive(q, "midi")) {
                if (!midiFromJson(midi, s.midi, err)) { cJSON_Delete(doc); return false; }
            }
            if (s.channel)
                pat.seqs.push_back(std::move(s));
        }
        if (pat.id)
            p.patterns.push_back(std::move(pat));
    }
    p.ensureMixer();
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(j, "mixer"))
    {
        int idx = (int)num(x, "index", -1);
        if (idx < 0 || idx >= kMaxMixerTracks)
            continue;
        MixerTrack &t = p.mixer[(size_t)idx];
        t.name = clampName(str(x, "name", t.name.c_str()));
        t.color = parseColor(str(x, "color").c_str(), t.color);
        t.vol = (float)num(x, "vol", 0.8);
        t.pan = (float)num(x, "pan", 0);
        t.mute = flag(x, "mute", false);
        t.solo = flag(x, "solo", false);
        t.route = (int)num(x, "route", 0);
        t.sends.clear();
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(x, "sends"))
        {
            Send sd;
            sd.target = (int)num(s, "target", 0);
            sd.level = (float)num(s, "level", 0);
            t.sends.push_back(sd);
        }
        t.fx.clear();
        const cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(x, "fx"))
        {
            FxSlot fx;
            fx.id = (Id)num(f, "id", 0);
            fx.enabled = flag(f, "enabled", true);
            fx.mix = (float)num(f, "mix", 1);
            pluginStateFromJson(cJSON_GetObjectItemCaseSensitive(f, "plugin"), fx.inst, g_known);
            if (fx.id)
                t.fx.push_back(std::move(fx));
        }
    }
    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(j, "playlist");
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(pl, "tracks"))
    {
        PlaylistTrack t;
        t.name = clampName(str(x, "name", "Track"));
        t.color = parseColor(str(x, "color").c_str(), 0x6b7280);
        t.mute = flag(x, "mute", false);
        t.solo = flag(x, "solo", false);
        p.tracks.push_back(t);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(pl, "clips"))
    {
        Clip c;
        c.id = (Id)num(x, "id", 0);
        std::string k = str(x, "kind", "pattern");
        c.kind = k == "automation" ? ClipKind::Automation : (k == "audio" ? ClipKind::Audio : ClipKind::Pattern);
        c.track = (int)num(x, "track", 0);
        c.start = (int32_t)num(x, "start", 0);
        c.len = (int32_t)num(x, "len", kPPQ * 4);
        c.offset = (int32_t)num(x, "offset", 0);
        c.ref = (Id)num(x, "ref", 0);
        c.muted = flag(x, "muted", false);
        if (c.id)
            p.clips.push_back(c);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(pl, "markers"))
    {
        Marker m;
        m.id = (Id)num(x, "id", 0);
        m.tick = (int32_t)num(x, "tick", 0);
        m.name = clampName(str(x, "name"));
        if (m.id)
            p.markers.push_back(m);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(j, "automation"))
    {
        Automation a;
        a.id = (Id)num(x, "id", 0);
        a.name = clampName(str(x, "name", "Automation"));
        a.color = parseColor(str(x, "color").c_str(), 0x40c4aa);
        a.lengthTicks = (int32_t)num(x, "length", kPPQ * 16);
        targetFromJson(cJSON_GetObjectItemCaseSensitive(x, "target"), a.target);
        const cJSON *pt;
        cJSON_ArrayForEach(pt, cJSON_GetObjectItemCaseSensitive(x, "points"))
        {
            AutoPoint ap;
            ap.tick = (int32_t)at(pt, 0, 0);
            ap.value = (float)at(pt, 1, 0.5);
            ap.tension = (float)at(pt, 2, 0);
            a.points.push_back(ap);
        }
        if (a.id)
            p.automation.push_back(std::move(a));
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(j, "samples"))
    {
        SampleRef s;
        s.id = (Id)num(x, "id", 0);
        s.name = clampName(str(x, "name"));
        s.key = clampName(str(x, "key"));
        s.frames = (uint32_t)num(x, "frames", 0);
        s.missing = true;  // no sample storage engine yet: references are kept, not played
        if (s.id)
            p.samples.push_back(s);
    }
    cJSON_Delete(doc);
    p.normalise();
    out = std::move(p);
    return true;
}

bool projectFromText(const char *text, size_t len, Project &p, std::string *err)
{
    cJSON *j = cJSON_ParseWithLength(text, len);
    if (!j) {
        if (err)
            *err = "invalid JSON";
        return false;
    }
    bool ok = projectFromJson(j, p, err);
    cJSON_Delete(j);
    return ok;
}

}  // namespace daw
