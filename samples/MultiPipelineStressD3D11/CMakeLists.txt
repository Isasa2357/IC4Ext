find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)

add_executable(MultiPipelineStressD3D11
    StressRunner.cpp
    StressSupport.cpp
    SobelProcessor.cpp
    PipelineWorkers.cpp)

target_link_libraries(MultiPipelineStressD3D11
    PRIVATE
        IC4Ext::IC4Ext
        ${OpenCV_LIBS}
        d3d11
        dxgi
        d3dcompiler)

target_include_directories(MultiPipelineStressD3D11
    PRIVATE
        ${OpenCV_INCLUDE_DIRS})

target_compile_features(MultiPipelineStressD3D11 PRIVATE cxx_std_17)
ic4ext_copy_dxc_runtime_to_target(MultiPipelineStressD3D11)

if(WIN32 AND IC4_SDK_ROOT)
    add_custom_command(TARGET MultiPipelineStressD3D11 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IC4_SDK_ROOT}/bin/x64/ic4core.dll"
            "$<TARGET_FILE_DIR:MultiPipelineStressD3D11>/ic4core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shaders"
            "$<TARGET_FILE_DIR:MultiPipelineStressD3D11>/shaders"
        VERBATIM)
endif()
