#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>

#if IC4EXT_ENABLE_D3D11
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#endif

#if IC4EXT_ENABLE_D3D12
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#endif

#include <exception>
#include <iostream>
#include <memory>

namespace {

#if IC4EXT_ENABLE_D3D11
int RunD3D11Path()
{
    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        core = D3D11CoreLib::D3D11Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D11Core creation failed; skipping D3D11 camera path: " << e.what() << "\n";
        return 77;
    }

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config = IC4ExtTest::MakeCameraConfig("d3d11", 0);

    IC4Ext::D3D11CameraCapture capture;
    if (!capture.open(selector, config, core.get())) {
        std::cerr << "D3D11 camera path skipped/open failed: " << capture.lastError().where
                  << ": " << capture.lastError().message << "\n";
        return 77;
    }

    auto latest = capture.read(IC4Ext::ReadMode::LatestFrame);
    if (!latest) {
        std::cerr << "D3D11 LatestFrame read failed: " << latest.error.where << ": " << latest.error.message << "\n";
        return 1;
    }
    if (!latest.frame.texture || !latest.frame.ready.wait(3000)) {
        std::cerr << "D3D11 LatestFrame texture/fence invalid\n";
        return 1;
    }

    auto next = capture.read(IC4ExtTest::ReadOptions(3000));
    if (!next) {
        std::cerr << "D3D11 NextFrame read failed: " << next.error.where << ": " << next.error.message << "\n";
        return 1;
    }
    if (!next.frame.texture || !next.frame.ready.wait(3000)) {
        std::cerr << "D3D11 NextFrame texture/fence invalid\n";
        return 1;
    }

    capture.close();
    IC4ExtTest::SleepAfterCameraAccess();
    std::cout << "D3D11 single camera path passed\n";
    return 0;
}
#endif

#if IC4EXT_ENABLE_D3D12
int RunD3D12Path()
{
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D12Core creation failed; skipping D3D12 camera path: " << e.what() << "\n";
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping D3D12 camera path\n";
        return 77;
    }

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config = IC4ExtTest::MakeCameraConfig("d3d12", 0);

    IC4Ext::D3D12CameraCapture capture;
    if (!capture.open(selector, config, backend)) {
        std::cerr << "D3D12 camera path skipped/open failed: " << capture.lastError().where
                  << ": " << capture.lastError().message << "\n";
        return 77;
    }

    auto latest = capture.read(IC4Ext::ReadMode::LatestFrame);
    if (!latest) {
        std::cerr << "D3D12 LatestFrame read failed: " << latest.error.where << ": " << latest.error.message << "\n";
        return 1;
    }
    if (!latest.frame.texture || !latest.frame.ready.wait(3000)) {
        std::cerr << "D3D12 LatestFrame texture/fence invalid\n";
        return 1;
    }

    auto next = capture.read(IC4ExtTest::ReadOptions(3000));
    if (!next) {
        std::cerr << "D3D12 NextFrame read failed: " << next.error.where << ": " << next.error.message << "\n";
        return 1;
    }
    if (!next.frame.texture || !next.frame.ready.wait(3000)) {
        std::cerr << "D3D12 NextFrame texture/fence invalid\n";
        return 1;
    }

    capture.close();
    IC4ExtTest::SleepAfterCameraAccess();
    std::cout << "D3D12 single camera path passed\n";
    return 0;
}
#endif

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    if (!IC4ExtTest::RequireCameraCount(1)) {
        return 77;
    }

    bool ranAnyPath = false;

#if IC4EXT_ENABLE_D3D11
    const int d3d11 = RunD3D11Path();
    if (d3d11 == 1) return 1;
    if (d3d11 == 0) ranAnyPath = true;
#endif

#if IC4EXT_ENABLE_D3D12
    const int d3d12 = RunD3D12Path();
    if (d3d12 == 1) return 1;
    if (d3d12 == 0) ranAnyPath = true;
#endif

    if (!ranAnyPath) {
        std::cerr << "No enabled backend completed the single camera smoke path; skipping\n";
        return 77;
    }

    std::cout << "test_single_camera_smoke passed\n";
    return 0;
}
