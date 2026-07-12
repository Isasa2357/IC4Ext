# IC4Ext DXC runtime resolver and deployment helpers.
#
# This module resolves dxcompiler.dll/dxil.dll from one of:
#   1. IC4EXT_DXC_RUNTIME_DIR
#   2. already-restored packages under source/build/global NuGet cache
#   3. Microsoft.Direct3D.DXC restored from NuGet during configure
#
# It also exposes ic4ext_copy_dxc_runtime_to_target(target), which copies both
# DLLs next to a built executable.

set(IC4EXT_DXC_RUNTIME_DIR "" CACHE PATH "Directory containing dxcompiler.dll and dxil.dll. Overrides discovery and NuGet restore.")
option(IC4EXT_FETCH_DXC_RUNTIME "Fetch Microsoft.Direct3D.DXC from NuGet when dxcompiler.dll/dxil.dll are not found" ON)
set(IC4EXT_DXC_NUGET_PACKAGE "Microsoft.Direct3D.DXC" CACHE STRING "NuGet package id used for the DirectX Shader Compiler runtime")
set(IC4EXT_DXC_NUGET_VERSION "" CACHE STRING "Optional Microsoft.Direct3D.DXC NuGet version. Empty means the NuGet feed resolves the latest package.")
set(IC4EXT_DXC_NUGET_ROOT "${CMAKE_BINARY_DIR}/_deps/dxc_nuget" CACHE PATH "Directory where IC4Ext restores Microsoft.Direct3D.DXC")

function(ic4ext_dxc_arch out_var)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(${out_var} "x64" PARENT_SCOPE)
    else()
        set(${out_var} "x86" PARENT_SCOPE)
    endif()
endfunction()

function(ic4ext_append_existing_dxc_dirs out_var)
    ic4ext_dxc_arch(_dxc_arch)
    set(_candidate_dirs)

    foreach(_base_dir IN LISTS ARGN)
        if(NOT _base_dir)
            continue()
        endif()
        file(TO_CMAKE_PATH "${_base_dir}" _base_dir_cmake)
        file(GLOB _package_dirs
            "${_base_dir_cmake}/packages/Microsoft.Direct3D.DXC*/bin/${_dxc_arch}"
            "${_base_dir_cmake}/packages/microsoft.direct3d.dxc*/bin/${_dxc_arch}"
            "${_base_dir_cmake}/packages/Microsoft.Direct3D.DXC*/build/native/bin/${_dxc_arch}"
            "${_base_dir_cmake}/packages/microsoft.direct3d.dxc*/build/native/bin/${_dxc_arch}"
            "${_base_dir_cmake}/Microsoft.Direct3D.DXC*/bin/${_dxc_arch}"
            "${_base_dir_cmake}/microsoft.direct3d.dxc*/bin/${_dxc_arch}"
            "${_base_dir_cmake}/Microsoft.Direct3D.DXC*/build/native/bin/${_dxc_arch}"
            "${_base_dir_cmake}/microsoft.direct3d.dxc*/build/native/bin/${_dxc_arch}"
            "${_base_dir_cmake}/bin/${_dxc_arch}"
            "${_base_dir_cmake}/build/native/bin/${_dxc_arch}")
        list(APPEND _candidate_dirs ${_package_dirs})
    endforeach()

    if(DEFINED ENV{USERPROFILE})
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _user_profile)
        file(GLOB _global_package_dirs
            "${_user_profile}/.nuget/packages/microsoft.direct3d.dxc/*/bin/${_dxc_arch}"
            "${_user_profile}/.nuget/packages/microsoft.direct3d.dxc/*/build/native/bin/${_dxc_arch}")
        list(APPEND _candidate_dirs ${_global_package_dirs})
    endif()

    if(_candidate_dirs)
        list(REMOVE_DUPLICATES _candidate_dirs)
    endif()

    set(_valid_dirs)
    foreach(_dir IN LISTS _candidate_dirs)
        if(EXISTS "${_dir}/dxcompiler.dll" AND EXISTS "${_dir}/dxil.dll")
            list(APPEND _valid_dirs "${_dir}")
        endif()
    endforeach()

    set(${out_var} ${_valid_dirs} PARENT_SCOPE)
endfunction()

function(ic4ext_restore_dxc_with_nuget out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT IC4EXT_FETCH_DXC_RUNTIME)
        return()
    endif()

    file(MAKE_DIRECTORY "${IC4EXT_DXC_NUGET_ROOT}")

    find_program(IC4EXT_NUGET_EXE NAMES nuget nuget.exe)
    if(IC4EXT_NUGET_EXE)
        set(_nuget_args
            install "${IC4EXT_DXC_NUGET_PACKAGE}"
            -OutputDirectory "${IC4EXT_DXC_NUGET_ROOT}"
            -NonInteractive
            -DirectDownload)
        if(IC4EXT_DXC_NUGET_VERSION)
            list(APPEND _nuget_args -Version "${IC4EXT_DXC_NUGET_VERSION}")
        endif()

        message(STATUS "IC4Ext: restoring ${IC4EXT_DXC_NUGET_PACKAGE} with NuGet CLI")
        execute_process(
            COMMAND "${IC4EXT_NUGET_EXE}" ${_nuget_args}
            RESULT_VARIABLE _nuget_result
            OUTPUT_VARIABLE _nuget_stdout
            ERROR_VARIABLE _nuget_stderr)
        if(NOT _nuget_result EQUAL 0)
            message(WARNING "IC4Ext: NuGet CLI restore failed (${_nuget_result}). Falling back to direct NuGet package download.\n${_nuget_stderr}")
        endif()
    endif()

    ic4ext_append_existing_dxc_dirs(_restored_dirs "${IC4EXT_DXC_NUGET_ROOT}")
    if(_restored_dirs)
        list(GET _restored_dirs 0 _dir)
        set(${out_var} "${_dir}" PARENT_SCOPE)
        return()
    endif()

    set(_nupkg "${IC4EXT_DXC_NUGET_ROOT}/${IC4EXT_DXC_NUGET_PACKAGE}.nupkg")
    if(IC4EXT_DXC_NUGET_VERSION)
        set(_url "https://www.nuget.org/api/v2/package/${IC4EXT_DXC_NUGET_PACKAGE}/${IC4EXT_DXC_NUGET_VERSION}")
    else()
        set(_url "https://www.nuget.org/api/v2/package/${IC4EXT_DXC_NUGET_PACKAGE}")
    endif()

    message(STATUS "IC4Ext: downloading ${IC4EXT_DXC_NUGET_PACKAGE} from NuGet")
    file(DOWNLOAD
        "${_url}"
        "${_nupkg}"
        STATUS _download_status
        SHOW_PROGRESS
        TLS_VERIFY ON)
    list(GET _download_status 0 _download_code)
    list(GET _download_status 1 _download_message)
    if(NOT _download_code EQUAL 0)
        message(WARNING "IC4Ext: failed to download ${IC4EXT_DXC_NUGET_PACKAGE}: ${_download_message}")
        return()
    endif()

    set(_extract_dir "${IC4EXT_DXC_NUGET_ROOT}/extracted")
    file(REMOVE_RECURSE "${_extract_dir}")
    file(MAKE_DIRECTORY "${_extract_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${_extract_dir}")

    ic4ext_append_existing_dxc_dirs(_downloaded_dirs "${_extract_dir}")
    if(_downloaded_dirs)
        list(GET _downloaded_dirs 0 _dir)
        set(${out_var} "${_dir}" PARENT_SCOPE)
    endif()
endfunction()

function(ic4ext_resolve_dxc_runtime out_var)
    if(IC4EXT_DXC_RUNTIME_DIR)
        if(EXISTS "${IC4EXT_DXC_RUNTIME_DIR}/dxcompiler.dll" AND EXISTS "${IC4EXT_DXC_RUNTIME_DIR}/dxil.dll")
            set(${out_var} "${IC4EXT_DXC_RUNTIME_DIR}" PARENT_SCOPE)
            return()
        endif()
        message(WARNING "IC4Ext_DXC_RUNTIME_DIR is set but does not contain both dxcompiler.dll and dxil.dll: ${IC4EXT_DXC_RUNTIME_DIR}")
    endif()

    ic4ext_append_existing_dxc_dirs(_existing_dirs
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${IC4EXT_DXC_NUGET_ROOT}")
    if(_existing_dirs)
        list(GET _existing_dirs 0 _dir)
        set(${out_var} "${_dir}" PARENT_SCOPE)
        return()
    endif()

    ic4ext_restore_dxc_with_nuget(_restored_dir)
    if(_restored_dir)
        set(${out_var} "${_restored_dir}" PARENT_SCOPE)
        return()
    endif()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(ic4ext_copy_dxc_runtime_to_target target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "ic4ext_copy_dxc_runtime_to_target called for non-target: ${target_name}")
    endif()

    if(NOT WIN32)
        return()
    endif()

    if(NOT IC4EXT_RESOLVED_DXC_RUNTIME_DIR)
        return()
    endif()

    if(NOT EXISTS "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}/dxcompiler.dll" OR
       NOT EXISTS "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}/dxil.dll")
        message(WARNING "IC4Ext: DXC runtime directory is incomplete, not copying runtime DLLs for ${target_name}: ${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}")
        return()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}/dxcompiler.dll"
            "$<TARGET_FILE_DIR:${target_name}>/dxcompiler.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}/dxil.dll"
            "$<TARGET_FILE_DIR:${target_name}>/dxil.dll"
        VERBATIM)
endfunction()
