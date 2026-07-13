#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#if IC4EXT_ENABLE_D3D11
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#endif

#if IC4EXT_ENABLE_D3D12
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

namespace {

void Stage(const char* backend, const char* stage)
{
    std::cerr << "[multi-camera-startup][" << backend << "] "
              << stage << std::endl;
}

template <class StartupResult>
void ShutdownStartedGroup(StartupResult& started) noexcept
{
    for (auto& thread : started.captureThreads) {
        if (thread) thread->stopAcquisition();
    }
    for (auto& entry : started.captures) {
        entry.capture.stopAcquisition();
    }
    for (auto& thread : started.captureThreads) {
        if (thread) thread->stopAndJoin();
    }
    for (auto& entry : started.captures) {
        entry.capture.close();
    }
}

template <class ReadOnlyFrame>
bool ValidateFrame(
    const char* backend,
    const char* source,
    const ReadOnlyFrame& frame,
    std::uint32_t readyTimeoutMs)
{
    if (!frame) {
        std::cerr << backend << ' ' << source
                  << " returned an invalid ReadOnlyFrame\n";
        return false;
    }
    if (!frame.waitReady(readyTimeoutMs)) {
        std::cerr << backend << ' ' << source
                  << " GPU-ready wait timed out\n";
        return false;
    }

    const auto& format = frame.format();
    if (format.width <= 0 || format.height <= 0) {
        std::cerr << backend << ' ' << source
                  << " returned invalid dimensions "
                  << format.width << 'x' << format.height << '\n';
        return false;
    }

    std::cout << backend << ' ' << source
              << " frameNumber=" << frame.timing().frameNumber
              << " size=" << format.width << 'x' << format.height
              << '\n';
    return true;
}

#if IC4EXT_ENABLE_D3D11
int RunD3D11Path()
{
    namespace Pipe = IC4Ext::D3D11;

    Stage("D3D11", "creating backend");
    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        core = D3D11CoreLib::D3D11Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D11Core creation failed; skipping D3D11 path: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D11BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D11 backend resolve failed; skipping D3D11 path\n";
        return 77;
    }

    const int directDevice =
        IC4ExtTest::EnvInt("IC4EXT_TEST_DIRECT_DEVICE", 0);
    const int threadedDevice =
        IC4ExtTest::EnvInt("IC4EXT_TEST_THREADED_DEVICE", 1);
    const auto readyTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt("IC4EXT_TEST_GPU_READY_TIMEOUT_MS", 5000)));
    const auto readTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt("IC4EXT_TEST_READ_TIMEOUT_MS", 5000)));

    ThreadKit::Queues::QueueOptions queueOptions;
    queueOptions.maxSize = 4;
    queueOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto threadQueue =
        std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(queueOptions);

    Pipe::CameraCaptureStartupConfig direct;
    direct.cameraId = 0;
    direct.selector.deviceIndex = directDevice;
    direct.captureConfig = IC4ExtTest::MakeCameraConfig("d3d11", directDevice);
    direct.openOrder = 0;

    Pipe::CameraCaptureThreadStartupConfig threaded;
    threaded.capture.cameraId = 1;
    threaded.capture.selector.deviceIndex = threadedDevice;
    threaded.capture.captureConfig =
        IC4ExtTest::MakeCameraConfig("d3d11", threadedDevice);
    threaded.capture.openOrder = 1;
    threaded.threadOptions.readTimeoutMs = readTimeoutMs;
    threaded.threadOptions.stopOnReadError = false;
    threaded.outputQueue = threadQueue;

    Pipe::MultiCameraStartupOptions startupOptions;
    startupOptions.interCameraOpenDelay = std::chrono::milliseconds(std::max(
        0,
        IC4ExtTest::EnvInt("IC4EXT_TEST_INTER_CAMERA_DELAY_MS", 1000)));

    Stage("D3D11", "opening mixed direct/threaded group");
    auto started = Pipe::OpenAndStartMultiCameraGroup(
        backend,
        std::vector<Pipe::CameraCaptureStartupConfig>{direct},
        std::vector<Pipe::CameraCaptureThreadStartupConfig>{threaded},
        startupOptions);

    if (!started) {
        std::cerr << "D3D11 multi-camera startup failed: "
                  << started.error.where << ": "
                  << started.error.message << '\n';
        return 1;
    }

    if (started.captures.size() != 1 ||
        started.captureThreads.size() != 1 ||
        !started.captureThreads[0] ||
        started.captures[0].cameraId != 0 ||
        started.captureThreads[0]->cameraId() != 1 ||
        !started.captureThreads[0]->isRunning()) {
        std::cerr << "D3D11 startup result shape/state is invalid\n";
        ShutdownStartedGroup(started);
        return 1;
    }

    bool valid = true;
    {
        Stage("D3D11", "reading one direct frame");
        auto directFrame = started.captures[0].capture.read(
            IC4Ext::CameraReadOptions{
                IC4Ext::ReadMode::NextFrame,
                readTimeoutMs});
        if (!directFrame) {
            std::cerr << "D3D11 direct read failed: "
                      << directFrame.error.where << ": "
                      << directFrame.error.message << '\n';
            valid = false;
        } else {
            valid = ValidateFrame(
                        "D3D11",
                        "direct",
                        directFrame.frame,
                        readyTimeoutMs) &&
                    valid;
        }

        Stage("D3D11", "waiting for one threaded frame");
        auto threadedFrame = threadQueue->waitPopFor(
            std::chrono::milliseconds(readTimeoutMs));
        if (!threadedFrame) {
            std::cerr << "D3D11 threaded queue timed out\n";
            valid = false;
        } else {
            if (threadedFrame->cameraId != 1) {
                std::cerr << "D3D11 threaded frame has unexpected cameraId="
                          << threadedFrame->cameraId << '\n';
                valid = false;
            }
            valid = ValidateFrame(
                        "D3D11",
                        "threaded",
                        threadedFrame->frame,
                        readyTimeoutMs) &&
                    valid;
        }
    }

    Stage("D3D11", "shutting down");
    ShutdownStartedGroup(started);
    IC4ExtTest::SleepAfterCameraAccess();

    if (!valid) return 1;
    std::cout << "D3D11 mixed multi-camera startup integration passed\n";
    return 0;
}
#endif

#if IC4EXT_ENABLE_D3D12
int RunD3D12Path()
{
    namespace Pipe = IC4Ext::D3D12;

    Stage("D3D12", "creating backend");
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Core creation failed; skipping D3D12 path: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping D3D12 path\n";
        return 77;
    }

    const int directDevice =
        IC4ExtTest::EnvInt("IC4EXT_TEST_DIRECT_DEVICE", 0);
    const int threadedDevice =
        IC4ExtTest::EnvInt("IC4EXT_TEST_THREADED_DEVICE", 1);
    const auto readyTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt("IC4EXT_TEST_GPU_READY_TIMEOUT_MS", 5000)));
    const auto readTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt("IC4EXT_TEST_READ_TIMEOUT_MS", 5000)));

    ThreadKit::Queues::QueueOptions queueOptions;
    queueOptions.maxSize = 4;
    queueOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto threadQueue =
        std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(queueOptions);

    Pipe::CameraCaptureStartupConfig direct;
    direct.cameraId = 0;
    direct.selector.deviceIndex = directDevice;
    direct.captureConfig = IC4ExtTest::MakeCameraConfig("d3d12", directDevice);
    direct.openOrder = 0;

    Pipe::CameraCaptureThreadStartupConfig threaded;
    threaded.capture.cameraId = 1;
    threaded.capture.selector.deviceIndex = threadedDevice;
    threaded.capture.captureConfig =
        IC4ExtTest::MakeCameraConfig("d3d12", threadedDevice);
    threaded.capture.openOrder = 1;
    threaded.threadOptions.readTimeoutMs = readTimeoutMs;
    threaded.threadOptions.stopOnReadError = false;
    threaded.outputQueue = threadQueue;

    Pipe::MultiCameraStartupOptions startupOptions;
    startupOptions.interCameraOpenDelay = std::chrono::milliseconds(std::max(
        0,
        IC4ExtTest::EnvInt("IC4EXT_TEST_INTER_CAMERA_DELAY_MS", 1000)));

    Stage("D3D12", "opening mixed direct/threaded group");
    auto started = Pipe::OpenAndStartMultiCameraGroup(
        backend,
        std::vector<Pipe::CameraCaptureStartupConfig>{direct},
        std::vector<Pipe::CameraCaptureThreadStartupConfig>{threaded},
        startupOptions);

    if (!started) {
        std::cerr << "D3D12 multi-camera startup failed: "
                  << started.error.where << ": "
                  << started.error.message << '\n';
        return 1;
    }

    if (started.captures.size() != 1 ||
        started.captureThreads.size() != 1 ||
        !started.captureThreads[0] ||
        started.captures[0].cameraId != 0 ||
        started.captureThreads[0]->cameraId() != 1 ||
        !started.captureThreads[0]->isRunning()) {
        std::cerr << "D3D12 startup result shape/state is invalid\n";
        ShutdownStartedGroup(started);
        return 1;
    }

    bool valid = true;
    {
        Stage("D3D12", "reading one direct frame");
        auto directFrame = started.captures[0].capture.read(
            IC4Ext::CameraReadOptions{
                IC4Ext::ReadMode::NextFrame,
                readTimeoutMs});
        if (!directFrame) {
            std::cerr << "D3D12 direct read failed: "
                      << directFrame.error.where << ": "
                      << directFrame.error.message << '\n';
            valid = false;
        } else {
            valid = ValidateFrame(
                        "D3D12",
                        "direct",
                        directFrame.frame,
                        readyTimeoutMs) &&
                    valid;
        }

        Stage("D3D12", "waiting for one threaded frame");
        auto threadedFrame = threadQueue->waitPopFor(
            std::chrono::milliseconds(readTimeoutMs));
        if (!threadedFrame) {
            std::cerr << "D3D12 threaded queue timed out\n";
            valid = false;
        } else {
            if (threadedFrame->cameraId != 1) {
                std::cerr << "D3D12 threaded frame has unexpected cameraId="
                          << threadedFrame->cameraId << '\n';
                valid = false;
            }
            valid = ValidateFrame(
                        "D3D12",
                        "threaded",
                        threadedFrame->frame,
                        readyTimeoutMs) &&
                    valid;
        }
    }

    Stage("D3D12", "shutting down");
    ShutdownStartedGroup(started);
    core->WaitIdle();
    IC4ExtTest::SleepAfterCameraAccess();

    if (!valid) return 1;
    std::cout << "D3D12 mixed multi-camera startup integration passed\n";
    return 0;
}
#endif

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    Stage("test", "enumerating cameras");
    if (!IC4ExtTest::RequireCameraCount(2)) return 77;

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
        std::cerr << "No enabled backend completed the multi-camera startup path; skipping\n";
        return 77;
    }

    std::cout << "test_multi_camera_startup_integration passed\n";
    return 0;
}
