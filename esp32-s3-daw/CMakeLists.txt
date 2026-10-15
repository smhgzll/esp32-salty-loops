# SPDX-License-Identifier: GPL-3.0-or-later
# esp32-s3-daw firmware (ESP-IDF v5.5.5). Build with scripts/build-hardware.sh or
# scripts/build-qemu.sh; the Linux host build is host/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS_BASE "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_daw)
