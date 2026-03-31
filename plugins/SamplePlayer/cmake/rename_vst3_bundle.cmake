if(NOT DEFINED source_bundle OR NOT EXISTS "${source_bundle}")
    return()
endif()

if(NOT DEFINED install_dir OR NOT DEFINED old_name OR NOT DEFINED new_name)
    message(FATAL_ERROR "rename_vst3_bundle.cmake requires source_bundle, install_dir, old_name, and new_name")
endif()

get_filename_component(bundle_parent "${source_bundle}" DIRECTORY)
set(renamed_bundle "${bundle_parent}/${new_name}.vst3")

if(EXISTS "${renamed_bundle}" AND NOT source_bundle STREQUAL renamed_bundle)
    file(REMOVE_RECURSE "${renamed_bundle}")
endif()

if(NOT source_bundle STREQUAL renamed_bundle)
    file(RENAME "${source_bundle}" "${renamed_bundle}")
endif()

set(exec_dir "${renamed_bundle}/Contents/MacOS")
set(old_exec "${exec_dir}/${old_name}")
set(new_exec "${exec_dir}/${new_name}")

if(EXISTS "${old_exec}" AND NOT old_exec STREQUAL new_exec)
    file(RENAME "${old_exec}" "${new_exec}")
endif()

if(EXISTS "${new_exec}")
    execute_process(
        COMMAND /usr/bin/perl -0pi -e "s/${old_name}/${new_name}/g"
        "${new_exec}"
        RESULT_VARIABLE binary_patch_result
        OUTPUT_QUIET
        ERROR_QUIET)

    if(NOT binary_patch_result EQUAL 0)
        message(WARNING "Failed to patch renamed VST3 executable strings: ${new_exec}")
    endif()
endif()

set(plist_path "${renamed_bundle}/Contents/Info.plist")
if(EXISTS "${plist_path}")
    file(READ "${plist_path}" plist_content)
    string(REPLACE "${old_name}" "${new_name}" plist_content "${plist_content}")
    file(WRITE "${plist_path}" "${plist_content}")
endif()

set(moduleinfo_path "${renamed_bundle}/Contents/Resources/moduleinfo.json")
if(EXISTS "${moduleinfo_path}")
    file(READ "${moduleinfo_path}" moduleinfo_content)
    string(REPLACE "${old_name}" "${new_name}" moduleinfo_content "${moduleinfo_content}")
    file(WRITE "${moduleinfo_path}" "${moduleinfo_content}")
endif()

execute_process(
    COMMAND codesign --force --sign - "${renamed_bundle}"
    RESULT_VARIABLE codesign_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(NOT codesign_result EQUAL 0)
    message(WARNING "Failed to re-sign renamed VST3 bundle: ${renamed_bundle}")
endif()

file(REMOVE_RECURSE "${install_dir}/${old_name}.vst3")
file(REMOVE_RECURSE "${install_dir}/${new_name}.vst3")
file(COPY "${renamed_bundle}" DESTINATION "${install_dir}")