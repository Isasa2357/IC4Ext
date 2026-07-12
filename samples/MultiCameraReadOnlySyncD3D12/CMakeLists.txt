add_executable(MultiCameraReadOnlySyncD3D12 main.cpp)
target_link_libraries(MultiCameraReadOnlySyncD3D12 PRIVATE IC4Ext::IC4Ext d3d12 dxgi)
ic4ext_copy_dxc_runtime_to_target(MultiCameraReadOnlySyncD3D12)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET MultiCameraReadOnlySyncD3D12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:MultiCameraReadOnlySyncD3D12>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:MultiCameraReadOnlySyncD3D12>/shaders"
        VERBATIM)
endif()

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../MultiPipelineStressD3D12"
    "${CMAKE_CURRENT_BINARY_DIR}/MultiPipelineStressD3D12")
