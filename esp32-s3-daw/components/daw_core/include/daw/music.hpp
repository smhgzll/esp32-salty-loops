// SPDX-License-Identifier: GPL-3.0-or-later
// Musical time. Positions are integer ticks at 960 PPQN (quarter notes); the audio thread maps
// them onto its monotonic sample clock block by block.
#pragma once

#include <stdint.h>

namespace daw {

constexpr int32_t kPPQ = 960;
constexpr double kMinBpm = 20.0;
constexpr double kMaxBpm = 300.0;

struct TimeSig {
    uint8_t num = 4;
    uint8_t den = 4;
    int32_t ticksPerBeat() const { return kPPQ * 4 / den; }
    int32_t ticksPerBar() const { return ticksPerBeat() * num; }
};

struct BarBeatTick {
    int32_t bar, beat, tick;  // 1-based bar and beat
};

inline BarBeatTick toBbt(int64_t ticks, TimeSig ts)
{
    if (ticks < 0)
        ticks = 0;
    const int64_t bar = ticks / ts.ticksPerBar();
    const int64_t inBar = ticks % ts.ticksPerBar();
    return {(int32_t)bar + 1, (int32_t)(inBar / ts.ticksPerBeat()) + 1, (int32_t)(inBar % ts.ticksPerBeat())};
}

inline double ticksPerSample(double bpm, double sampleRate) { return bpm * kPPQ / (60.0 * sampleRate); }

inline int32_t roundUpTo(int32_t v, int32_t q) { return q <= 0 ? v : ((v + q - 1) / q) * q; }

}  // namespace daw
