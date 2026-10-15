# SPDX-License-Identifier: MIT
# daw_add_plugin(NAME <name> ENTRY <c symbol> DIR <dir> SOURCES <files...>
#                [EXTRA_SOURCES <absolute paths...>] [INCLUDES <dirs...>] [DEFINES <defs...>])
#
# Builds lib/libdaw_plugin_<name>.a, the archive a package carries. The archive holds ONE
# relocatable object (ld -r of all sources) in which every symbol except the entry point is made
# local (objcopy --keep-global-symbol). Two plugins vendoring the same library (DaisySP, ...) or
# using different SDK helper versions therefore never collide when linked into one firmware.
# Registers the plugin for the host tools (global properties DAWSDK_PLUGINS / _ENTRIES).
set(DAWSDK_PLUGIN_FLAGS
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>
  -ffunction-sections -fdata-sections -ffp-contract=off
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(NOT CMAKE_OBJCOPY)
  find_program(CMAKE_OBJCOPY objcopy REQUIRED)
endif()
if(NOT CMAKE_LINKER)
  find_program(CMAKE_LINKER ld REQUIRED)
endif()

function(daw_add_plugin)
  cmake_parse_arguments(P "" "NAME;ENTRY;DIR" "SOURCES;EXTRA_SOURCES;INCLUDES;DEFINES;OPTIONS" ${ARGN})
  set(t dawplug_${P_NAME})
  list(TRANSFORM P_SOURCES PREPEND ${P_DIR}/)
  add_library(${t}_obj OBJECT ${P_SOURCES} ${P_EXTRA_SOURCES})
  target_link_libraries(${t}_obj PUBLIC dawsdk_api)
  target_include_directories(${t}_obj PRIVATE ${P_DIR} ${P_INCLUDES})
  target_compile_definitions(${t}_obj PRIVATE NDEBUG ${P_DEFINES})
  # Keep IEEE finite/NaN guards and arithmetic order; permit inlining math without errno.
  target_compile_options(${t}_obj PRIVATE ${DAWSDK_PLUGIN_FLAGS} -O3 -fno-math-errno ${P_OPTIONS})

  set(_dir ${CMAKE_BINARY_DIR}/lib)
  set(_obj ${CMAKE_CURRENT_BINARY_DIR}/plugin_${P_NAME}.o)
  set(_lib ${_dir}/libdaw_plugin_${P_NAME}.a)
  add_custom_command(OUTPUT ${_lib}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}
    COMMAND ${CMAKE_LINKER} -r -o ${_obj} $<TARGET_OBJECTS:${t}_obj>
    COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=${P_ENTRY} ${_obj}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${_lib}
    COMMAND ${CMAKE_AR} rcsD ${_lib} ${_obj}
    DEPENDS ${t}_obj $<TARGET_OBJECTS:${t}_obj>
    COMMAND_EXPAND_LISTS VERBATIM
    COMMENT "plugin ${P_NAME}: single object, only ${P_ENTRY} global")
  add_custom_target(${t}_archive ALL DEPENDS ${_lib})
  add_library(${t} STATIC IMPORTED GLOBAL)
  set_target_properties(${t} PROPERTIES IMPORTED_LOCATION ${_lib})
  add_dependencies(${t} ${t}_archive)
  target_link_libraries(${t} INTERFACE dawsdk_api)
  set_property(GLOBAL APPEND PROPERTY DAWSDK_PLUGINS ${P_NAME})
  set_property(GLOBAL APPEND PROPERTY DAWSDK_PLUGIN_ENTRIES ${P_ENTRY})
endfunction()
