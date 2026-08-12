#include "TestCameraUtils.hpp"

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>
#include <ic4/ic4.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace Pipe = IC4Ext::D3D12;

struct EnumeratedDevice
{
    int index = -1;
    std::string serial;
};

std::vector<EnumeratedDevice> EnumerateDevices()
{
    ic4::InitLibraryConfig config;
    config.defaultErrorHandlerBehavior = ic4::ErrorHandlerBehavior::Ignore;

    if (!ic4::initLibrary(config)) {
        std::cerr << "IC4 library initialization failed while enumerating serials\n";
        return {};
    }

    std::vector<EnumeratedDevice> result;
    {
        ic4::Error enumError;
        auto devices = ic4::DeviceEnum::enumDevices(enumError);
        if (enumError.isError()) {
            std::cerr << "IC4 device enumeration failed: "
                      << enumError.message() << '\n';
        } else {
            result.reserve(devices.size());
            for (std::size_t index = 0; index < devices.size(); ++index) {
                ic4::Error serialError;
                std::string serial = devices[index].serial(serialError);
                if (serialError.isError()) {
                    std::cerr << "Could not read serial for device " << index
                              << ": " << serialError.message() << '\n';
                    serial.clear();
                }
                result.push_back(EnumeratedDevice{
                    static_cast<int>(index),
                    std::move(serial)});
            }
        }
    }

    ic4::exitLibrary();
    return result;
}

const EnumeratedDevice* FindDevice(
    const std::vector<EnumeratedDevice>& devices,
    int index)
{
    const auto it = std::find_if(
        devices.begin(),
        devices.end(),
        [index](const EnumeratedDevice& device) {
            return device.index == index;
        });
    return it == devices.end() ? nullptr : &(*it);
}

std::string MakeMissingSerial(
    const std::vector<EnumeratedDevice>& devices)
{
    std::string candidate = "__IC4EXT_SERIAL_DOES_NOT_EXIST__";
    const auto exists = [&devices](const std::string& serial) {
        return std::any_of(
            devices.begin(),
            devices.end(),
            [&serial](const EnumeratedDevice& device) {
                return device.serial == serial;
            });
    };

    while (exists(candidate)) {
        candidate.push_back('_');
    }
    return candidate;
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

bool ValidateFrame(
    const char* source,
    const Pipe::ReadOnlyFrame& frame,
    std::uint32_t readyTimeoutMs)
{
    if (!frame) {
        std::cerr << source << " returned an invalid ReadOnlyFrame\n";
        return false;
    }
    if (!frame.waitReady(readyTimeoutMs)) {
        std::cerr << source << " GPU-ready wait timed out\n";
        return false;
    }

    const auto& format = frame.format();
    if (format.width <= 0 || format.height <= 0) {
        std::cerr << source << " returned invalid dimensions "
                  << format.width << 'x' << format.height << '\n';
        return false;
    }

    std::cout << source
              << " frameNumber=" << frame.timing().frameNumber
              << " size=" << format.width << 'x' << format.height
              << '\n';
    return true;
}

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    const int directDevice =
        IC4ExtTest::EnvInt("IC4EXT_TEST_DIRECT_DEVICE", 0);
    const int threadedDevice =
        IC4ExtTest::EnvInt("IC4EXT_TEST_THREADED_DEVICE", 1);

    if (directDevice < 0 || threadedDevice < 0 ||
        directDevice == threadedDevice) {
        std::cerr << "Serial-selection test requires two distinct non-negative "
                     "device indices\n";
        return 1;
    }

    std::cerr << "[d3d12-serial-selection] enumerating camera serials\n";
    const auto devices = EnumerateDevices();
    const auto* directInfo = FindDevice(devices, directDevice);
    const auto* threadedInfo = FindDevice(devices, threadedDevice);
    if (!directInfo || !threadedInfo) {
        std::cerr << "Skipping serial-selection test: requested device indices "
                  << directDevice << " and " << threadedDevice
                  << " are not both available\n";
        return 77;
    }
    if (directInfo->serial.empty() || threadedInfo->serial.empty()) {
        std::cerr << "Skipping serial-selection test: both cameras must expose "
                     "non-empty serial numbers\n";
        return 77;
    }
    if (directInfo->serial == threadedInfo->serial) {
        std::cerr << "Two enumerated cameras reported the same serial number\n";
        return 1;
    }

    std::cerr << "[d3d12-serial-selection] creating backend\n";
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Core creation failed; skipping test: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping test\n";
        return 77;
    }

    const auto readyTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt("IC4EXT_TEST_GPU_READY_TIMEOUT_MS", 5000)));
    const auto readTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt("IC4EXT_TEST_READ_TIMEOUT_MS", 5000)));

    // A specified serial must be authoritative. Even though deviceIndex points
    // at a valid camera, an unknown serial must fail rather than falling back.
    std::cerr << "[d3d12-serial-selection] checking missing-serial rejection\n";
    {
        Pipe::CameraCapture missingCapture;
        IC4Ext::IC4DeviceSelector missingSelector;
        missingSelector.serial = MakeMissingSerial(devices);
        missingSelector.deviceIndex = directDevice;

        const auto missingConfig =
            IC4ExtTest::MakeCameraConfig("d3d12", directDevice);
        if (missingCapture.open(missingSelector, missingConfig, backend)) {
            std::cerr << "Unknown serial unexpectedly fell back to deviceIndex\n";
            missingCapture.close();
            return 1;
        }

        const auto error = missingCapture.lastError();
        const bool serialError =
            error.where.find("serial") != std::string::npos ||
            error.message.find("serial") != std::string::npos;
        if (!serialError) {
            std::cerr << "Unknown serial failed for an unexpected reason: "
                      << error.where << ": " << error.message << '\n';
            return 1;
        }
    }

    ThreadKit::Queues::QueueOptions queueOptions;
    queueOptions.maxSize = 4;
    queueOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto threadQueue =
        std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(queueOptions);

    Pipe::CameraCaptureStartupConfig direct;
    direct.cameraId = 0;
    direct.selector.serial = directInfo->serial;
    direct.selector.deviceIndex = directDevice;
    direct.captureConfig =
        IC4ExtTest::MakeCameraConfig("d3d12", directDevice);
    direct.openOrder = 0;

    Pipe::CameraCaptureThreadStartupConfig threaded;
    threaded.capture.cameraId = 1;
    threaded.capture.selector.serial = threadedInfo->serial;

    // Deliberately use the same fallback index as the direct camera. Successful
    // two-camera startup therefore requires the distinct serials to be honored.
    threaded.capture.selector.deviceIndex = directDevice;
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

    std::cerr << "[d3d12-serial-selection] opening two cameras by serial\n";
    auto started = Pipe::OpenAndStartMultiCameraGroup(
        backend,
        std::vector<Pipe::CameraCaptureStartupConfig>{direct},
        std::vector<Pipe::CameraCaptureThreadStartupConfig>{threaded},
        startupOptions);

    if (!started) {
        std::cerr << "Serial-based multi-camera startup failed: "
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
        std::cerr << "Serial-based startup result shape/state is invalid\n";
        ShutdownStartedGroup(started);
        return 1;
    }

    bool valid = true;
    {
        std::cerr << "[d3d12-serial-selection] reading direct frame\n";
        auto directFrame = started.captures[0].capture.read(
            IC4Ext::CameraReadOptions{
                IC4Ext::ReadMode::NextFrame,
                readTimeoutMs});
        if (!directFrame) {
            std::cerr << "Direct serial-selected read failed: "
                      << directFrame.error.where << ": "
                      << directFrame.error.message << '\n';
            valid = false;
        } else {
            valid = ValidateFrame(
                        "D3D12 serial direct",
                        directFrame.frame,
                        readyTimeoutMs) &&
                    valid;
        }

        std::cerr << "[d3d12-serial-selection] waiting for threaded frame\n";
        auto threadedFrame = threadQueue->waitPopFor(
            std::chrono::milliseconds(readTimeoutMs));
        if (!threadedFrame) {
            std::cerr << "Threaded serial-selected queue timed out\n";
            valid = false;
        } else {
            if (threadedFrame->cameraId != 1) {
                std::cerr << "Threaded frame has unexpected cameraId="
                          << threadedFrame->cameraId << '\n';
                valid = false;
            }
            valid = ValidateFrame(
                        "D3D12 serial threaded",
                        threadedFrame->frame,
                        readyTimeoutMs) &&
                    valid;
        }
    }

    std::cerr << "[d3d12-serial-selection] shutting down\n";
    ShutdownStartedGroup(started);
    core->WaitIdle();

    if (!valid) return 1;

    std::cout << "D3D12 serial selection integration passed\n";
    return 0;
}
