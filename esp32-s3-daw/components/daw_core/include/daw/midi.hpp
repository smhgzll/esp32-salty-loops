// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI byte-stream parsing and external clock following (transport-independent).
#pragma once

#include <stdint.h>

namespace daw {

struct MidiMsg {
    uint8_t status = 0, d1 = 0, d2 = 0;
    uint8_t type() const { return status >= 0xF0 ? status : (uint8_t)(status & 0xF0); }
    uint8_t channel() const { return status & 0x0F; }
};

// running status, interleaved real-time bytes, SysEx skipped
class MidiParser {
public:
    // true when `out` holds a complete message
    bool feed(uint8_t b, MidiMsg &out);

private:
    uint8_t running_ = 0, data_[2] = {}, have_ = 0, need_ = 0;
    bool sysex_ = false;
};

// Follows an external MIDI clock (24 ticks per quarter note). The tempo comes from the sender's
// timestamps when the transport provides them (the browser / ALSA bridge stamp each tick), so
// network jitter in packet arrival does not become tempo jitter. The estimate is the time spanned
// by the last 48 ticks (two beats): per-tick jitter only enters through the two end points.
// A tick much later than expected (a dropped tick, a pause) restarts the window; a tick much
// earlier (a duplicate) is ignored. Arrival times are only a fallback.
class ClockFollower {
public:
    void reset();
    void tick(double ms);  // timestamp of the 0xF8, milliseconds
    bool locked() const { return n_ > kSpan; }
    double bpm() const { return bpm_; }
    uint32_t ticks() const { return count_; }

private:
    static constexpr uint32_t kSpan = 48;
    double t_[kSpan + 1] = {}, bpm_ = 0, last_ = 0, avgIv_ = 0;
    uint32_t n_ = 0, w_ = 0, count_ = 0;  // n_: timestamps in the ring (up to kSpan + 1)
};

}  // namespace daw
