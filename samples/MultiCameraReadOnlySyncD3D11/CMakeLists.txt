add_executable(MultiCameraReadOnlySyncD3D11 main.cpp)

target_link_libraries(MultiCameraReadOnlySyncD3D11
    PRIVATE
        IC4Ext::IC4Ext
        d3d11
        dxgi
        d3dcompiler)

target_compile_features(MultiCameraReadOnlySyncD3D11 PRIVATE cxx_std_17)
ic4ext_copy_dxc_runtime_to_target(MultiCameraReadOnlySyncD3D11)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET MultiCameraReadOnlySyncD3D11 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:MultiCameraReadOnlySyncD3D11>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:MultiCameraReadOnlySyncD3D11>/shaders"
        VERBATIM)
endif()
