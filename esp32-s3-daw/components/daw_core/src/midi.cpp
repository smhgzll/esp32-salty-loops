// SPDX-License-Identifier: GPL-3.0-or-later
#include "daw/midi.hpp"

#include <algorithm>

namespace daw {

static uint8_t dataBytes(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0:
    case 0xD0: return 1;
    case 0xF0:
        switch (status) {
        case 0xF1:
        case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;
        }
    default: return 2;
    }
}

bool MidiParser::feed(uint8_t b, MidiMsg &out)
{
    if (b >= 0xF8) {  // real-time: never interrupts running status
        out = MidiMsg{b, 0, 0};
        return true;
    }
    if (b == 0xF0) {
        sysex_ = true;
        running_ = 0;
        return false;
    }
    if (b == 0xF7) {
        sysex_ = false;
        return false;
    }
    if (b & 0x80) {
        sysex_ = false;
        running_ = b < 0xF0 ? b : 0;
        need_ = dataBytes(b);
        have_ = 0;
        if (need_ == 0) {
            out = MidiMsg{b, 0, 0};
            return true;
        }
        if (b >= 0xF0)
            running_ = b;  // system common: no running status afterwards, but collect its data
        return false;
    }
    if (sysex_ || !running_)
        return false;
    data_[have_++] = b;
    if (have_ < need_)
        return false;
    out = MidiMsg{running_, data_[0], need_ > 1 ? data_[1] : (uint8_t)0};
    have_ = 0;
    if (running_ >= 0xF0)
        running_ = 0;
    return true;
}

void ClockFollower::reset()
{
    n_ = w_ = count_ = 0;
    bpm_ = avgIv_ = 0;
}

void ClockFollower::tick(double ms)
{
    count_++;
    if (n_ > 0) {
        const double d = ms - last_;
        if (d <= 0.2 || (avgIv_ > 0 && d < avgIv_ * 0.5))
            return;  // a duplicate or a burst: ignore
        if (d > 250.0 || (avgIv_ > 0 && d > avgIv_ * 1.8)) {
            n_ = 0;  // a gap (dropped ticks, a pause): restart the window here
            w_ = 0;
        } else {
            avgIv_ = avgIv_ == 0 ? d : avgIv_ * 0.9 + d * 0.1;
        }
    }
    last_ = ms;
    t_[w_] = ms;
    w_ = (w_ + 1) % (kSpan + 1);
    if (n_ <= kSpan)
        n_++;
    if (n_ > kSpan) {  // full: t_[w_] is the oldest, the one just written the newest
        const double span = ms - t_[w_];
        const double est = 60000.0 / (span / kSpan * 24.0);
        bpm_ = bpm_ == 0 ? est : bpm_ + (est - bpm_) * 0.25;
    }
}

}  // namespace daw
