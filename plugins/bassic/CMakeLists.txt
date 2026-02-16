# BASSic-style synth plugin (separate from SPECRAUM)

set(PLUGIN_FORMATS VST3 CLAP)
if(APPLE)
    list(APPEND PLUGIN_FORMATS AU)
endif()

message(STATUS "BASSic: Building for ${PLUGIN_FORMATS}")

juce_add_plugin(bassic
    COMPANY_NAME "PsiloCybian"
    PLUGIN_MANUFACTURER_CODE NPS0
    PLUGIN_CODE P101
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "BASSic"
    VST3_CATEGORIES Instrument Synth
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
)

target_sources(bassic
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
)

target_link_libraries(bassic
    PRIVATE
        juce::juce_audio_basics
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

target_compile_definitions(bassic
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
)

apc_enable_windows_open_build_folder_after_build(bassic ${PLUGIN_FORMATS})
