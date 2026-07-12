function(ic4ext_add_no_camera_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE IC4Ext::IC4Ext)
    ic4ext_copy_dxc_runtime_to_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "no_camera"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

function(ic4ext_add_camera_test name label)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE IC4Ext::IC4Ext)
    ic4ext_copy_dxc_runtime_to_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "${label}"
        SKIP_RETURN_CODE 77
        RESOURCE_LOCK "ic4_camera"
        RUN_SERIAL TRUE
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

ic4ext_add_no_camera_test(test_core test_core.cpp)
ic4ext_add_no_camera_test(test_cpu_frame test_cpu_frame.cpp)
ic4ext_add_no_camera_test(test_backend_config test_backend_config.cpp)
ic4ext_add_no_camera_test(test_chunk_metadata test_chunk_metadata.cpp)
ic4ext_add_no_camera_test(test_no_camera_pipeline_stress test_no_camera_pipeline_stress.cpp)
set_tests_properties(test_no_camera_pipeline_stress PROPERTIES TIMEOUT 120)

if(IC4EXT_ENABLE_D3D11)
    ic4ext_add_no_camera_test(test_d3d11_frame_readback test_d3d11_frame_readback.cpp)
    set_tests_properties(test_d3d11_frame_readback PROPERTIES SKIP_RETURN_CODE 77)

    ic4ext_add_no_camera_test(test_d3d11_dummy_camera_capture test_d3d11_dummy_camera_capture.cpp)
    ic4ext_add_no_camera_test(test_d3d11_frame_sync_thread test_d3d11_frame_sync_thread.cpp)
endif()

if(IC4EXT_ENABLE_D3D11 OR IC4EXT_ENABLE_D3D12)
    ic4ext_add_camera_test(test_single_camera_smoke camera1 test_single_camera_smoke.cpp)

    ic4ext_add_camera_test(test_camera1_readback_integration camera1 test_camera1_readback_integration.cpp)
    set_tests_properties(test_camera1_readback_integration PROPERTIES TIMEOUT 240)

    ic4ext_add_camera_test(test_camera1_long_run_stress camera1 test_camera1_long_run_stress.cpp)
    set_tests_properties(test_camera1_long_run_stress PROPERTIES TIMEOUT 900)
endif()

if(IC4EXT_ENABLE_D3D12)
    ic4ext_add_no_camera_test(test_d3d12_core test_d3d12_core.cpp)

    ic4ext_add_no_camera_test(test_d3d12_shader_reference test_d3d12_shader_reference.cpp)
    target_compile_definitions(test_d3d12_shader_reference PRIVATE IC4EXT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

    ic4ext_add_no_camera_test(test_d3d12_dummy_camera_capture test_d3d12_dummy_camera_capture.cpp)

    ic4ext_add_no_camera_test(test_d3d12_frame_readback test_d3d12_frame_readback.cpp)
    set_tests_properties(test_d3d12_frame_readback PROPERTIES SKIP_RETURN_CODE 77)

    ic4ext_add_no_camera_test(test_d3d12_frame_sync_thread test_d3d12_frame_sync_thread.cpp)
    ic4ext_add_no_camera_test(test_v2_d3d12_foundation test_v2_d3d12_foundation.cpp)

    ic4ext_add_no_camera_test(test_d3d12_shader_compile test_d3d12_shader_compile.cpp)
    target_compile_definitions(test_d3d12_shader_compile PRIVATE IC4EXT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    if(WIN32)
        target_link_libraries(test_d3d12_shader_compile PRIVATE d3dcompiler)
    endif()
    set_tests_properties(test_d3d12_shader_compile PROPERTIES SKIP_RETURN_CODE 77)

    ic4ext_add_camera_test(test_camera2plus_frame_sync_smoke camera2plus test_camera2plus_frame_sync_smoke.cpp)
endif()
