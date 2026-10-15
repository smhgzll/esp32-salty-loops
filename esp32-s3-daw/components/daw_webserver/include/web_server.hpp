// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware's network side. All callbacks run in the HTTP server task (core 0); the owner
// (main/control.cpp) forwards them to the control task, which owns the DAW session.
#pragma once

#include <stdint.h>

#include <functional>
#include <string>

#include "daw/session.hpp"
#include "esp_err.h"

namespace daw::web {

struct Callbacks {
    std::function<void(int client)> onOpen;
    std::function<void(int client)> onClose;
    std::function<bool(int client, char *data, size_t len)> onText;  // takes ownership of data (free())
    std::function<std::string()> status;                             // GET /api/status
    std::function<bool(std::string &out)> exportProject;             // GET /api/project
    std::function<bool(std::string &in, std::string &err)> importProject;  // POST /api/project
    std::function<void()> beforeFlashWork;                           // OTA / factory reset: stop audio work
    std::function<void()> factoryReset;
    // the sample library: GET /api/samples (JSON), POST /api/samples?name=... (the file as the body)
    std::function<std::string()> samplesJson;
    std::function<bool(const std::string &name, const std::string &data, std::string &err, bool reclaimModules)> importSample;
    // a SoundFont larger than 1 MB arrives in pieces, straight into the library (no RAM copy)
    std::function<bool(const std::string &name, size_t size, const std::string &head, std::string &err, bool reclaimModules)> sampleBegin;
    std::function<bool(const std::string &piece, std::string &err)> sampleAppend;
    std::function<bool(std::string &err)> sampleFinish;
    std::function<void()> sampleAbort;
    // device settings (admin token): GET /api/settings (no passwords), POST /api/settings (an update)
    std::function<std::string()> settingsJson;
    std::function<bool(const std::string &body, std::string &err)> saveSettings;
    // QEMU builds only (the route does not exist otherwise): make the next OTA boot's health check
    // fail, to test the rollback path in emulation (POST /api/test/fail-next-health, token)
    std::function<void()> testFailNextHealth;
};

// network (Wi-Fi AP / STA, or OpenCores Ethernet under QEMU)
esp_err_t netStart(const char *mode, const char *ssid, const char *password, const char *hostname);
const char *netAddress(uint32_t waitMs);

esp_err_t start(const Callbacks &cb, uint16_t port, const std::string &adminPassword);
// queue a text frame (thread-safe). Live messages are dropped when the client is behind; a dropped
// State message marks the client for a resync (takeResync()).
void send(int client, const std::string &text, MsgClass cls);
int takeResync();  // a client that needs a full snapshot, or -1
void kick();       // retry scheduling queued frames (the control task calls it every tick)
void setAdminPassword(const std::string &pw);  // a new admin password: every login token is revoked
uint32_t clientCount();
uint32_t droppedFrames();

// OTA: whether the image in `data` (its first bytes) is a firmware for this device and profile
const char *otaCheckImageHeader(const uint8_t *data, size_t len);

}  // namespace daw::web
