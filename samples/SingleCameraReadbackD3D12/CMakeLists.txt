add_executable(SingleCameraReadbackD3D12 main.cpp)
target_link_libraries(SingleCameraReadbackD3D12 PRIVATE IC4Ext::IC4Ext d3d12 dxgi)
ic4ext_copy_dxc_runtime_to_target(SingleCameraReadbackD3D12)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET SingleCameraReadbackD3D12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:SingleCameraReadbackD3D12>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:SingleCameraReadbackD3D12>/shaders"
        VERBATIM)
endif()
