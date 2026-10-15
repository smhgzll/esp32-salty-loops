/* SPDX-License-Identifier: GPL-3.0-or-later
 * The PCM5102A pin assignment, checked at build time and at boot. One place for the wiring:
 * Kconfig (main/Kconfig.projbuild) defaults are the MEASURED pins GPIO16 / GPIO6 / GPIO7. */
#pragma once

#include "sdkconfig.h"

#define DAW_PIN_BCLK CONFIG_DAW_I2S_BCLK_GPIO
#define DAW_PIN_WS CONFIG_DAW_I2S_WS_GPIO
#define DAW_PIN_DOUT CONFIG_DAW_I2S_DOUT_GPIO

/* pins an N16R16 module must not use for I2S:
 *   26..37  flash and octal PSRAM          19, 20  native USB (USB-Serial/JTAG, TinyUSB)
 *   43, 44  UART0 console                  0, 3, 45, 46  strapping pins */
#define DAW_PIN_FORBIDDEN(p) \
    (((p) >= 26 && (p) <= 37) || (p) == 19 || (p) == 20 || (p) == 43 || (p) == 44 || (p) == 0 || (p) == 3 || (p) == 45 || (p) == 46 || (p) > 48)

#if DAW_PIN_BCLK >= 0 && DAW_PIN_FORBIDDEN(DAW_PIN_BCLK)
#error "CONFIG_DAW_I2S_BCLK_GPIO uses a reserved pin (flash/PSRAM, USB, UART0 or strapping)"
#endif
#if DAW_PIN_WS >= 0 && DAW_PIN_FORBIDDEN(DAW_PIN_WS)
#error "CONFIG_DAW_I2S_WS_GPIO uses a reserved pin (flash/PSRAM, USB, UART0 or strapping)"
#endif
#if DAW_PIN_DOUT >= 0 && DAW_PIN_FORBIDDEN(DAW_PIN_DOUT)
#error "CONFIG_DAW_I2S_DOUT_GPIO uses a reserved pin (flash/PSRAM, USB, UART0 or strapping)"
#endif
#if (DAW_PIN_BCLK >= 0 && (DAW_PIN_BCLK == DAW_PIN_WS || DAW_PIN_BCLK == DAW_PIN_DOUT)) || (DAW_PIN_WS >= 0 && DAW_PIN_WS == DAW_PIN_DOUT)
#error "two I2S signals are assigned to the same GPIO"
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* the same checks at run time (pins may come from a Kconfig override); NULL if fine, else why */
const char *daw_pins_check(int bclk, int ws, int dout);
#ifdef __cplusplus
}
#endif
