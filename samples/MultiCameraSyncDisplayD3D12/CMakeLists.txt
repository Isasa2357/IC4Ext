option(IC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING
    "Enable H.264 recording in MultiCameraSyncDisplayD3D12"
    ON)

option(IC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE
    "Build the OpenCV synchronized multi-camera analysis/display/recording sample"
    OFF)

if(IC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING AND NOT TARGET D3DVideoEncoder::D3DVideoEncoder)
    include(FetchContent)
    set(D3DVIDEOENCODER_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(D3DVIDEOENCODER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(D3DVIDEOENCODER_FETCH_D3DHELPERS ON CACHE BOOL "" FORCE)
    set(D3DVIDEOENCODER_ENABLE_NVENC OFF CACHE BOOL "" FORCE)
    set(D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE ON CACHE BOOL "" FORCE)
    if(IC4EXT_RESOLVED_DXC_RUNTIME_DIR)
        set(D3DVIDEOENCODER_DXC_RUNTIME_DIR
            "${IC4EXT_RESOLVED_DXC_RUNTIME_DIR}"
            CACHE PATH "" FORCE)
    endif()
    FetchContent_Declare(
        D3DVideoEncoder
        GIT_REPOSITORY https://github.com/Isasa2357/D3DVideoEncoder.git
        GIT_TAG main
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(D3DVideoEncoder)
endif()

add_executable(MultiCameraSyncDisplayD3D12 main.cpp)
target_link_libraries(MultiCameraSyncDisplayD3D12 PRIVATE IC4Ext::IC4Ext user32 gdi32 d3d12 dxgi)

if(TARGET D3DVideoEncoder::D3DVideoEncoder)
    target_link_libraries(MultiCameraSyncDisplayD3D12 PRIVATE D3DVideoEncoder::D3DVideoEncoder)
    target_compile_definitions(MultiCameraSyncDisplayD3D12 PRIVATE IC4EXT_SAMPLE_HAS_VIDEO_ENCODER=1)
else()
    target_compile_definitions(MultiCameraSyncDisplayD3D12 PRIVATE IC4EXT_SAMPLE_HAS_VIDEO_ENCODER=0)
endif()

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET MultiCameraSyncDisplayD3D12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:MultiCameraSyncDisplayD3D12>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:MultiCameraSyncDisplayD3D12>/shaders"
        VERBATIM)

    if(IC4EXT_D3D12HELPER_SOURCE_DIR)
        add_custom_command(TARGET MultiCameraSyncDisplayD3D12 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${IC4EXT_D3D12HELPER_SOURCE_DIR}/shaders/D3D12Processing"
                "$<TARGET_FILE_DIR:MultiCameraSyncDisplayD3D12>/shaders/D3D12Processing"
            VERBATIM)
    endif()
endif()

if(IC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE)
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../MultiCameraAnalysisDisplayD3D12"
        "${CMAKE_CURRENT_BINARY_DIR}/MultiCameraAnalysisDisplayD3D12")
endif()
