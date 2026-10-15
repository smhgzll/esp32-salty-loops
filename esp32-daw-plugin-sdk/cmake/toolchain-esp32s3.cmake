# SPDX-License-Identifier: MIT
# Cross-compile plugin libraries for the ESP32-S3 with Espressif's GCC (xtensa-esp32s3-elf-*), with
# the code-generation flags ESP-IDF v5.5 uses for this chip, so the archives link into an IDF app.
# The compiler comes from PATH (source $IDF_PATH/export.sh) or $ENV{DAWSDK_XTENSA_BIN}.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)
set(_pfx xtensa-esp32s3-elf-)
if(DEFINED ENV{DAWSDK_XTENSA_BIN})
  set(_pfx $ENV{DAWSDK_XTENSA_BIN}/xtensa-esp32s3-elf-)
endif()
set(CMAKE_C_COMPILER ${_pfx}gcc)
set(CMAKE_CXX_COMPILER ${_pfx}g++)
set(CMAKE_ASM_COMPILER ${_pfx}gcc)
set(CMAKE_AR ${_pfx}ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${_pfx}ranlib CACHE FILEPATH "")
set(CMAKE_LINKER ${_pfx}ld CACHE FILEPATH "")
set(CMAKE_OBJCOPY ${_pfx}objcopy CACHE FILEPATH "")
set(CMAKE_NM ${_pfx}nm CACHE FILEPATH "")
set(_flags "-mlongcalls -mdisable-hardware-atomics -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-bzero")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
