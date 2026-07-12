add_executable(SingleCameraLog main.cpp)
target_link_libraries(SingleCameraLog PRIVATE IC4Ext::IC4Ext)
ic4ext_copy_dxc_runtime_to_target(SingleCameraLog)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET SingleCameraLog POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:SingleCameraLog>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:SingleCameraLog>/shaders"
        VERBATIM)
endif()

# D3D11 2.0.0 ReadOnly samples live as sibling directories. They are registered
# here so the top-level sample selection remains backward compatible.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../SingleCameraReadOnlyReadbackD3D11"
    "${CMAKE_CURRENT_BINARY_DIR}/../SingleCameraReadOnlyReadbackD3D11")
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../MultiCameraReadOnlySyncD3D11"
    "${CMAKE_CURRENT_BINARY_DIR}/../MultiCameraReadOnlySyncD3D11")
