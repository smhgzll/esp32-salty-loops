// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "daw/engine.hpp"
#include "daw/session.hpp"

namespace daw::selftest {
// before the live engine exists: an offline render of the demo project through the real plugins
// (prints DAW_RENDER stats for comparison with the Linux build)
bool offlineRender();
// after boot: storage, persistence across a restart, heap integrity; prints DAW_SELFTEST_PASS / FAIL
void startLive(Session *s, Engine *e);
}  // namespace daw::selftest
