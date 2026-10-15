# SPDX-License-Identifier: MIT
# esp32-daw-plugin-sdk: plugins (static libraries), host test harness, tools and tests.
#   host:   cmake -S . -B build-host            (plugins + tools + tests, native)
#   esp32:  cmake -S . -B build-esp32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-esp32s3.cmake
#           (plugins only, cross-compiled for the ESP32-S3; see scripts/build-esp32.sh)
cmake_minimum_required(VERSION 3.16)
project(esp32_daw_plugin_sdk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CROSSCOMPILING)
  set(DAWSDK_TARGET_ARCH "esp32s3")
  set(_tools_default OFF)
else()
  set(DAWSDK_TARGET_ARCH "host-${CMAKE_SYSTEM_PROCESSOR}-linux")
  set(_tools_default ON)
endif()
option(DAWSDK_BUILD_TOOLS "Build the host tools and tests" ${_tools_default})

# the public ABI (C) and the C++ helper layer (header only)
add_library(dawsdk_api INTERFACE)
target_include_directories(dawsdk_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                ${CMAKE_CURRENT_SOURCE_DIR}/core/include)

include(cmake/DawPlugin.cmake)
include(plugins.cmake)

if(DAWSDK_BUILD_TOOLS)
  enable_testing()
  add_subdirectory(host)
  add_subdirectory(tools)
  add_subdirectory(tests)
endif()
