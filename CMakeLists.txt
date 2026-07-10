cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" IC4EXT_VERSION)
string(STRIP "${IC4EXT_VERSION}" IC4EXT_VERSION)
if(NOT IC4EXT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Invalid IC4Ext VERSION value: '${IC4EXT_VERSION}'")
endif()

project(IC4Ext VERSION "${IC4EXT_VERSION}" LANGUAGES CXX)

include(FetchContent)

option(IC4EXT_BUILD_SAMPLES "Build IC4Ext samples" ON)
option(IC4EXT_BUILD_TESTS "Build IC4Ext tests" ON)

# Backend selection. These options also define public preprocessor macros:
#   IC4EXT_ENABLE_D3D11
#   IC4EXT_ENABLE_D3D12
# Users that include headers manually can define the same macros themselves.
option(IC4EXT_ENABLE_D3D11 "Build and expose the D3D11 backend" ON)
option(IC4EXT_ENABLE_D3D12 "Build and expose the D3D12 backend" ON)

if(NOT IC4EXT_ENABLE_D3D11 AND NOT IC4EXT_ENABLE_D3D12)
    message(FATAL_ERROR "At least one backend must be enabled: IC4EXT_ENABLE_D3D11 or IC4EXT_ENABLE_D3D12")
endif()

message(STATUS "IC4Ext version: ${PROJECT_VERSION}")
message(STATUS "IC4Ext backend selection: D3D11=${IC4EXT_ENABLE_D3D11}, D3D12=${IC4EXT_ENABLE_D3D12}")

# Dependency acquisition policy.
# IC4 SDK is not fetched: install IC Imaging Control 4 locally and pass IC4_SDK_ROOT.
option(IC4EXT_FETCH_DEPENDENCIES "Fetch open-source dependencies with CMake FetchContent" ON)
option(IC4EXT_FETCH_D3D11HELPER "Fetch D3D11Helper from GitHub when D3D11HELPER_ROOT is empty" ON)
option(IC4EXT_FETCH_D3D12HELPER "Fetch D3D12Helper from GitHub when D3D12HELPER_ROOT is empty" ON)
option(IC4EXT_FETCH_THREADKIT "Fetch ThreadKit from GitHub when THREADKIT_ROOT is empty" ON)
option(IC4EXT_FETCH_NLOHMANN_JSON "Fetch nlohmann/json from GitHub instead of find_package" ON)

set(IC4_SDK_ROOT "" CACHE PATH "Path to IC Imaging Control 4 SDK root. If empty, IC4Ext uses the IC4PATH environment variable when available.")
if(NOT IC4_SDK_ROOT AND DEFINED ENV{IC4PATH} AND NOT "$ENV{IC4PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{IC4PATH}" IC4EXT_IC4PATH_ROOT)
    set(IC4_SDK_ROOT "${IC4EXT_IC4PATH_ROOT}" CACHE PATH "Path to IC Imaging Control 4 SDK root" FORCE)
    message(STATUS "IC4Ext: using IC4_SDK_ROOT from IC4PATH=${IC4_SDK_ROOT}")
endif()
set(D3D11HELPER_ROOT "" CACHE PATH "Optional local path to D3D11Helper. Overrides FetchContent when set.")
set(D3D12HELPER_ROOT "" CACHE PATH "Optional local path to D3D12Helper. Overrides FetchContent when set.")
set(THREADKIT_ROOT "" CACHE PATH "Optional local path to ThreadKit. Overrides FetchContent when set.")

# Pin helper dependencies by tag. D3D11Helper/D3D12Helper CMake project versions may lag the tag name,
# so the Git ref is the authoritative version selector for IC4Ext.
set(IC4EXT_D3D11HELPER_GIT_TAG "v1.12.1" CACHE STRING "Git tag/branch/commit for D3D11Helper FetchContent")
set(IC4EXT_D3D12HELPER_GIT_TAG "v1.12.1" CACHE STRING "Git tag/branch/commit for D3D12Helper FetchContent")
set(IC4EXT_THREADKIT_GIT_TAG "main" CACHE STRING "Git tag/branch/commit for ThreadKit FetchContent")
set(IC4EXT_NLOHMANN_JSON_GIT_TAG "v3.11.3" CACHE STRING "Git tag/branch/commit for nlohmann/json FetchContent")

# -----------------------------------------------------------------------------
# DirectX Shader Compiler runtime discovery
# -----------------------------------------------------------------------------
# D3D11Helper/D3D12Helper can copy dxcompiler.dll/dxil.dll next to their
# samples/tests, but their subprojects search relative to their own source dir.
# When users run:
#   nuget install Microsoft.Direct3D.DXC -OutputDirectory packages
# from the IC4Ext root, the package is under IC4Ext/packages and would otherwise
# be missed. Resolve it once at the top-level and pass it down to both helpers.
set(IC4EXT_DXC_RUNTIME_DIR "" CACHE PATH "Directory that contains dxcompiler.dll and dxil.dll. Overrides auto-detection.")

function(ic4ext_find_dxc_runtime_dir out_var)
    if(IC4EXT_DXC_RUNTIME_DIR)
        set(${out_var} "${IC4EXT_DXC_RUNTIME_DIR}" PARENT_SCOPE)
        return()
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_dxc_arch "x64")
    else()
        set(_dxc_arch "x86")
    endif()

    set(_candidate_dirs)
    foreach(_base_dir IN LISTS CMAKE_CURRENT_SOURCE_DIR CMAKE_BINARY_DIR)
        file(GLOB _package_dirs
            "${_base_dir}/packages/Microsoft.Direct3D.DXC*/bin/${_dxc_arch}"
            "${_base_dir}/packages/microsoft.direct3d.dxc*/bin/${_dxc_arch}"
            "${_base_dir}/packages/Microsoft.Direct3D.DXC*/build/native/bin/${_dxc_arch}"
            "${_base_dir}/packages/microsoft.direct3d.dxc*/build/native/bin/${_dxc_arch}")
        list(APPEND _candidate_dirs ${_package_dirs})
    endforeach()

    if(DEFINED ENV{USERPROFILE})
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _user_profile)
        file(GLOB _global_package_dirs
            "${_user_profile}/.nuget/packages/microsoft.direct3d.dxc/*/bin/${_dxc_arch}"
            "${_user_profile}/.nuget/packages/microsoft.direct3d.dxc/*/build/native/bin/${_dxc_arch}")
        list(APPEND _candidate_dirs ${_global_package_dirs})
    endif()

    list(REMOVE_DUPLICATES _candidate_dirs)
    foreach(_dir IN LISTS _candidate_dirs)
        if(EXISTS "${_dir}/dxcompiler.dll")
            set(${out_var} "${_dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

ic4ext_find_dxc_runtime_dir(IC4EXT_RESOLVED_DXC_RUNTIME_DIR)
if(IC4EXT_RESOLVED_DXC_RUNTIME_DIR)
    message(STATUS "IC4Ext: found DXC runtime: ${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}")
    set(D3D11HELPER_DXC_RUNTIME_DIR "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}" CACHE PATH "Directory that contains dxcompiler.dll and dxil.dll" FORCE)
    set(D3D12HELPER_DXC_RUNTIME_DIR "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}" CACHE PATH "Directory that contains dxcompiler.dll and dxil.dll" FORCE)
else()
    message(WARNING "IC4Ext: DXC runtime was not found. If helper samples/tests need DXC, run 'nuget install Microsoft.Direct3D.DXC -OutputDirectory packages' in the IC4Ext root or pass -DIC4EXT_DXC_RUNTIME_DIR=path/to/bin/x64.")
endif()

# -----------------------------------------------------------------------------
# nlohmann/json
# -----------------------------------------------------------------------------
# Do not FetchContent the full nlohmann/json repository on Windows. The repo
# contains very long test/report file names that can exceed MAX_PATH when this
# project is placed under a nested Downloads path. IC4Ext only needs the single
# public header, so fetch that file directly and expose the normal
# nlohmann_json::nlohmann_json interface target.
set(IC4EXT_NLOHMANN_JSON_SINGLE_HEADER_URL
    "https://raw.githubusercontent.com/nlohmann/json/${IC4EXT_NLOHMANN_JSON_GIT_TAG}/single_include/nlohmann/json.hpp"
    CACHE STRING "URL used when fetching the nlohmann/json single header")
