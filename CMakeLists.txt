cmake_minimum_required(VERSION 3.22)
project(Orgelwerk VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_subdirectory(third_party/JUCE)
include(cmake/verovio.cmake)

# Plugin formats: standalone app everywhere, VST3 everywhere, AU on macOS, LV2 on Linux.
set(ORGEL_FORMATS Standalone VST3)
if(APPLE)
    list(APPEND ORGEL_FORMATS AU)
elseif(UNIX)
    list(APPEND ORGEL_FORMATS LV2)
endif()

juce_add_plugin(Orgelwerk
    COMPANY_NAME "Cloke"
    BUNDLE_ID "com.cloke.orgelwerk"
    PLUGIN_MANUFACTURER_CODE Clke
    PLUGIN_CODE Orgw
    PRODUCT_NAME "Orgelwerk"
    DESCRIPTION "Pfeifenorgel mit SF2-Klang und Notenpult"
    FORMATS ${ORGEL_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    LV2URI "https://github.com/cloke/orgelwerk"
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    MICROPHONE_PERMISSION_ENABLED FALSE
    DOCUMENT_EXTENSIONS mid midi musicxml mxl xml mei abc krn
    ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png)

file(GLOB_RECURSE ORGEL_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
target_sources(Orgelwerk PRIVATE ${ORGEL_SOURCES})
target_include_directories(Orgelwerk PRIVATE src third_party/TinySoundFont)

# Verovio resources (fonts) are packed into a zip and embedded, so the plugin works without an installer.
set(VRV_ZIP ${CMAKE_CURRENT_BINARY_DIR}/verovio-data.zip)
add_custom_command(OUTPUT ${VRV_ZIP}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${VRV_ZIP} --format=zip Bravura.xml Bravura Leipzig.xml Leipzig text
    WORKING_DIRECTORY ${VRV_DIR}/data
    DEPENDS ${VRV_DIR}/data/Bravura.xml ${VRV_DIR}/data/Leipzig.xml)
add_custom_target(verovio_data_zip DEPENDS ${VRV_ZIP})

juce_add_binary_data(OrgelData SOURCES
    resources/Jeux14.sf2
    ${VRV_ZIP})
add_dependencies(OrgelData verovio_data_zip)

target_compile_definitions(Orgelwerk PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE=0)

target_link_libraries(Orgelwerk
    PRIVATE
        OrgelData
        verovio_static
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(Orgelwerk PRIVATE /bigobj /utf-8)
endif()

# Command line tool for tests: import, engrave and export without the UI.
option(ORGEL_BUILD_CLI "Build the orgelcli test tool" ON)
if(ORGEL_BUILD_CLI)
    juce_add_console_app(orgelcli PRODUCT_NAME "orgelcli")
    file(GLOB ORGEL_CORE_SOURCES CONFIGURE_DEPENDS src/engine/*.cpp src/score/*.cpp)
    target_sources(orgelcli PRIVATE tools/cli.cpp ${ORGEL_CORE_SOURCES})
    target_include_directories(orgelcli PRIVATE src third_party/TinySoundFont)
    target_compile_definitions(orgelcli PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_USE_OGGVORBIS=1)
    target_link_libraries(orgelcli PRIVATE OrgelData verovio_static
        juce::juce_audio_formats juce::juce_gui_basics juce::juce_dsp juce::juce_recommended_config_flags)
endif()

# Small host that loads the built VST3 and checks it makes sound.
if(ORGEL_BUILD_CLI)
    juce_add_console_app(plugincheck PRODUCT_NAME "plugincheck")
    target_sources(plugincheck PRIVATE tools/plugincheck.cpp)
    target_compile_definitions(plugincheck PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(plugincheck PRIVATE juce::juce_audio_processors juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
