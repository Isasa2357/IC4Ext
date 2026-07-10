#include <IC4Ext/IC4Ext.hpp>
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <iostream>
#include <filesystem>
#include <cstdlib>

int main()
{
    auto core = D3D11CoreLib::D3D11Core::CreateShared();

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config;
    if (const char* jsonPath = std::getenv("IC4EXT_TEST_IC4_JSON")) {
        config.ic4StateJson.path = jsonPath;
    } else {
        config.streamRequest.requestedFormat = IC4Ext::CameraPixelFormat::BGR8;
        config.streamRequest.forceRequestedFormat = true;
    }
    if (const char* ox = std::getenv("IC4EXT_TEST_OFFSET_X")) config.streamRequest.offsetX = std::atoi(ox);
    if (const char* oy = std::getenv("IC4EXT_TEST_OFFSET_Y")) config.streamRequest.offsetY = std::atoi(oy);
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.shaderConfig.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d11";

    IC4Ext::D3D11CameraCapture capture;
    if (!capture.open(selector, config, core.get())) {
        std::cerr << "Camera smoke test skipped/open failed: " << capture.lastError().where
                  << ": " << capture.lastError().message << "\n";
        return 77; // common skip-like code; CTest can still show message
    }

    auto latest = capture.read(IC4Ext::ReadMode::LatestFrame);
    if (!latest) {
        std::cerr << "LatestFrame read failed: " << latest.error.where << ": " << latest.error.message << "\n";
        return 1;
    }
    if (!latest.frame.texture || !latest.frame.ready.wait(1000)) {
        std::cerr << "LatestFrame texture/fence invalid\n";
        return 1;
    }

    auto next = capture.read(IC4Ext::ReadMode::NextFrame);
    if (!next) {
        std::cerr << "NextFrame read failed: " << next.error.where << ": " << next.error.message << "\n";
        return 1;
    }
    if (!next.frame.texture || !next.frame.ready.wait(1000)) {
        std::cerr << "NextFrame texture/fence invalid\n";
        return 1;
    }

    capture.close();
    std::cout << "test_single_camera_smoke passed\n";
    return 0;
}
