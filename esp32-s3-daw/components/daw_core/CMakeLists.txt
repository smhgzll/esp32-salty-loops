# SPDX-License-Identifier: GPL-3.0-or-later
# The portable DAW core. As an ESP-IDF component it uses IDF's cJSON (json); the Linux host build
# (host/CMakeLists.txt) compiles the same sources with third_party/cJSON.
idf_component_register(SRCS "src/compile.cpp" "src/engine.cpp" "src/midi.cpp" "src/model.cpp"
                            "src/plugin_host.cpp" "src/project_json.cpp" "src/session.cpp" "src/store.cpp"
                            "src/sample_store.cpp" "src/audio_file.cpp" "src/preset_library.cpp" "src/settings_update.cpp" "src/sf2_info.cpp" "src/sf2_subset.cpp"
                       INCLUDE_DIRS "include" "../../third_party/daw_plugin_abi"
                       REQUIRES json
                       PRIV_REQUIRES daw_plugins)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# Optimize only the audio/sequencer loop; keep the control/JSON code compact.
set_source_files_properties(src/engine.cpp PROPERTIES COMPILE_OPTIONS "-O3;-fno-math-errno")
