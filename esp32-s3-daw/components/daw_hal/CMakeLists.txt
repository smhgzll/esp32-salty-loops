# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "audio_out.cpp" "audio_worker.cpp" "platform_esp.cpp" "pins.c"
                       INCLUDE_DIRS "include"
                       REQUIRES daw_core esp_driver_i2s esp_timer esp_hw_support esp_rom heap log json)
