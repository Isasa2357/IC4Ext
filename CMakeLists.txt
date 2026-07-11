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

# Pin helper dependencies by tag. Output-queue resize uses the v1.13 non-owning
# resource-view Processing APIs.
set(IC4EXT_D3D11HELPER_GIT_TAG "v1.13.0" CACHE STRING "Git tag/branch/commit for D3D11Helper FetchContent")
set(IC4EXT_D3D12HELPER_GIT_TAG "v1.13.0" CACHE STRING "Git tag/branch/commit for D3D12Helper FetchContent")
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

if(IC4EXT_FETCH_DEPENDENCIES AND IC4EXT_FETCH_NLOHMANN_JSON)
    set(IC4EXT_NLOHMANN_JSON_SINGLE_INCLUDE_DIR
        "${CMAKE_BINARY_DIR}/_deps/nlohmann_json_single/include")
    set(IC4EXT_NLOHMANN_JSON_SINGLE_HEADER
        "${IC4EXT_NLOHMANN_JSON_SINGLE_INCLUDE_DIR}/nlohmann/json.hpp")

    if(NOT EXISTS "${IC4EXT_NLOHMANN_JSON_SINGLE_HEADER}")
        message(STATUS "Fetching nlohmann/json single header: ${IC4EXT_NLOHMANN_JSON_GIT_TAG}")
        file(MAKE_DIRECTORY "${IC4EXT_NLOHMANN_JSON_SINGLE_INCLUDE_DIR}/nlohmann")
        file(DOWNLOAD
            "${IC4EXT_NLOHMANN_JSON_SINGLE_HEADER_URL}"
            "${IC4EXT_NLOHMANN_JSON_SINGLE_HEADER}"
            STATUS IC4EXT_NLOHMANN_JSON_DOWNLOAD_STATUS
            SHOW_PROGRESS
            TLS_VERIFY ON)
        list(GET IC4EXT_NLOHMANN_JSON_DOWNLOAD_STATUS 0 IC4EXT_NLOHMANN_JSON_DOWNLOAD_CODE)
        list(GET IC4EXT_NLOHMANN_JSON_DOWNLOAD_STATUS 1 IC4EXT_NLOHMANN_JSON_DOWNLOAD_MESSAGE)
        if(NOT IC4EXT_NLOHMANN_JSON_DOWNLOAD_CODE EQUAL 0)
            file(REMOVE "${IC4EXT_NLOHMANN_JSON_SINGLE_HEADER}")
            message(FATAL_ERROR
                "Failed to download nlohmann/json single header: "
                "${IC4EXT_NLOHMANN_JSON_DOWNLOAD_MESSAGE}. "
                "Either enable internet access, pass -DIC4EXT_FETCH_NLOHMANN_JSON=OFF "
                "and provide nlohmann_json via find_package, or set "
                "IC4EXT_NLOHMANN_JSON_SINGLE_HEADER_URL to a reachable json.hpp URL.")
        endif()
    endif()

    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE
        "${IC4EXT_NLOHMANN_JSON_SINGLE_INCLUDE_DIR}")
else()
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

# -----------------------------------------------------------------------------
# D3D11Helper, only when D3D11 backend is enabled
# -----------------------------------------------------------------------------
set(IC4EXT_D3D11HELPER_SOURCE_DIR "")
if(IC4EXT_ENABLE_D3D11)
    set(D3D11HELPER_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(D3D11HELPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(D3D11HELPER_ENABLE_PACKAGE_SMOKE_TESTS OFF CACHE BOOL "" FORCE)
    if(D3D11HELPER_ROOT)
        set(IC4EXT_D3D11HELPER_SOURCE_DIR "${D3D11HELPER_ROOT}")
        if(EXISTS "${D3D11HELPER_ROOT}/CMakeLists.txt")
            add_subdirectory("${D3D11HELPER_ROOT}" "${CMAKE_BINARY_DIR}/_deps/D3D11Helper-local-build" EXCLUDE_FROM_ALL)
        endif()
    elseif(IC4EXT_FETCH_DEPENDENCIES AND IC4EXT_FETCH_D3D11HELPER)
        FetchContent_Declare(
            D3D11Helper
            GIT_REPOSITORY https://github.com/Isasa2357/D3D11Helper.git
            GIT_TAG        ${IC4EXT_D3D11HELPER_GIT_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(D3D11Helper)
        set(IC4EXT_D3D11HELPER_SOURCE_DIR "${d3d11helper_SOURCE_DIR}")
    endif()
endif()

# -----------------------------------------------------------------------------
# D3D12Helper, only when D3D12 backend is enabled
# -----------------------------------------------------------------------------
set(IC4EXT_D3D12HELPER_SOURCE_DIR "")
if(IC4EXT_ENABLE_D3D12)
    set(D3D12HELPER_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(D3D12HELPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(D3D12HELPER_ENABLE_PACKAGE_SMOKE_TESTS OFF CACHE BOOL "" FORCE)
    set(D3D12HELPER_COPY_DXC_RUNTIME ON CACHE BOOL "" FORCE)
    if(D3D12HELPER_ROOT)
        set(IC4EXT_D3D12HELPER_SOURCE_DIR "${D3D12HELPER_ROOT}")
        if(EXISTS "${D3D12HELPER_ROOT}/CMakeLists.txt")
            add_subdirectory("${D3D12HELPER_ROOT}" "${CMAKE_BINARY_DIR}/_deps/D3D12Helper-local-build" EXCLUDE_FROM_ALL)
        endif()
    elseif(IC4EXT_FETCH_DEPENDENCIES AND IC4EXT_FETCH_D3D12HELPER)
        FetchContent_Declare(
            D3D12Helper
            GIT_REPOSITORY https://github.com/Isasa2357/D3D12Helper.git
            GIT_TAG        ${IC4EXT_D3D12HELPER_GIT_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(D3D12Helper)
        set(IC4EXT_D3D12HELPER_SOURCE_DIR "${d3d12helper_SOURCE_DIR}")
    endif()
endif()

# -----------------------------------------------------------------------------
# ThreadKit
# -----------------------------------------------------------------------------
set(IC4EXT_THREADKIT_SOURCE_DIR "")
if(THREADKIT_ROOT)
    set(IC4EXT_THREADKIT_SOURCE_DIR "${THREADKIT_ROOT}")
    if(EXISTS "${THREADKIT_ROOT}/CMakeLists.txt")
        set(THREADKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(THREADKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${THREADKIT_ROOT}" "${CMAKE_BINARY_DIR}/_deps/ThreadKit-local-build" EXCLUDE_FROM_ALL)
    endif()
elseif(IC4EXT_FETCH_DEPENDENCIES AND IC4EXT_FETCH_THREADKIT)
    set(THREADKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(THREADKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        ThreadKit
        GIT_REPOSITORY https://github.com/Isasa2357/ThreadKit.git
        GIT_TAG        ${IC4EXT_THREADKIT_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(ThreadKit)
    set(IC4EXT_THREADKIT_SOURCE_DIR "${threadkit_SOURCE_DIR}")
endif()

set(IC4EXT_SOURCES
    src/Core/Config.cpp
    src/Core/CpuFrame.cpp
)

if(IC4EXT_ENABLE_D3D11)
    list(APPEND IC4EXT_SOURCES
        src/D3D11/D3D11ReadyToken.cpp
        src/D3D11/D3D11FenceManager.cpp
        src/D3D11/D3D11FrameConverter.cpp
        src/D3D11/D3D11FrameCopier.cpp
        src/D3D11/D3D11FrameResizer.cpp
        src/D3D11/D3D11FrameReadback.cpp
        src/D3D11/D3D11DummyCameraCapture.cpp
        src/D3D11/D3D11DummyCameraCaptureGenerator.cpp
        src/D3D11/D3D11CameraCapture.cpp
        src/D3D11/D3D11CameraCaptureThread.cpp
        src/D3D11/D3D11FrameSyncThread.cpp
    )
endif()

if(IC4EXT_ENABLE_D3D12)
    list(APPEND IC4EXT_SOURCES
        src/D3D12/D3D12ReadyToken.cpp
        src/D3D12/D3D12FenceManager.cpp
        src/D3D12/D3D12FrameConverter.cpp
        src/D3D12/D3D12FrameCopier.cpp
        src/D3D12/D3D12FrameResizer.cpp
        src/D3D12/D3D12FrameReadback.cpp
        src/D3D12/D3D12DummyCameraCapture.cpp
        src/D3D12/D3D12DummyCameraCaptureGenerator.cpp
        src/D3D12/D3D12CameraCapture.cpp
        src/D3D12/D3D12CameraCaptureThread.cpp
        src/D3D12/D3D12FrameSyncThread.cpp
    )
endif()

add_library(IC4Ext STATIC ${IC4EXT_SOURCES})
add_library(IC4Ext::IC4Ext ALIAS IC4Ext)

target_compile_features(IC4Ext PUBLIC cxx_std_17)
target_include_directories(IC4Ext PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(IC4Ext PUBLIC
    IC4EXT_ENABLE_D3D11=$<IF:$<BOOL:${IC4EXT_ENABLE_D3D11}>,1,0>
    IC4EXT_ENABLE_D3D12=$<IF:$<BOOL:${IC4EXT_ENABLE_D3D12}>,1,0>
)

target_link_libraries(IC4Ext PUBLIC nlohmann_json::nlohmann_json)

if(NOT WIN32)
    message(WARNING "IC4Ext targets Windows/D3D backends. Non-Windows configure is for source inspection only.")
endif()

if(IC4_SDK_ROOT)
    target_include_directories(IC4Ext PUBLIC "${IC4_SDK_ROOT}/include")
    target_link_directories(IC4Ext PUBLIC "${IC4_SDK_ROOT}/lib/x64")
else()
    message(WARNING "IC4_SDK_ROOT is empty and IC4PATH was not available. Configure with -DIC4_SDK_ROOT=%IC4PATH% or set the IC4PATH environment variable to build against IC4 SDK.")
endif()

if(IC4EXT_ENABLE_D3D11)
    if(IC4EXT_D3D11HELPER_SOURCE_DIR)
        target_include_directories(IC4Ext PUBLIC "${IC4EXT_D3D11HELPER_SOURCE_DIR}/include")
    else()
        message(WARNING "D3D11 backend is enabled, but D3D11Helper source directory is empty. Set D3D11HELPER_ROOT or enable IC4EXT_FETCH_D3D11HELPER.")
    endif()
endif()

if(IC4EXT_ENABLE_D3D12)
    if(IC4EXT_D3D12HELPER_SOURCE_DIR)
        target_include_directories(IC4Ext PUBLIC "${IC4EXT_D3D12HELPER_SOURCE_DIR}/include")
    else()
        message(WARNING "D3D12 backend is enabled, but D3D12Helper source directory is empty. Set D3D12HELPER_ROOT or enable IC4EXT_FETCH_D3D12HELPER.")
    endif()
endif()

if(IC4EXT_THREADKIT_SOURCE_DIR)
    target_include_directories(IC4Ext PUBLIC "${IC4EXT_THREADKIT_SOURCE_DIR}/include")
else()
    message(WARNING "ThreadKit source directory is empty. Set THREADKIT_ROOT or enable IC4EXT_FETCH_THREADKIT.")
endif()

# Link helper targets if the fetched/local projects define them. Include dirs above are
# still added so header-only or differently named helper CMake projects remain usable.
if(IC4EXT_ENABLE_D3D11)
    if(TARGET D3D11Helper::D3D11Helper)
        target_link_libraries(IC4Ext PUBLIC D3D11Helper::D3D11Helper)
    elseif(TARGET D3D11Helper)
        target_link_libraries(IC4Ext PUBLIC D3D11Helper)
    elseif(TARGET D3D11CoreLib)
        target_link_libraries(IC4Ext PUBLIC D3D11CoreLib)
    elseif(TARGET D3D11CoreLib::D3D11CoreLib)
        target_link_libraries(IC4Ext PUBLIC D3D11CoreLib::D3D11CoreLib)
    endif()
endif()

if(IC4EXT_ENABLE_D3D12)
    if(TARGET D3D12Helper::D3D12Helper)
        target_link_libraries(IC4Ext PUBLIC D3D12Helper::D3D12Helper)
    elseif(TARGET D3D12Helper)
        target_link_libraries(IC4Ext PUBLIC D3D12Helper)
    elseif(TARGET D3D12CoreLib)
        target_link_libraries(IC4Ext PUBLIC D3D12CoreLib)
    elseif(TARGET D3D12CoreLib::D3D12CoreLib)
        target_link_libraries(IC4Ext PUBLIC D3D12CoreLib::D3D12CoreLib)
    endif()
endif()

if(TARGET ThreadKit::ThreadKit)
    target_link_libraries(IC4Ext PUBLIC ThreadKit::ThreadKit)
elseif(TARGET ThreadKit)
    target_link_libraries(IC4Ext PUBLIC ThreadKit)
endif()

if(WIN32)
    target_link_libraries(IC4Ext PUBLIC ic4core dxgi d3dcompiler)
    if(IC4EXT_ENABLE_D3D11)
        target_link_libraries(IC4Ext PUBLIC d3d11)
    endif()
    if(IC4EXT_ENABLE_D3D12)
        target_link_libraries(IC4Ext PUBLIC d3d12)
    endif()
    target_compile_definitions(IC4Ext PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    target_compile_options(IC4Ext PRIVATE /W4 /permissive-)
else()
    target_compile_options(IC4Ext PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(IC4EXT_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/d3d11" CACHE PATH "IC4Ext D3D11 shader source directory")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/shaders" DESTINATION "${CMAKE_BINARY_DIR}")

# The per-output resizers use helper Processing shaders at runtime.
if(IC4EXT_D3D11HELPER_SOURCE_DIR AND EXISTS "${IC4EXT_D3D11HELPER_SOURCE_DIR}/shaders/D3D11Processing")
    file(COPY "${IC4EXT_D3D11HELPER_SOURCE_DIR}/shaders/D3D11Processing"
         DESTINATION "${CMAKE_BINARY_DIR}/shaders")
endif()
if(IC4EXT_D3D12HELPER_SOURCE_DIR AND EXISTS "${IC4EXT_D3D12HELPER_SOURCE_DIR}/shaders/D3D12Processing")
    file(COPY "${IC4EXT_D3D12HELPER_SOURCE_DIR}/shaders/D3D12Processing"
         DESTINATION "${CMAKE_BINARY_DIR}/shaders")
endif()

if(IC4EXT_BUILD_SAMPLES)
    if(IC4EXT_ENABLE_D3D11)
        add_subdirectory(samples/SingleCameraLog)
        add_subdirectory(samples/SingleCameraReadback)
    endif()
    if(IC4EXT_ENABLE_D3D12)
        add_subdirectory(samples/SingleCameraLogD3D12)
        add_subdirectory(samples/SingleCameraReadbackD3D12)
    endif()
endif()

if(IC4EXT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
