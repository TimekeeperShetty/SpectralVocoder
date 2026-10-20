cmake_minimum_required(VERSION 3.22)
project(SpectralVocoder VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)

# JUCE lives in ./JUCE, a git submodule pinned to 8.0.14
add_subdirectory(JUCE)

# Copying into the system plug-in folders needs admin rights on Windows
# (C:\Program Files\Common Files\VST3), so it is off there by default.
if(WIN32)
    set(SV_COPY_PLUGINS_DEFAULT OFF)
else()
    set(SV_COPY_PLUGINS_DEFAULT ON)
endif()
option(SV_COPY_PLUGINS "Copy the built plugins into the system plug-in folders" ${SV_COPY_PLUGINS_DEFAULT})

juce_add_plugin(SpectralVocoder
    COMPANY_NAME "AbhijeetShetty"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SV_COPY_PLUGINS}
    PLUGIN_MANUFACTURER_CODE Yrco
    PLUGIN_CODE Svc1
    FORMATS AU VST3 Standalone           # AU is built on macOS only
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Audio input is required for the vocoder."
    PRODUCT_NAME "Spectral Vocoder")

target_sources(SpectralVocoder PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/VocoderEngine.cpp)

target_compile_definitions(SpectralVocoder PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SpectralVocoder
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Offline engine tests (no DAW needed). The presets in CMakePresets.json turn
# these on; run them with: ctest --preset <preset>
option(SV_BUILD_TESTS "Build the offline VocoderEngine tests" OFF)

if(SV_BUILD_TESTS)
    enable_testing()
    add_test(NAME EngineTests COMMAND EngineTests)

    juce_add_console_app(EngineTests PRODUCT_NAME "EngineTests")
    target_sources(EngineTests PRIVATE
        tests/EngineTests.cpp
        source/VocoderEngine.cpp)
    target_include_directories(EngineTests PRIVATE source)
    target_compile_definitions(EngineTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(EngineTests
        PRIVATE
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

