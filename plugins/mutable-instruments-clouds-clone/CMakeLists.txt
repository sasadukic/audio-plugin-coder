# Mutable Instruments Clouds Clone

if(WIN32)
    set(PLUGIN_FORMATS VST3 Standalone)
    set(NEEDS_WEBVIEW2 TRUE)
elseif(APPLE)
    set(PLUGIN_FORMATS VST3 AU Standalone)
    set(NEEDS_WEBVIEW2 FALSE)
    set(AU_MAIN_TYPE kAudioUnitType_Effect)
elseif(UNIX)
    set(PLUGIN_FORMATS VST3 LV2 Standalone)
    set(NEEDS_WEBVIEW2 FALSE)
    set(NEEDS_WEB_BROWSER TRUE)
    set(LV2_URI "https://github.com/noizefield/audio-plugin-coder/mutable-instruments-clouds-clone")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

juce_add_binary_data(mutable_instruments_clouds_clone_WebUI
    SOURCES
        Source/ui/public/index.html
        Source/ui/public/js/index.js
        Source/ui/public/js/juce/index.js
    NAMESPACE mutable_instruments_clouds_clone_BinaryData
)

juce_add_plugin(mutable_instruments_clouds_clone
    COMPANY_NAME "Noizefield"
    PLUGIN_MANUFACTURER_CODE NPS0
    PLUGIN_CODE MiCc
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "Clouds Clone"
    NEEDS_WEBVIEW2 ${NEEDS_WEBVIEW2}
    NEEDS_WEB_BROWSER ${NEEDS_WEB_BROWSER}
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE ${AU_MAIN_TYPE}
    LV2URI ${LV2_URI}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
)

target_sources(mutable_instruments_clouds_clone
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        ../CloudWash/Source/dsp/clouds/dsp/granular_processor.cc
        ../CloudWash/Source/dsp/clouds/resources.cc
        ../CloudWash/Source/dsp/clouds/dsp/pvoc/phase_vocoder.cc
        ../CloudWash/Source/dsp/clouds/dsp/pvoc/stft.cc
        ../CloudWash/Source/dsp/clouds/dsp/pvoc/frame_transformation.cc
        ../CloudWash/Source/dsp/clouds/dsp/mu_law.cc
        ../CloudWash/Source/dsp/clouds/dsp/correlator.cc
        ../CloudWash/Source/dsp/stmlib/dsp/atan.cc
        ../CloudWash/Source/dsp/stmlib/utils/random.cc
        ../CloudWash/Source/dsp/stmlib/dsp/units.cc
)

target_include_directories(mutable_instruments_clouds_clone
    PRIVATE
        ../CloudWash/Source/dsp
)

target_link_libraries(mutable_instruments_clouds_clone
    PRIVATE
        mutable_instruments_clouds_clone_WebUI
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

target_compile_definitions(mutable_instruments_clouds_clone
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
)

if(WIN32)
    target_compile_definitions(mutable_instruments_clouds_clone
        PUBLIC
            JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
            _USE_MATH_DEFINES=1
    )
endif()

apc_enable_windows_open_build_folder_after_build(mutable_instruments_clouds_clone ${PLUGIN_FORMATS})
