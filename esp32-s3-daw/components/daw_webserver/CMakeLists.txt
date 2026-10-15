# SPDX-License-Identifier: GPL-3.0-or-later
# HTTP + WebSocket server, network bring-up, browser firmware updates. The browser DAW is embedded
# from generated/ (scripts/embed-web.py writes it from web/dist before every firmware build).
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/generated/web_index.c)
  message(FATAL_ERROR "web assets not generated: run scripts/embed-web.py (the build scripts do)")
endif()
set(_reqs daw_core daw_esp_storage esp_http_server esp_netif esp_event esp_wifi esp_eth app_update json esp_timer nvs_flash)
idf_component_register(SRCS "net.cpp" "web_server.cpp" "ota.cpp" "generated/web_index.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ${_reqs})
target_add_binary_data(${COMPONENT_LIB} "generated/web_bundle.bin" BINARY)
if(CONFIG_DAW_MDNS)
  idf_component_optional_requires(PRIVATE mdns)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
