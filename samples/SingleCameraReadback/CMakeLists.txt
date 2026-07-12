add_executable(SingleCameraReadback main.cpp)
target_link_libraries(SingleCameraReadback PRIVATE IC4Ext::IC4Ext)
ic4ext_copy_dxc_runtime_to_target(SingleCameraReadback)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET SingleCameraReadback POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:SingleCameraReadback>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:SingleCameraReadback>/shaders"
        VERBATIM)
endif()
