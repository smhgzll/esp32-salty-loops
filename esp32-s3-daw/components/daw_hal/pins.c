/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "daw_pins.h"

const char *daw_pins_check(int bclk, int ws, int dout)
{
    const int p[3] = {bclk, ws, dout};
    for (int i = 0; i < 3; i++) {
        if (p[i] < 0)
            return "a pin is not configured (-1)";
        if (DAW_PIN_FORBIDDEN(p[i]))
            return "a pin is reserved (flash/PSRAM 26-37, USB 19-20, UART0 43-44, strapping 0/3/45/46)";
    }
    if (bclk == ws || bclk == dout || ws == dout)
        return "two signals share a GPIO";
    return 0;
}
