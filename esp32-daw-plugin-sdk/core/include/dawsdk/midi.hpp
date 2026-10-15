// SPDX-License-Identifier: MIT
// MIDI 1.0 channel state shared by import-aware instruments. No allocation or I/O.
#pragma once
#include <stdint.h>
#include "daw_plugin_events.h"
namespace dawsdk { inline namespace v1 {
struct MidiControls {
    uint16_t volume = 100 << 7, expression = 16383, pan = 8192, balance = 8192;
    uint16_t rpn = 0x3fff, nrpn = 0x3fff, data = 0;
    uint16_t fine = 8192;
    int coarse = 64;
    float range = 2, wheel = 0, master = 1, masterFine = 0, masterCoarse = 0;
    uint8_t soft = 0, modulation = 0, pressure = 0;
    bool nrpnSelected = false;
    void resetControllers() {
        expression = 16383; rpn = nrpn = 0x3fff; data = 0; nrpnSelected = false;
        wheel = 0; soft = modulation = pressure = 0;
    }
    bool cc(uint8_t number, uint8_t value) {
        auto msb = [&](uint16_t &v) { v = (v & 127) | (uint16_t(value) << 7); };
        auto lsb = [&](uint16_t &v) { v = (v & 0x3f80) | value; };
        auto selected = [&] {
            if (rpn == 0) data = (uint16_t(range) << 7) | uint16_t((range - int(range)) * 100 + .5f);
            else if (rpn == 1) data = fine;
            else if (rpn == 2) data = uint16_t(coarse) << 7;
            else data = 0;
        };
        const int step = rpn == 0 || rpn == 2 ? 128 : 1;
        bool pitch = false;
        switch (number) {
        case 7: volume = uint16_t(value) << 7; break; case 39: lsb(volume); break;
        case 11: expression = uint16_t(value) << 7; break; case 43: lsb(expression); break;
        case 10: pan = uint16_t(value) << 7; break; case 42: lsb(pan); break;
        case 8: balance = uint16_t(value) << 7; break; case 40: lsb(balance); break;
        case 1: modulation = value; break;
        case 67: soft = value; break;
        case 101: msb(rpn); nrpnSelected = false; selected(); break;
        case 100: lsb(rpn); nrpnSelected = false; selected(); break;
        case 99: msb(nrpn); nrpnSelected = true; break;
        case 98: lsb(nrpn); nrpnSelected = true; break;
        case 6: data = uint16_t(value) << 7; pitch = true; break;
        case 38: lsb(data); pitch = true; break;
        case 96: data = uint16_t(int(data) + step > 16383 ? 16383 : int(data) + step); pitch = true; break;
        case 97: data = uint16_t(data < step ? 0 : data - step); pitch = true; break;
        case 121: resetControllers(); return true;
        default: break;
        }
        if (pitch && !nrpnSelected) {
            if (rpn == 0) range = (data >> 7) + (data & 127) * .01f;
            else if (rpn == 1) fine = data;
            else if (rpn == 2) coarse = data >> 7;
            else pitch = false;
        } else pitch = false;
        return pitch;
    }
    float tuning() const { return coarse - 64 + (fine - 8192) / 8192.f + masterFine + masterCoarse; }
    float gain() const {
        // Relative to GM's initial CC7=100. Preserve note velocity separately.
        const float g = (volume / 12800.f) * (expression / 16383.f);
        return master * g * g;
    }
    void gains(float &l, float &r) const {
        l = r = gain();
        const float p = (int(pan) - 8192) / 8192.f, b = (int(balance) - 8192) / 8192.f;
        if (p > 0) l *= 1 - p; else r *= 1 + p;
        if (b > 0) l *= 1 - b; else r *= 1 + b;
    }
};
// Universal real-time device control. Complete F0...F7 packets only; unknown data stays opaque.
inline bool midiDeviceControl(const uint8_t *d, uint32_t n, MidiControls &c, bool &reset) {
    reset = false;
    if (!d || n < 6 || d[0] != 0xf0 || d[n-1] != 0xf7) return false;
    if (n != 6 && n != 8 && n != 9 && n != 11) return false;
    for (uint32_t i = 1; i + 1 < n; ++i) if (d[i] > 127) return false;
    // Roland GS DT1: validate the seven-bit address/data checksum before applying it.
    if (n == 11 && d[1] == 0x41 && d[3] == 0x42 && d[4] == 0x12 && d[5] == 0x40 && d[6] == 0 &&
        ((d[5] + d[6] + d[7] + d[8] + d[9]) & 127) == 0) {
        if (d[7] == 0x7f && d[8] == 0) reset = true;
        else if (d[7] == 4) c.master = d[8] / 127.f;
        else if (d[7] == 5) c.masterCoarse = int(d[8]) - 64.f;
        else return false;
        return true;
    }
    // Yamaha XG parameter change, global system address 00 00 xx.
    if (n == 9 && d[1] == 0x43 && (d[2] & 0xf0) == 0x10 && d[3] == 0x4c && d[4] == 0 && d[5] == 0) {
        if ((d[6] == 0x7e || d[6] == 0x7f) && d[7] == 0) reset = true;
        else if (d[6] == 4) c.master = d[7] / 127.f;
        else if (d[6] == 6) c.masterCoarse = int(d[7]) - 64.f;
        else return false;
        return true;
    }
    if (n == 6 && d[1] == 0x7e && d[3] == 9 && (d[4] == 1 || d[4] == 3)) { reset = true; return true; }
    if (n == 8 && d[1] == 0x7f && d[3] == 4 && d[5] < 128 && d[6] < 128) {
        const int v = d[5] | d[6] << 7;
        if (d[4] == 1) c.master = v / 16383.f;
        else if (d[4] == 2) c.balance = (uint16_t)v;
        else if (d[4] == 3) c.masterFine = (v - 8192) / 8192.f;
        else if (d[4] == 4) c.masterCoarse = int(d[6]) - 64.f;
        else return false;
        return true;
    }
    return false;
}
} }
