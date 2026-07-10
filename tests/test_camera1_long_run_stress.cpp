#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>

#if IC4EXT_ENABLE_D3D11
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#endif

#if IC4EXT_ENABLE_D3D12
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ShouldRunBackend(const char* backendName)
{
    const char* requestedEnv = IC4ExtTest::Env("IC4EXT_TEST_STRESS_BACKEND");
    if (!requestedEnv || std::string(requestedEnv).empty()) {
        return true;
    }
    const std::string requested = Lower(requestedEnv);
    return requested == "both" || requested == "all" || requested == Lower(backendName);
}

void PrintPerformance(const char* tag, const IC4Ext::CameraPerformanceSnapshot& perf)
{
    std::cout << tag << " performance: received=" << perf.captureStats.receivedBuffers
              << " read=" << perf.captureStats.readFrames
              << " droppedPending=" << perf.captureStats.droppedPendingBuffers;
    if (perf.streamStatistics.hasValue) {
        std::cout << " deviceDelivered=" << perf.streamStatistics.deviceDelivered
                  << " sinkDelivered=" << perf.streamStatistics.sinkDelivered
                  << " sinkUnderrun=" << perf.streamStatistics.sinkUnderrun
                  << " deviceTransmissionError=" << perf.streamStatistics.deviceTransmissionError;
    }
    if (perf.timing.hasHostInterval) {
        std::cout << " hostFps=" << perf.timing.hostReceiveFps;
    }
    if (perf.timing.hasDeviceInterval) {
        std::cout << " deviceFps=" << perf.timing.deviceFps;
    }
    std::cout << " temperatures=" << perf.temperatures.size() << "\n";
}

void ApplyStressOverrides(IC4Ext::CameraCaptureConfig& config)
{
    const double stressFps = IC4ExtTest::EnvDouble("IC4EXT_TEST_STRESS_FPS", 0.0);
    if (stressFps > 0.0) {
        config.streamRequest.fps = stressFps;
    }
}

#if IC4EXT_ENABLE_D3D11
int RunD3D11LongRun(int frameCount, int restartCycles, int readTimeoutMs)
{
    if (!ShouldRunBackend("d3d11")) {
        return 77;
    }

    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        core = D3D11CoreLib::D3D11Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D11Core creation failed; skipping D3D11 stress path: " << e.what() << "\n";
        return 77;
    }

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    auto runOnce = [&](int framesToRead, const char* phase) -> int {
        IC4Ext::CameraCaptureConfig config = IC4ExtTest::MakeCameraConfig("d3d11", 0);
        ApplyStressOverrides(config);

        IC4Ext::D3D11CameraCapture capture;
        if (!capture.open(selector, config, core.get())) {
            std::cerr << "D3D11 " << phase << " open failed; skipping: "
                      << capture.lastError().where << ": " << capture.lastError().message << "\n";
            return 77;
        }

        IC4Ext::CameraReadOptions readOptions;
        readOptions.mode = IC4Ext::ReadMode::NextFrame;
        readOptions.timeoutMs = static_cast<unsigned>(readTimeoutMs);

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < framesToRead; ++i) {
            auto result = capture.read(readOptions);
            if (!result) {
                std::cerr << "D3D11 " << phase << " read failed at frame " << i
                          << ": " << result.error.where << ": " << result.error.message << "\n";
                capture.close();
                return 1;
            }
            if (!result.frame.texture || !result.frame.ready.wait(static_cast<unsigned>(readTimeoutMs))) {
                std::cerr << "D3D11 " << phase << " texture/fence invalid at frame " << i << "\n";
                capture.close();
                return 1;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        const auto perf = capture.performance();
        PrintPerformance(phase, perf);
        assert(perf.captureStats.readFrames >= static_cast<std::uint64_t>(framesToRead));
        std::cout << "D3D11 " << phase << " read " << framesToRead << " frames in " << elapsed << " ms\n";
        capture.close();
        IC4ExtTest::SleepAfterCameraAccess();
        return 0;
    };

    int rc = runOnce(frameCount, "long-run");
    if (rc != 0) return rc;

    for (int i = 0; i < restartCycles; ++i) {
        rc = runOnce(3, "restart");
        if (rc != 0) return rc;
    }

    return 0;
}
#endif

#if IC4EXT_ENABLE_D3D12
int RunD3D12LongRun(int frameCount, int restartCycles, int readTimeoutMs)
{
    if (!ShouldRunBackend("d3d12")) {
        return 77;
    }

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D12Core creation failed; skipping D3D12 stress path: " << e.what() << "\n";
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping D3D12 stress path\n";
        return 77;
    }

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    auto runOnce = [&](int framesToRead, const char* phase) -> int {
        IC4Ext::CameraCaptureConfig config = IC4ExtTest::MakeCameraConfig("d3d12", 0);
        ApplyStressOverrides(config);

        IC4Ext::D3D12CameraCapture capture;
        if (!capture.open(selector, config, backend)) {
            std::cerr << "D3D12 " << phase << " open failed; skipping: "
                      << capture.lastError().where << ": " << capture.lastError().message << "\n";
            return 77;
        }

        IC4Ext::CameraReadOptions readOptions;
        readOptions.mode = IC4Ext::ReadMode::NextFrame;
        readOptions.timeoutMs = static_cast<unsigned>(readTimeoutMs);

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < framesToRead; ++i) {
            auto result = capture.read(readOptions);
            if (!result) {
                std::cerr << "D3D12 " << phase << " read failed at frame " << i
                          << ": " << result.error.where << ": " << result.error.message << "\n";
                capture.close();
                return 1;
            }
            if (!result.frame.texture || !result.frame.ready.wait(static_cast<unsigned>(readTimeoutMs))) {
                std::cerr << "D3D12 " << phase << " texture/fence invalid at frame " << i << "\n";
                capture.close();
                return 1;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        const auto perf = capture.performance();
        PrintPerformance(phase, perf);
        assert(perf.captureStats.readFrames >= static_cast<std::uint64_t>(framesToRead));
        std::cout << "D3D12 " << phase << " read " << framesToRead << " frames in " << elapsed << " ms\n";
        capture.close();
        IC4ExtTest::SleepAfterCameraAccess();
        return 0;
    };

    int rc = runOnce(frameCount, "long-run");
    if (rc != 0) return rc;

    for (int i = 0; i < restartCycles; ++i) {
        rc = runOnce(3, "restart");
        if (rc != 0) return rc;
    }

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

    const int frameCount = IC4ExtTest::EnvInt("IC4EXT_TEST_STRESS_FRAMES", 1000);
    const int restartCycles = IC4ExtTest::EnvInt("IC4EXT_TEST_RESTART_CYCLES", 3);
    const int readTimeoutMs = IC4ExtTest::EnvInt("IC4EXT_TEST_STRESS_READ_TIMEOUT_MS", 5000);

    bool ranAnyPath = false;

#if IC4EXT_ENABLE_D3D11
    const int d3d11 = RunD3D11LongRun(frameCount, restartCycles, readTimeoutMs);
    if (d3d11 == 1) return 1;
    if (d3d11 == 0) ranAnyPath = true;
#endif

#if IC4EXT_ENABLE_D3D12
    const int d3d12 = RunD3D12LongRun(frameCount, restartCycles, readTimeoutMs);
    if (d3d12 == 1) return 1;
    if (d3d12 == 0) ranAnyPath = true;
#endif

    if (!ranAnyPath) {
        std::cerr << "No enabled backend completed the long-run stress path; skipping\n";
        return 77;
    }

    std::cout << "test_camera1_long_run_stress passed frames=" << frameCount
              << " restartCycles=" << restartCycles << "\n";
    return 0;
}
