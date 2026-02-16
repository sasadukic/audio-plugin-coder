# SPECRAUM Plugin
# VST3 + CLAP

set(PLUGIN_FORMATS VST3 CLAP)
set(NEEDS_WEBVIEW2 FALSE)
set(NEEDS_WEB_BROWSER FALSE)

if(WIN32)
    set(NEEDS_WEBVIEW2 TRUE)
elseif(APPLE)
    set(NEEDS_WEB_BROWSER TRUE)
elseif(UNIX)
    set(NEEDS_WEB_BROWSER TRUE)
endif()

message(STATUS "SPECRAUM: Building for ${PLUGIN_FORMATS}")

juce_add_binary_data(specraum_WebUI
    SOURCES
        Source/ui/public/index.html
    NAMESPACE specraum_BinaryData
)

juce_add_plugin(specraum
    COMPANY_NAME "PsiloCybian"
    PLUGIN_MANUFACTURER_CODE NPS0
    PLUGIN_CODE SpcA
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "SPECRAUM"
    VST3_AUTO_MANIFEST FALSE
    NEEDS_WEBVIEW2 ${NEEDS_WEBVIEW2}
    NEEDS_WEB_BROWSER ${NEEDS_WEB_BROWSER}
    VST3_CATEGORIES Fx Analyzer
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
)

target_sources(specraum
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
)

target_link_libraries(specraum
    PRIVATE
        specraum_WebUI
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

target_compile_definitions(specraum
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
)

if(WIN32)
    target_compile_definitions(specraum
        PUBLIC
            JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
            _USE_MATH_DEFINES=1
    )
endif()

apc_enable_windows_open_build_folder_after_build(specraum ${PLUGIN_FORMATS})

juce_add_console_app(specraum_smooth_preset_builder)

target_sources(specraum_smooth_preset_builder
    PRIVATE
        Source/tools/SmoothPresetBuilder.cpp
)

target_link_libraries(specraum_smooth_preset_builder
    PRIVATE
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

target_compile_definitions(specraum_smooth_preset_builder
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)
