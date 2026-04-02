# Sample Player plugin

if(WIN32)
    set(PLUGIN_FORMATS VST3 Standalone)
    set(NEEDS_WEBVIEW2 TRUE)
    set(NEEDS_WEB_BROWSER FALSE)
    set(AU_MAIN_TYPE "")
    set(LV2_URI "")
elseif(APPLE)
    set(PLUGIN_FORMATS VST3 AU Standalone)
    set(NEEDS_WEBVIEW2 FALSE)
    set(NEEDS_WEB_BROWSER TRUE)
    set(AU_MAIN_TYPE kAudioUnitType_MusicDevice)
    set(LV2_URI "")
elseif(UNIX)
    set(PLUGIN_FORMATS VST3 LV2 Standalone)
    set(NEEDS_WEBVIEW2 FALSE)
    set(NEEDS_WEB_BROWSER TRUE)
    set(AU_MAIN_TYPE "")
    set(LV2_URI "https://github.com/noizefield/audio-plugin-coder/SamplePlayer")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

juce_add_binary_data(sampleplayer_WebUI
    SOURCES
        Design/index.html
        Design/assets/handyman.svg
    NAMESPACE sampleplayer_BinaryData
)

juce_add_plugin(sample_player
    COMPANY_NAME "Noizefield"
    VERSION "1.0.1"
    PLUGIN_MANUFACTURER_CODE NPSA
    PLUGIN_CODE SMP2
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "Sample Player"
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3"
    AU_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components"
    VST3_CATEGORIES Instrument
    NEEDS_WEBVIEW2 ${NEEDS_WEBVIEW2}
    NEEDS_WEB_BROWSER ${NEEDS_WEB_BROWSER}
    AU_MAIN_TYPE ${AU_MAIN_TYPE}
    LV2URI ${LV2_URI}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
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
        sampleplayer_WebUI
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
        JUCE_WEB_BROWSER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
)

if(WIN32)
    target_compile_definitions(sample_player
        PUBLIC
            JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
            _USE_MATH_DEFINES=1
    )
endif()

if(APPLE AND TARGET sample_player_VST3)
    target_compile_options(sample_player_VST3
        PRIVATE
            -UJucePlugin_Name
            -DJucePlugin_Name=\"Ritual\ Engine\"
            -UJucePlugin_Desc
            -DJucePlugin_Desc=\"Ritual\ Engine\"
    )

    if(TARGET sample_player_vst3_helper)
        target_compile_options(sample_player_vst3_helper
            PRIVATE
                -UJucePlugin_Name
                -DJucePlugin_Name=\"Ritual\ Engine\"
                -UJucePlugin_Desc
                -DJucePlugin_Desc=\"Ritual\ Engine\"
        )
    endif()

    add_custom_command(TARGET sample_player_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -Dsource_bundle=$<TARGET_BUNDLE_DIR:sample_player_VST3>
            -Dinstall_dir=$ENV{HOME}/Library/Audio/Plug-Ins/VST3
            -Dold_name=Sample\ Player
            -Dnew_name=Ritual\ Engine
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/rename_vst3_bundle.cmake
        VERBATIM)
endif()
