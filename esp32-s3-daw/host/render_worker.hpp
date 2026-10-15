// SPDX-License-Identifier: GPL-3.0-or-later
// Persistent native worker for testing the same start/join contract as the ESP32 task.
#pragma once
#include <semaphore>
#include <thread>
#include "daw/engine.hpp"

class RenderWorker {
    std::binary_semaphore pending_{0}, finished_{0};
    void (*function_)(void *) = nullptr;
    void *argument_ = nullptr;
    std::thread thread_;
    void loop()
    {
        for (;;) {
            pending_.acquire();
            if (!function_) return;
            function_(argument_);
            finished_.release();
        }
    }
public:
    daw::Engine::Worker api{this,
        [](void *self, void (*function)(void *), void *argument) {
            auto &w = *static_cast<RenderWorker *>(self);
            w.function_ = function; w.argument_ = argument; w.pending_.release();
        },
        [](void *self) { static_cast<RenderWorker *>(self)->finished_.acquire(); }};
    RenderWorker() : thread_([this] { loop(); }) {}
    ~RenderWorker() { function_ = nullptr; pending_.release(); thread_.join(); }
};
