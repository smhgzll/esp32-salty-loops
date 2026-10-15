// SPDX-License-Identifier: GPL-3.0-or-later
// The control task (core 0): the only task that touches the DAW session. The HTTP server task
// hands it WebSocket messages and calls through a bounded queue; it ticks the session every 20 ms.
#pragma once

#include <stddef.h>

#include <functional>

#include "daw/session.hpp"

namespace daw::control {

void start(Session *s);
bool postText(int client, char *data, size_t len);  // takes ownership (free()); false = queue full
void postOpen(int client);
void postClose(int client);
// run fn in the control task and wait for it (HTTP handlers); false on timeout / queue full
bool call(const std::function<void()> &fn, uint32_t timeoutMs);
bool booted();

}  // namespace daw::control
