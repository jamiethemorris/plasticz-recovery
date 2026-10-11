cmake_minimum_required(VERSION 3.22)
project(PlastiCZRecovery VERSION 0.3.2 LANGUAGES C CXX)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Native recovery target" FORCE)
endif()

option(PLASTICZ_BUILD_PLUGINS "Build the native AU and VST3 recovery preview" ON)
set(PLASTICZ_JUCE_PATH "" CACHE PATH "Path to a JUCE checkout (tested with 7.0.5)")
include(CTest)

add_library(plasticz_waveforms STATIC src/Waveforms.cpp)
target_include_directories(plasticz_waveforms PUBLIC src)
target_compile_features(plasticz_waveforms PUBLIC cxx_std_17)
# Preserve the individual SSE single-precision operations in the recovered code.
target_compile_options(plasticz_waveforms PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)

add_executable(check_waveforms tests/check_waveforms.cpp)
target_link_libraries(check_waveforms PRIVATE plasticz_waveforms)
target_compile_options(check_waveforms PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)

add_library(plasticz_engine STATIC src/SynthEngine.cpp src/Presets.cpp
    src/OutputProcessing.cpp src/Effects.cpp src/LegacyReverb.cpp src/Downsampler4x.cpp)
target_include_directories(plasticz_engine PUBLIC src)
target_compile_features(plasticz_engine PUBLIC cxx_std_17)
target_compile_options(plasticz_engine PRIVATE -ffp-contract=off -fno-fast-math)
target_link_libraries(plasticz_engine PUBLIC plasticz_waveforms)

add_executable(check_presets tests/check_presets.cpp)
target_link_libraries(check_presets PRIVATE plasticz_engine)
add_executable(check_voices tests/check_voices.cpp)
target_link_libraries(check_voices PRIVATE plasticz_engine)
target_compile_options(check_voices PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(check_engine_render tests/check_engine_render.cpp)
target_link_libraries(check_engine_render PRIVATE plasticz_engine)
target_compile_options(check_engine_render PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(check_output_processing tests/check_output_processing.cpp)
target_link_libraries(check_output_processing PRIVATE plasticz_engine)
target_compile_options(check_output_processing PRIVATE -ffp-contract=off -fno-fast-math)
add_library(check_legacy_reverb SHARED tests/check_legacy_reverb.cpp src/LegacyReverb.cpp)
target_include_directories(check_legacy_reverb PRIVATE src)
target_compile_features(check_legacy_reverb PRIVATE cxx_std_17)
target_compile_options(check_legacy_reverb PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(check_effects tests/check_effects.cpp)
target_link_libraries(check_effects PRIVATE plasticz_engine)
target_compile_options(check_effects PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(check_effects_safety tests/check_effects_safety.cpp)
target_link_libraries(check_effects_safety PRIVATE plasticz_engine)
target_compile_options(check_effects_safety PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(check_effects_sequence tests/check_effects_sequence.cpp)
target_link_libraries(check_effects_sequence PRIVATE plasticz_engine)
target_compile_options(check_effects_sequence PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(check_oversampling tests/check_oversampling.cpp)
target_link_libraries(check_oversampling PRIVATE plasticz_engine)
target_compile_options(check_oversampling PRIVATE -ffp-contract=off -fno-fast-math)
add_executable(render_presets tests/render_presets.cpp)
target_link_libraries(render_presets PRIVATE plasticz_engine)
target_compile_options(render_presets PRIVATE -ffp-contract=off -fno-fast-math)
set(PLASTICZ_PRESET_REFERENCE_DIR "${CMAKE_SOURCE_DIR}/analysis/recovered/preset-renders/reference"
    CACHE PATH "Original-instruction rendered factory preset WAV directory")
set(PLASTICZ_FULL_PRESET_REFERENCE_DIR "${CMAKE_SOURCE_DIR}/analysis/recovered/preset-renders-full/reference"
    CACHE PATH "Original-instruction full stereo factory preset WAV directory")
option(PLASTICZ_TEST_FULL_PRESETS "Include strict full stereo preset comparison in CTest" ON)
if(BUILD_TESTING)
    add_test(NAME waveforms COMMAND check_waveforms
        "${CMAKE_SOURCE_DIR}/analysis/recovered/cosine-table.f32"
        "${CMAKE_SOURCE_DIR}/analysis/recovered/waveform-oracle.csv")
    add_test(NAME presets COMMAND check_presets "${CMAKE_SOURCE_DIR}/analysis/recovered/factory-bank.rawchunk")
    add_test(NAME voices COMMAND check_voices "${CMAKE_SOURCE_DIR}/analysis/recovered/voice-oracle")
    add_test(NAME engine_render COMMAND check_engine_render)
    add_test(NAME output_processing COMMAND check_output_processing
        "${CMAKE_SOURCE_DIR}/analysis/recovered/output-processing")
    add_test(NAME effects COMMAND check_effects "${CMAKE_SOURCE_DIR}/analysis/recovered/effects/cases.bin")
    add_test(NAME effects_safety COMMAND check_effects_safety)
    add_test(NAME effects_sequence COMMAND check_effects_sequence
        "${CMAKE_SOURCE_DIR}/analysis/recovered/effects-sequence/sequence.bin")
    add_test(NAME oversampling COMMAND check_oversampling
        "${CMAKE_SOURCE_DIR}/analysis/recovered/oversampling")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME legacy_reverb COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/check_legacy_reverb_cached.py"
            --library "$<TARGET_FILE:check_legacy_reverb>"
            --reference "${CMAKE_SOURCE_DIR}/analysis/recovered/legacy-reverb")
    endif()
    if(EXISTS "${PLASTICZ_PRESET_REFERENCE_DIR}/preset-000.wav" AND EXISTS "${PLASTICZ_PRESET_REFERENCE_DIR}/preset-127.wav")
        add_test(NAME rendered_presets COMMAND render_presets
            --reference "${PLASTICZ_PRESET_REFERENCE_DIR}"
            --output "${CMAKE_BINARY_DIR}/preset-regression/native"
            --difference "${CMAKE_BINARY_DIR}/preset-regression/difference")
    endif()
    if(PLASTICZ_TEST_FULL_PRESETS)
        add_test(NAME rendered_presets_full COMMAND render_presets --full
            --reference "${PLASTICZ_FULL_PRESET_REFERENCE_DIR}"
            --output "${CMAKE_BINARY_DIR}/preset-regression-full/native"
            --difference "${CMAKE_BINARY_DIR}/preset-regression-full/difference")
    endif()
endif()

if(PLASTICZ_BUILD_PLUGINS)
    if(NOT EXISTS "${PLASTICZ_JUCE_PATH}/CMakeLists.txt")
        message(FATAL_ERROR "Set PLASTICZ_JUCE_PATH to an existing JUCE checkout")
    endif()
    add_subdirectory("${PLASTICZ_JUCE_PATH}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)

    file(GLOB PLASTICZ_CLASSIC_ASSETS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/classic/*.png")
    juce_add_binary_data(PlastiCZAssets NAMESPACE PlastiCZAssets SOURCES ${PLASTICZ_CLASSIC_ASSETS})

    juce_add_plugin(PlastiCZRecovery
        COMPANY_NAME "Recovery Project"
        BUNDLE_ID "org.local.plasticzrecovery"
        PLUGIN_MANUFACTURER_CODE Rcvr
        PLUGIN_CODE PzR1
        PRODUCT_NAME "PlastiCZ Recovery"
        DESCRIPTION "PlastiCZ native recovery preview"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Instrument Synth
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        FORMATS AU VST3)

    target_sources(PlastiCZRecovery PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp)
    target_compile_features(PlastiCZRecovery PRIVATE cxx_std_17)
    target_compile_options(PlastiCZRecovery PRIVATE -ffp-contract=off -fno-fast-math)
    target_compile_definitions(PlastiCZRecovery PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
    target_link_libraries(PlastiCZRecovery PRIVATE plasticz_engine PlastiCZAssets juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags)
    if(APPLE)
        # Seal the complete local bundle, not only the linker's Mach-O signature.
        foreach(format AU VST3)
            add_custom_command(TARGET PlastiCZRecovery_${format} POST_BUILD
                COMMAND /usr/bin/codesign --force --sign - --timestamp=none
                    "$<TARGET_BUNDLE_DIR:PlastiCZRecovery_${format}>"
                # Nested file changes do not update the outer bundle's date.
                COMMAND /usr/bin/touch "$<TARGET_BUNDLE_DIR:PlastiCZRecovery_${format}>"
                VERBATIM)
        endforeach()
    endif()

    juce_add_console_app(check_plugin_host PRODUCT_NAME "PlastiCZ local validation host")
    target_sources(check_plugin_host PRIVATE tests/check_plugin_host.cpp)
    target_compile_definitions(check_plugin_host PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(check_plugin_host PRIVATE juce::juce_audio_utils)

    add_executable(check_plugin_editor tests/check_plugin_editor.cpp)
    target_compile_features(check_plugin_editor PRIVATE cxx_std_17)
    target_link_libraries(check_plugin_editor PRIVATE PlastiCZRecovery)
    target_include_directories(check_plugin_editor PRIVATE src $<TARGET_PROPERTY:PlastiCZRecovery,INCLUDE_DIRECTORIES>)
    target_compile_definitions(check_plugin_editor PRIVATE $<TARGET_PROPERTY:PlastiCZRecovery,COMPILE_DEFINITIONS>)
endif()
