// SPDX-License-Identifier: GPL-3.0-or-later
// The project file format ("esp32-daw-project", schemas 1/2) and the JSON encodings of its entities,
// shared by storage, export / import and the WebSocket protocol (docs/STORAGE.md, PROTOCOL.md).
#pragma once

#include <string>

#include "daw/model.hpp"

struct cJSON;

namespace daw {

constexpr const char *kProjectFormat = "esp32-daw-project";

cJSON *projectToJson(const Project &p);
// false + err on corrupt / unsupported input; `p` is only written on success
bool projectFromJson(const cJSON *j, Project &p, std::string *err);
bool projectFromText(const char *text, size_t len, Project &p, std::string *err);
std::string projectToText(const Project &p);

// entity encoders (protocol patches)
cJSON *metaJson(const Project &p);
cJSON *channelJson(const Channel &c);
cJSON *channelsJson(const Project &p);
cJSON *patternJson(const Pattern &p, int32_t length);
cJSON *patternsJson(const Project &p);
cJSON *mixerJson(const Project &p);
cJSON *trackJson(const MixerTrack &t);
cJSON *playlistJson(const Project &p);
cJSON *automationJson(const Project &p);
cJSON *pluginStateJson(const PluginState &s);

// entity decoders used by protocol operations
// Strict, transactional MIDI decoding shared by project loading and editing operations.
bool midiFromJson(const cJSON *j, std::vector<MidiEvent> &out, std::string *err = nullptr);
cJSON *midiJson(const std::vector<MidiEvent> &events, size_t from = 0);
bool noteFromJson(const cJSON *j, Note &n);  // array or object form
bool stepFromJson(const cJSON *j, Step &s);
bool targetFromJson(const cJSON *j, AutomationTarget &t);
cJSON *targetJson(const AutomationTarget &t);

// set by the session: is this plugin in the firmware? Instances of unknown plugins load as
// "missing" and are written back verbatim.
void setPluginKnownCheck(bool (*fn)(const std::string &id));

std::string colorString(uint32_t rgb);
uint32_t parseColor(const char *s, uint32_t def);
std::string jsonText(cJSON *j, bool free = true);  // unformatted

}  // namespace daw
