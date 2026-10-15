// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "daw/engine.hpp"

// Allocate one persistent task at startup. nullptr retains the serial renderer.
daw::Engine::Worker *audioWorkerStart(unsigned core, unsigned priority, uint32_t blockPeriodUs);
unsigned audioWorkerStackFree();

int32_t audioWorkerParallel(void *, void (*local)(void *), void *localArg,
                             void (*remote)(void *), void *remoteArg);
