# Sample Player plugin

if(WIN32)
    set(PLUGIN_FORMATS VST3 Standalone)
    set(AU_MAIN_TYPE "")
    set(LV2_URI "")
elseif(APPLE)
    set(PLUGIN_FORMATS VST3 AU Standalone)
    set(AU_MAIN_TYPE kAudioUnitType_MusicDevice)
    set(LV2_URI "")
elseif(UNIX)
    set(PLUGIN_FORMATS VST3 LV2 Standalone)
    set(AU_MAIN_TYPE "")
    set(LV2_URI "https://github.com/noizefield/audio-plugin-coder/SamplePlayer")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

juce_add_plugin(sample_player
    COMPANY_NAME "Noizefield"
    PLUGIN_MANUFACTURER_CODE NPSA
    PLUGIN_CODE SMP2
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "Sample Player"
    VST3_CATEGORIES Instrument
    AU_MAIN_TYPE ${AU_MAIN_TYPE}
    LV2URI ${LV2_URI}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
)

target_sources(sample_player
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
)

target_link_libraries(sample_player
    PRIVATE
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

target_compile_definitions(sample_player
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
)
