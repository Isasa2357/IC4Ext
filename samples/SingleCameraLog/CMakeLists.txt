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
