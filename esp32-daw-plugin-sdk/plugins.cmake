# SPDX-License-Identifier: MIT
# The plugins this SDK builds. Each one: a directory with its sources and a plugin.json.
daw_add_plugin(NAME sine ENTRY daw_plugin_entry_sine DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/sine
               SOURCES sine.cpp)
daw_add_plugin(NAME 3xosc ENTRY daw_plugin_entry_3xosc DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/3xosc
               SOURCES threexosc.cpp)
daw_add_plugin(NAME fm6 ENTRY daw_plugin_entry_fm6 DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/fm6 SOURCES fm6.cpp)
daw_add_plugin(NAME sampler ENTRY daw_plugin_entry_sampler DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/sampler SOURCES sampler.cpp)
# DaisySP's drum models, compiled into DrumLab's object under a private namespace name
set(_daisy ${CMAKE_CURRENT_LIST_DIR}/third_party/DaisySP/Source)
daw_add_plugin(NAME drumlab ENTRY daw_plugin_entry_drumlab DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/drumlab
               SOURCES drumlab.cpp
               EXTRA_SOURCES ${_daisy}/Drums/analogbassdrum.cpp ${_daisy}/Drums/analogsnaredrum.cpp
                             ${_daisy}/Drums/hihat.cpp ${_daisy}/Drums/synthbassdrum.cpp
                             ${_daisy}/Drums/synthsnaredrum.cpp ${_daisy}/Filters/svf.cpp
                             ${_daisy}/Synthesis/oscillator.cpp
               INCLUDES ${_daisy} ${_daisy}/Utility
               OPTIONS -Wno-unused-variable -Wno-sign-compare -Wno-implicit-fallthrough -Wno-extra)
daw_add_plugin(NAME fx_utility ENTRY daw_plugin_entry_fx_utility DIR ${CMAKE_CURRENT_LIST_DIR}/effects/utility SOURCES utility.cpp)
daw_add_plugin(NAME fx_filter ENTRY daw_plugin_entry_fx_filter DIR ${CMAKE_CURRENT_LIST_DIR}/effects/filter SOURCES filter.cpp)
daw_add_plugin(NAME fx_delay ENTRY daw_plugin_entry_fx_delay DIR ${CMAKE_CURRENT_LIST_DIR}/effects/delay SOURCES delay.cpp)
daw_add_plugin(NAME fx_limiter ENTRY daw_plugin_entry_fx_limiter DIR ${CMAKE_CURRENT_LIST_DIR}/effects/limiter SOURCES limiter.cpp)
daw_add_plugin(NAME fx_reverb ENTRY daw_plugin_entry_fx_reverb DIR ${CMAKE_CURRENT_LIST_DIR}/effects/reverb SOURCES reverb.cpp)
daw_add_plugin(NAME fx_chorus ENTRY daw_plugin_entry_fx_chorus DIR ${CMAKE_CURRENT_LIST_DIR}/effects/chorus SOURCES chorus.cpp)
daw_add_plugin(NAME fx_flanger ENTRY daw_plugin_entry_fx_flanger DIR ${CMAKE_CURRENT_LIST_DIR}/effects/flanger SOURCES flanger.cpp)
daw_add_plugin(NAME fx_eq ENTRY daw_plugin_entry_fx_eq DIR ${CMAKE_CURRENT_LIST_DIR}/effects/eq SOURCES eq.cpp)
daw_add_plugin(NAME fx_compressor ENTRY daw_plugin_entry_fx_compressor DIR ${CMAKE_CURRENT_LIST_DIR}/effects/compressor SOURCES compressor.cpp)
daw_add_plugin(NAME fx_distortion ENTRY daw_plugin_entry_fx_distortion DIR ${CMAKE_CURRENT_LIST_DIR}/effects/distortion SOURCES distortion.cpp)
# examples (built and validated with the others; scripts/package.sh example_tremolo packs it)
daw_add_plugin(NAME example_tremolo ENTRY daw_plugin_entry_example_tremolo
               DIR ${CMAKE_CURRENT_LIST_DIR}/examples/c-tremolo SOURCES tremolo.c)
# Acid303: Open303's TB-303 filter / envelopes (single precision, see third_party/patches)
set(_o303 ${CMAKE_CURRENT_LIST_DIR}/third_party/Open303/Source/DSPCode)
daw_add_plugin(NAME acid303 ENTRY daw_plugin_entry_acid303 DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/acid303
               SOURCES acid303.cpp
               EXTRA_SOURCES ${_o303}/rosic_TeeBeeFilter.cpp ${_o303}/rosic_AnalogEnvelope.cpp ${_o303}/rosic_DecayEnvelope.cpp
                             ${_o303}/rosic_LeakyIntegrator.cpp ${_o303}/rosic_OnePoleFilter.cpp ${_o303}/rosic_BiquadFilter.cpp
               INCLUDES ${_o303}
               OPTIONS -fsingle-precision-constant -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-extra
                       -Wno-double-promotion -Wno-float-conversion)
# SoundFont: TinySoundFont (single header, voice loop in single precision, see third_party/patches)
daw_add_plugin(NAME soundfont ENTRY daw_plugin_entry_soundfont DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/soundfont
               SOURCES soundfont.cpp
               INCLUDES ${CMAKE_CURRENT_LIST_DIR}/third_party/TinySoundFont
               OPTIONS -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-extra -Wno-unused-function
                       -Wno-implicit-fallthrough -Wno-missing-field-initializers -Wno-double-promotion -Wno-float-conversion)
# MacroSynth: Mutable Instruments Braids' macro oscillator (integer DSP, see third_party/braids)
set(_braids ${CMAKE_CURRENT_LIST_DIR}/third_party/braids)
daw_add_plugin(NAME macrosynth ENTRY daw_plugin_entry_macrosynth DIR ${CMAKE_CURRENT_LIST_DIR}/plugins/macrosynth
               SOURCES macrosynth.cpp
               EXTRA_SOURCES ${_braids}/braids/macro_oscillator.cc ${_braids}/braids/analog_oscillator.cc
                             ${_braids}/braids/digital_oscillator.cc ${_braids}/braids/resources.cc
                             ${_braids}/stmlib/utils/random.cc
               INCLUDES ${_braids}
               OPTIONS -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-extra -Wno-implicit-fallthrough
                       -Wno-missing-field-initializers -Wno-double-promotion -Wno-float-conversion -Wno-conversion
                       -Wno-shadow -Wno-unused-function -Wno-unused-but-set-variable)
# Tracker: libxmp's player for MOD / XM / S3M / IT (the "lite" core, patched: third_party/patches; its C library
# calls go to xmp_shim.h, force-included into libxmp's sources)
set(_xmp ${CMAKE_CURRENT_LIST_DIR}/third_party/libxmp)
set(_xmp_src
    ${_xmp}/src/virtual.c ${_xmp}/src/format.c ${_xmp}/src/period.c ${_xmp}/src/player.c ${_xmp}/src/read_event.c
    ${_xmp}/src/misc.c ${_xmp}/src/dataio.c ${_xmp}/src/lfo.c ${_xmp}/src/scan.c ${_xmp}/src/control.c
    ${_xmp}/src/filter.c ${_xmp}/src/effects.c ${_xmp}/src/flow.c ${_xmp}/src/mixer.c ${_xmp}/src/mix_all.c
    ${_xmp}/src/load_helpers.c ${_xmp}/src/load.c ${_xmp}/src/filetype.c ${_xmp}/src/hio.c ${_xmp}/src/smix.c
    ${_xmp}/src/memio.c ${_xmp}/src/rng.c
    ${_xmp}/src/loaders/common.c ${_xmp}/src/loaders/itsex.c ${_xmp}/src/loaders/sample.c
    ${_xmp}/src/loaders/xm_load.c ${_xmp}/src/loaders/mod_load.c ${_xmp}/src/loaders/s3m_load.c
    ${_xmp}/src/loaders/it_load.c)
set(_tracker ${CMAKE_CURRENT_LIST_DIR}/plugins/tracker)
set_source_files_properties(${_xmp_src} ${_tracker}/xmp_glue.c PROPERTIES
    COMPILE_OPTIONS "-include;${_tracker}/xmp_shim.h;-Wno-sign-compare;-Wno-unused-parameter;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-unused-function;-Wno-missing-field-initializers;-Wno-unused-variable;-Wno-extra")
daw_add_plugin(NAME tracker ENTRY daw_plugin_entry_tracker DIR ${_tracker}
               SOURCES tracker.cpp xmp_glue.c xmp_shim.c
               EXTRA_SOURCES ${_xmp_src}
               INCLUDES ${_xmp}/include ${_xmp}/src
               DEFINES LIBXMP_CORE_PLAYER LIBXMP_STATIC)
