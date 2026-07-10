add_executable(SingleCameraLogD3D12 main.cpp)
target_link_libraries(SingleCameraLogD3D12 PRIVATE IC4Ext::IC4Ext d3d12 dxgi)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET SingleCameraLogD3D12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:SingleCameraLogD3D12>/ic4core.dll"
        VERBATIM)
endif()

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../IC4DeviceDiagnostics"
    "${CMAKE_CURRENT_BINARY_DIR}/IC4DeviceDiagnostics")

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../MultiCameraSyncDisplayD3D12"
    "${CMAKE_CURRENT_BINARY_DIR}/MultiCameraSyncDisplayD3D12")
