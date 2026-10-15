# SPDX-License-Identifier: GPL-3.0-or-later
# The imported plugin packages (scripts/import-plugins.py): the generated registry and the
# packages' prebuilt ESP32-S3 archives.
get_filename_component(DAW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
if(NOT EXISTS ${DAW_ROOT}/plugins/generated/plugins.cmake)
  message(FATAL_ERROR "no plugins imported: run scripts/import-plugins.sh")
endif()
include(${DAW_ROOT}/plugins/generated/plugins.cmake)
idf_component_register(SRCS "${DAW_ROOT}/plugins/generated/daw_plugin_registry.c"
                       PRIV_INCLUDE_DIRS "${DAW_ROOT}/third_party/daw_plugin_abi")
set(_i 0)
foreach(lib ${DAW_PLUGIN_LIBS_ESP32S3})
  if(NOT EXISTS ${lib})
    message(FATAL_ERROR "missing plugin library ${lib}: run scripts/import-plugins.sh")
  endif()
  add_prebuilt_library(daw_plugin_${_i} ${lib})
  target_link_libraries(${COMPONENT_LIB} PRIVATE daw_plugin_${_i})
  math(EXPR _i "${_i} + 1")
endforeach()
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
