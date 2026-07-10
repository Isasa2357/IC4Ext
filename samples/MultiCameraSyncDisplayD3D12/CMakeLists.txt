add_executable(MultiCameraSyncDisplayD3D12 main.cpp)
target_link_libraries(MultiCameraSyncDisplayD3D12 PRIVATE IC4Ext::IC4Ext user32 gdi32)

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
endif()
