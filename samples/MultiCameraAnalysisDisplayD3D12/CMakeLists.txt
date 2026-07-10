find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)

add_executable(MultiCameraAnalysisDisplayD3D12 main_compat.cpp)
target_link_libraries(MultiCameraAnalysisDisplayD3D12
    PRIVATE
        IC4Ext::IC4Ext
        ${OpenCV_LIBS}
        d3d12
        dxgi)
target_include_directories(MultiCameraAnalysisDisplayD3D12 PRIVATE ${OpenCV_INCLUDE_DIRS})

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET MultiCameraAnalysisDisplayD3D12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:MultiCameraAnalysisDisplayD3D12>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:MultiCameraAnalysisDisplayD3D12>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:MultiCameraAnalysisDisplayD3D12>/config"
        VERBATIM)
endif()
