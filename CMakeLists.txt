cmake_minimum_required(VERSION 3.22)
project(APC_System VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================
# OPTIONAL FEATURES
# ============================================
option(APC_ENABLE_VISAGE "Enable Visage UI framework support" OFF)
option(APC_AUTO_COPY_VST3_TO_SYSTEM_DIR "Auto-copy built VST3 bundles to C:/Program Files/Common Files/VST3 after successful builds (Windows only)" ON)
option(APC_OPEN_BUILD_FOLDER_AFTER_BUILD "Open the VST3 output folder in Explorer after successful VST3 builds (Windows only)" OFF)

function(apc_enable_windows_open_build_folder_after_build plugin_target)
    if(NOT WIN32)
        return()
    endif()

    set(copy_vst3_script "${CMAKE_SOURCE_DIR}/scripts/auto-copy-vst3.ps1")
    set(open_folder_script "${CMAKE_SOURCE_DIR}/scripts/open-folder-after-build.ps1")
    set(can_auto_copy_vst3 TRUE)
    set(can_open_folder TRUE)

    if(APC_AUTO_COPY_VST3_TO_SYSTEM_DIR AND NOT EXISTS "${copy_vst3_script}")
        set(can_auto_copy_vst3 FALSE)
        message(WARNING "VST3 auto-copy script not found: ${copy_vst3_script}")
    endif()

    if(APC_OPEN_BUILD_FOLDER_AFTER_BUILD AND NOT EXISTS "${open_folder_script}")
        set(can_open_folder FALSE)
        message(WARNING "Open-folder script not found: ${open_folder_script}")
    endif()

    foreach(format IN LISTS ARGN)
        if(NOT format STREQUAL "VST3")
            continue()
        endif()

        set(format_target "${plugin_target}_${format}")
        if(TARGET ${format_target})
            if(APC_AUTO_COPY_VST3_TO_SYSTEM_DIR AND can_auto_copy_vst3)
                add_custom_command(
                    TARGET ${format_target}
                    POST_BUILD
                    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${copy_vst3_script}" -BuiltBinaryPath "$<TARGET_FILE:${format_target}>" -DestinationVst3Dir "C:/Program Files/Common Files/VST3/$<TARGET_FILE_NAME:${format_target}>"
                    VERBATIM
                )
            endif()

            if(APC_OPEN_BUILD_FOLDER_AFTER_BUILD AND can_open_folder)
                add_custom_command(
                    TARGET ${format_target}
                    POST_BUILD
                    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${open_folder_script}" -FolderPath "$<TARGET_FILE_DIR:${format_target}>/../../.."
                    VERBATIM
                )
            endif()
        endif()
    endforeach()
endfunction()

# Backward compatible alias for the current helper name used by plugin CMake files.
function(apc_enable_windows_vst3_post_build_tasks plugin_target)
    apc_enable_windows_open_build_folder_after_build(${plugin_target} ${ARGN})
endfunction()

# ============================================
# PLATFORM DETECTION
# ============================================
if(WIN32)
    set(APC_PLATFORM "Windows")
    set(APC_PLATFORM_WINDOWS TRUE)
elseif(APPLE)
    set(APC_PLATFORM "macOS")
    set(APC_PLATFORM_MACOS TRUE)
elseif(UNIX)
    set(APC_PLATFORM "Linux")
    set(APC_PLATFORM_LINUX TRUE)
else()
    set(APC_PLATFORM "Unknown")
endif()

message(STATUS "--- APC BUILD SYSTEM ---")
message(STATUS "Platform: ${APC_PLATFORM}")
message(STATUS "Root Directory: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Auto-copy VST3 to system folder after build: ${APC_AUTO_COPY_VST3_TO_SYSTEM_DIR}")
message(STATUS "Open VST3 folder after build: ${APC_OPEN_BUILD_FOLDER_AFTER_BUILD}")

# ============================================
# GLOBAL DEFINITIONS (Platform-Specific)
# ============================================
if(APC_PLATFORM_WINDOWS)
    # Windows-specific: WebView2
    set(JUCE_USE_WIN_WEBVIEW2 ON CACHE BOOL "" FORCE)
    add_definitions(-DJUCE_USE_WIN_WEBVIEW2=1)
    message(STATUS "WebView2: Enabled")
elseif(APC_PLATFORM_MACOS)
    # macOS-specific: WKWebView (system framework)
    set(JUCE_USE_WIN_WEBVIEW2 OFF CACHE BOOL "" FORCE)
    message(STATUS "WebView: WKWebView (macOS)")
elseif(APC_PLATFORM_LINUX)
    # Linux-specific: WebKitGTK
    set(JUCE_USE_WIN_WEBVIEW2 OFF CACHE BOOL "" FORCE)
    message(STATUS "WebView: WebKitGTK (Linux)")
endif()

# ============================================
# EXTERNAL TOOLS
# ============================================
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_tools/JUCE")
set(VISAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_tools/visage")

if(NOT EXISTS "${JUCE_DIR}")
    message(FATAL_ERROR "JUCE missing at ${JUCE_DIR}")
endif()
add_subdirectory("${JUCE_DIR}")

# Visage is only needed for Visage UI framework plugins
if(APC_ENABLE_VISAGE)
    if(NOT EXISTS "${VISAGE_DIR}")
        message(FATAL_ERROR "Visage missing at ${VISAGE_DIR}")
    endif()
    add_subdirectory("${VISAGE_DIR}")
    if(TARGET visage AND NOT TARGET visage::visage)
        add_library(visage::visage ALIAS visage)
    endif()
endif()

# ============================================
# PLUGIN DISCOVERY
# ============================================
message(STATUS "Scanning for plugins in: ${CMAKE_CURRENT_SOURCE_DIR}/plugins")

file(GLOB PLUGIN_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*")

set(APC_PLUGIN_COUNT 0)

foreach(PLUGIN_DIR ${PLUGIN_DIRS})
    get_filename_component(DIR_NAME ${PLUGIN_DIR} NAME)
    
    if(IS_DIRECTORY "${PLUGIN_DIR}")
        if(EXISTS "${PLUGIN_DIR}/CMakeLists.txt")
            message(STATUS " -> FOUND: ${DIR_NAME}")
            add_subdirectory("${PLUGIN_DIR}")
            math(EXPR APC_PLUGIN_COUNT "${APC_PLUGIN_COUNT} + 1")
        else()
            message(STATUS " -> SKIPPING: ${DIR_NAME} (No CMakeLists.txt)")
        endif()
    endif()
endforeach()

message(STATUS "Total plugins found: ${APC_PLUGIN_COUNT}")
message(STATUS "--- END ---")
