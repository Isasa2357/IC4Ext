#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>

#if IC4EXT_ENABLE_D3D11
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#endif

#if IC4EXT_ENABLE_D3D12
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#endif

#include <algorithm>
#include <cctype>
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
    const char* requestedEnv = IC4ExtTest::Env("IC4EXT_TEST_READBACK_BACKEND");
    if (!requestedEnv || std::string(requestedEnv).empty()) {
        return true;
    }
    const std::string requested = Lower(requestedEnv);
    return requested == "both" || requested == "all" || requested == Lower(backendName);
}

void ValidateCpuFrame(const IC4Ext::CpuFrame& cpu)
{
    if (cpu.empty()) {
        std::cerr << "Readback returned an empty CpuFrame\n";
        assert(false);
    }
    assert(cpu.width > 0);
    assert(cpu.height > 0);
    assert(cpu.format == IC4Ext::CpuFrameFormat::BGR8);
    assert(cpu.rowPitch == cpu.width * 3u);
    assert(cpu.data.size() == static_cast<std::size_t>(cpu.rowPitch) * cpu.height);
}

void ValidateCacheStats(const char* tag, const IC4Ext::FrameReadbackCacheStats& stats, int frameCount)
{
    std::cout << tag << " readback cache: readbacks=" << stats.readbacks
              << " hits=" << stats.cacheHits
              << " misses=" << stats.cacheMisses
              << " rebuilds=" << stats.resourceRebuilds
              << " bytes=" << stats.bytesAllocated << "\n";
    assert(stats.readbacks == static_cast<std::uint64_t>(frameCount));
    assert(stats.cacheMisses == 1);
    assert(stats.resourceRebuilds == 1);
    if (frameCount > 1) {
        assert(stats.cacheHits >= static_cast<std::uint64_t>(frameCount - 1));
    }
    assert(stats.bytesAllocated > 0);
}

void PrintPerformance(const char* tag, const IC4Ext::CameraPerformanceSnapshot& perf)
{
    std::cout << tag << " performance: received=" << perf.captureStats.receivedBuffers
              << " read=" << perf.captureStats.readFrames;
    if (perf.streamStatistics.hasValue) {
        std::cout << " sinkDelivered=" << perf.streamStatistics.sinkDelivered
                  << " sinkUnderrun=" << perf.streamStatistics.sinkUnderrun;
    }
    if (perf.timing.hasHostInterval) {
        std::cout << " hostFps=" << perf.timing.hostReceiveFps;
    }
    std::cout << " temperatures=" << perf.temperatures.size() << "\n";
}

#if IC4EXT_ENABLE_D3D11
int RunD3D11Readback(int frameCount, int readTimeoutMs)
{
    if (!ShouldRunBackend("d3d11")) {
        return 77;
    }

    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        core = D3D11CoreLib::D3D11Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D11Core creation failed; skipping D3D11 readback integration: " << e.what() << "\n";
        return 77;
    }

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config = IC4ExtTest::MakeCameraConfig("d3d11", 0);
    IC4Ext::D3D11CameraCapture capture;
    if (!capture.open(selector, config, core.get())) {
        std::cerr << "D3D11 readback integration open failed; skipping: "
                  << capture.lastError().where << ": " << capture.lastError().message << "\n";
        return 77;
    }

    IC4Ext::D3D11FrameReadback readback;
    if (!readback.initialize(core.get())) {
        std::cerr << "D3D11 readback initialize failed: " << readback.lastError().where
                  << ": " << readback.lastError().message << "\n";
        capture.close();
        return 1;
    }

    IC4Ext::CameraReadOptions readOptions;
    readOptions.mode = IC4Ext::ReadMode::NextFrame;
    readOptions.timeoutMs = static_cast<unsigned>(readTimeoutMs);

    for (int i = 0; i < frameCount; ++i) {
        auto result = capture.read(readOptions);
        if (!result) {
            std::cerr << "D3D11 readback integration read failed at frame " << i
                      << ": " << result.error.where << ": " << result.error.message << "\n";
            capture.close();
            return 1;
        }
        IC4Ext::CpuFrame cpu;
        if (!readback.readback(result.frame, IC4Ext::CpuFrameFormat::BGR8, cpu, static_cast<unsigned>(readTimeoutMs))) {
            std::cerr << "D3D11 readback failed at frame " << i << ": "
                      << readback.lastError().where << ": " << readback.lastError().message << "\n";
            capture.close();
            return 1;
        }
        ValidateCpuFrame(cpu);
    }

    ValidateCacheStats("D3D11", readback.cacheStats(), frameCount);
    PrintPerformance("D3D11 readback integration", capture.performance());
    capture.close();
    IC4ExtTest::SleepAfterCameraAccess();
    return 0;
}
#endif

#if IC4EXT_ENABLE_D3D12
int RunD3D12Readback(int frameCount, int readTimeoutMs)
{
    if (!ShouldRunBackend("d3d12")) {
        return 77;
    }

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D12Core creation failed; skipping D3D12 readback integration: " << e.what() << "\n";
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping D3D12 readback integration\n";
        return 77;
    }

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config = IC4ExtTest::MakeCameraConfig("d3d12", 0);
    IC4Ext::D3D12CameraCapture capture;
    if (!capture.open(selector, config, backend)) {
        std::cerr << "D3D12 readback integration open failed; skipping: "
                  << capture.lastError().where << ": " << capture.lastError().message << "\n";
        return 77;
    }

    IC4Ext::D3D12FrameReadback readback;
    if (!readback.initialize(backend)) {
        std::cerr << "D3D12 readback initialize failed: " << readback.lastError().where
                  << ": " << readback.lastError().message << "\n";
        capture.close();
        return 1;
    }

    IC4Ext::CameraReadOptions readOptions;
    readOptions.mode = IC4Ext::ReadMode::NextFrame;
    readOptions.timeoutMs = static_cast<unsigned>(readTimeoutMs);

    for (int i = 0; i < frameCount; ++i) {
        auto result = capture.read(readOptions);
        if (!result) {
            std::cerr << "D3D12 readback integration read failed at frame " << i
                      << ": " << result.error.where << ": " << result.error.message << "\n";
            capture.close();
            return 1;
        }
        IC4Ext::CpuFrame cpu;
        if (!readback.readback(result.frame, IC4Ext::CpuFrameFormat::BGR8, cpu, static_cast<unsigned>(readTimeoutMs))) {
            std::cerr << "D3D12 readback failed at frame " << i << ": "
                      << readback.lastError().where << ": " << readback.lastError().message << "\n";
            capture.close();
            return 1;
        }
        ValidateCpuFrame(cpu);
    }

    ValidateCacheStats("D3D12", readback.cacheStats(), frameCount);
    PrintPerformance("D3D12 readback integration", capture.performance());
    capture.close();
    IC4ExtTest::SleepAfterCameraAccess();
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

    const int frameCount = IC4ExtTest::EnvInt("IC4EXT_TEST_READBACK_FRAMES", 5);
    const int readTimeoutMs = IC4ExtTest::EnvInt("IC4EXT_TEST_READBACK_TIMEOUT_MS", 5000);

    bool ranAnyPath = false;

#if IC4EXT_ENABLE_D3D11
    const int d3d11 = RunD3D11Readback(frameCount, readTimeoutMs);
    if (d3d11 == 1) return 1;
    if (d3d11 == 0) ranAnyPath = true;
#endif

#if IC4EXT_ENABLE_D3D12
    const int d3d12 = RunD3D12Readback(frameCount, readTimeoutMs);
    if (d3d12 == 1) return 1;
    if (d3d12 == 0) ranAnyPath = true;
#endif

    if (!ranAnyPath) {
        std::cerr << "No enabled backend completed the readback integration path; skipping\n";
        return 77;
    }

    std::cout << "test_camera1_readback_integration passed frames=" << frameCount << "\n";
    return 0;
}
