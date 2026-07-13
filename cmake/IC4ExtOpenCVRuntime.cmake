include_guard(GLOBAL)

# Copy the OpenCV runtime next to an executable target.
#
# OpenCV's Windows package separates import libraries (lib/) from runtime DLLs
# (bin/). Linking succeeds with OpenCV_DIR pointed at lib/, but launching the
# executable fails unless opencv_world*.dll and the videoio backend DLLs are on
# PATH or beside the executable. This helper discovers the runtime directories
# from OpenCVConfig.cmake variables and imported targets, then performs a
# POST_BUILD copy.
function(ic4ext_copy_opencv_runtime_to_target target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "ic4ext_copy_opencv_runtime_to_target: target '${target_name}' does not exist")
    endif()

    set(_ic4ext_opencv_runtime_dirs)

    if(DEFINED OpenCV_BIN_PATH AND
       NOT "${OpenCV_BIN_PATH}" STREQUAL "" AND
       IS_DIRECTORY "${OpenCV_BIN_PATH}")
        list(APPEND _ic4ext_opencv_runtime_dirs "${OpenCV_BIN_PATH}")
    endif()

    # The official Windows package normally uses:
    #   OpenCV_DIR = <root>/build/x64/vcXX/lib
    #   runtime    = <root>/build/x64/vcXX/bin
    if(DEFINED OpenCV_DIR AND NOT "${OpenCV_DIR}" STREQUAL "")
        get_filename_component(
            _ic4ext_opencv_runtime_from_config
            "${OpenCV_DIR}/../bin"
            ABSOLUTE)
        if(IS_DIRECTORY "${_ic4ext_opencv_runtime_from_config}")
            list(APPEND
                _ic4ext_opencv_runtime_dirs
                "${_ic4ext_opencv_runtime_from_config}")
        endif()
    endif()

    # Also inspect imported OpenCV targets. This supports custom OpenCV builds
    # whose runtime directory is not a sibling of OpenCV_DIR.
    foreach(_ic4ext_opencv_library IN LISTS OpenCV_LIBS)
        if(NOT TARGET "${_ic4ext_opencv_library}")
            continue()
        endif()

        foreach(_ic4ext_location_property IN ITEMS
                IMPORTED_LOCATION_DEBUG
                IMPORTED_LOCATION_RELEASE
                IMPORTED_LOCATION_RELWITHDEBINFO
                IMPORTED_LOCATION_MINSIZEREL
                IMPORTED_LOCATION)
            get_target_property(
                _ic4ext_opencv_location
                "${_ic4ext_opencv_library}"
                "${_ic4ext_location_property}")
            if(_ic4ext_opencv_location AND
               NOT _ic4ext_opencv_location MATCHES "-NOTFOUND$")
                get_filename_component(
                    _ic4ext_opencv_location_dir
                    "${_ic4ext_opencv_location}"
                    DIRECTORY)
                if(IS_DIRECTORY "${_ic4ext_opencv_location_dir}")
                    list(APPEND
                        _ic4ext_opencv_runtime_dirs
                        "${_ic4ext_opencv_location_dir}")
                endif()
            endif()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _ic4ext_opencv_runtime_dirs)

    set(_ic4ext_opencv_runtime_dlls)
    foreach(_ic4ext_opencv_runtime_dir IN LISTS _ic4ext_opencv_runtime_dirs)
        file(GLOB
            _ic4ext_opencv_runtime_dir_dlls
            CONFIGURE_DEPENDS
            "${_ic4ext_opencv_runtime_dir}/*.dll")
        list(APPEND
            _ic4ext_opencv_runtime_dlls
            ${_ic4ext_opencv_runtime_dir_dlls})
    endforeach()
    list(REMOVE_DUPLICATES _ic4ext_opencv_runtime_dlls)

    if(_ic4ext_opencv_runtime_dlls)
        add_custom_command(TARGET "${target_name}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${_ic4ext_opencv_runtime_dlls}
                "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND_EXPAND_LISTS
            VERBATIM)

        message(STATUS
            "${target_name}: OpenCV runtime DLLs will be copied from: "
            "${_ic4ext_opencv_runtime_dirs}")
    else()
        message(WARNING
            "${target_name}: OpenCV runtime DLLs were not found. "
            "Set OpenCV_DIR to the directory containing OpenCVConfig.cmake, "
            "or set OpenCV_BIN_PATH to the directory containing opencv_world*.dll.")
    endif()

    # Some OpenCV builds place dynamically loaded backends below bin/plugins.
    # Preserve that directory layout when it exists.
    foreach(_ic4ext_opencv_runtime_dir IN LISTS _ic4ext_opencv_runtime_dirs)
        if(IS_DIRECTORY "${_ic4ext_opencv_runtime_dir}/plugins")
            add_custom_command(TARGET "${target_name}" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_ic4ext_opencv_runtime_dir}/plugins"
                    "$<TARGET_FILE_DIR:${target_name}>/plugins"
                VERBATIM)
        endif()
    endforeach()
endfunction()
