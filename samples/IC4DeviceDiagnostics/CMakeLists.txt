add_executable(IC4DeviceDiagnostics main.cpp)
target_link_libraries(IC4DeviceDiagnostics PRIVATE IC4Ext::IC4Ext)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET IC4DeviceDiagnostics POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:IC4DeviceDiagnostics>/ic4core.dll"
        VERBATIM)
endif()
