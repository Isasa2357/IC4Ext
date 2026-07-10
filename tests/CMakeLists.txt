add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE IC4Ext::IC4Ext)
add_test(NAME test_core COMMAND test_core)

add_executable(test_cpu_frame test_cpu_frame.cpp)
target_link_libraries(test_cpu_frame PRIVATE IC4Ext::IC4Ext)
add_test(NAME test_cpu_frame COMMAND test_cpu_frame)

add_executable(test_backend_config test_backend_config.cpp)
target_link_libraries(test_backend_config PRIVATE IC4Ext::IC4Ext)
add_test(NAME test_backend_config COMMAND test_backend_config)

if(IC4EXT_ENABLE_D3D11)
    add_executable(test_d3d11_frame_readback test_d3d11_frame_readback.cpp)
    target_link_libraries(test_d3d11_frame_readback PRIVATE IC4Ext::IC4Ext)
    add_test(NAME test_d3d11_frame_readback COMMAND test_d3d11_frame_readback)
    set_tests_properties(test_d3d11_frame_readback PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_single_camera_smoke test_single_camera_smoke.cpp)
    target_link_libraries(test_single_camera_smoke PRIVATE IC4Ext::IC4Ext)
    add_test(NAME test_single_camera_smoke COMMAND test_single_camera_smoke)
    set_tests_properties(test_single_camera_smoke PROPERTIES LABELS "camera" SKIP_RETURN_CODE 77)
endif()

if(IC4EXT_ENABLE_D3D12)
    add_executable(test_d3d12_core test_d3d12_core.cpp)
    target_link_libraries(test_d3d12_core PRIVATE IC4Ext::IC4Ext)
    add_test(NAME test_d3d12_core COMMAND test_d3d12_core)

    add_executable(test_d3d12_shader_reference test_d3d12_shader_reference.cpp)
    target_link_libraries(test_d3d12_shader_reference PRIVATE IC4Ext::IC4Ext)
    target_compile_definitions(test_d3d12_shader_reference PRIVATE IC4EXT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME test_d3d12_shader_reference COMMAND test_d3d12_shader_reference)

    add_executable(test_d3d12_dummy_camera_capture test_d3d12_dummy_camera_capture.cpp)
    target_link_libraries(test_d3d12_dummy_camera_capture PRIVATE IC4Ext::IC4Ext)
    add_test(NAME test_d3d12_dummy_camera_capture COMMAND test_d3d12_dummy_camera_capture)


    add_executable(test_d3d12_frame_readback test_d3d12_frame_readback.cpp)
    target_link_libraries(test_d3d12_frame_readback PRIVATE IC4Ext::IC4Ext)
    add_test(NAME test_d3d12_frame_readback COMMAND test_d3d12_frame_readback)
    set_tests_properties(test_d3d12_frame_readback PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_d3d12_frame_sync_thread test_d3d12_frame_sync_thread.cpp)
    target_link_libraries(test_d3d12_frame_sync_thread PRIVATE IC4Ext::IC4Ext)
    add_test(NAME test_d3d12_frame_sync_thread COMMAND test_d3d12_frame_sync_thread)

    add_executable(test_d3d12_shader_compile test_d3d12_shader_compile.cpp)
    target_link_libraries(test_d3d12_shader_compile PRIVATE IC4Ext::IC4Ext)
    target_compile_definitions(test_d3d12_shader_compile PRIVATE IC4EXT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    if(WIN32)
        target_link_libraries(test_d3d12_shader_compile PRIVATE d3dcompiler)
    endif()
    add_test(NAME test_d3d12_shader_compile COMMAND test_d3d12_shader_compile)
    set_tests_properties(test_d3d12_shader_compile PROPERTIES SKIP_RETURN_CODE 77)
endif()
