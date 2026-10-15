# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.cpp" "control.cpp" "selftest.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES daw_core daw_hal daw_esp_storage daw_webserver daw_plugins nvs_flash app_update
                                esp_timer esp_system spi_flash esp_psram heap espcoredump)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
