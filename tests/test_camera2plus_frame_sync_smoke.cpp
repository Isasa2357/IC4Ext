#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr const char* kBuildMarker = "camera2plus-command-lifecycle-v4";

std::uint64_t HostTimestampNs(const IC4Ext::D3D12IndexedCameraFrame& frame)
{
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        frame.frame.timing.hostReceivedTime.time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t AbsDiff(std::uint64_t lhs, std::uint64_t rhs)
{
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

void PrintStage(const char* stage)
{
    std::cerr << "[camera2plus] " << stage << std::endl;
}

void PrintStats(const IC4Ext::D3D12CameraCaptureThread& camera0,
                const IC4Ext::D3D12CameraCaptureThread& camera1,
                const IC4Ext::D3D12FrameSyncThread& sync,
                int receivedSets)
{
    const auto stats0 = camera0.stats();
    const auto stats1 = camera1.stats();
    const auto syncStats = sync.stats();
    std::cerr << "[camera2plus] sets=" << receivedSets
              << " syncInput=" << syncStats.inputFrames
              << " syncEmitted=" << syncStats.emittedSets
              << " syncDropped=" << syncStats.droppedFrames
              << " syncIgnored=" << syncStats.ignoredFrames
              << " camera0{read=" << stats0.readFrames
              << ",pushed=" << stats0.pushedFrames
              << ",timeouts=" << stats0.readTimeouts
              << ",errors=" << stats0.readErrors << "}"
              << " camera1{read=" << stats1.readFrames
              << ",pushed=" << stats1.pushedFrames
              << ",timeouts=" << stats1.readTimeouts
              << ",errors=" << stats1.readErrors << "}"
              << std::endl;
}

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    std::cerr << "[camera2plus] build=" << kBuildMarker << std::endl;
    PrintStage("enumerating cameras");
    if (!IC4ExtTest::RequireCameraCount(2)) return 77;

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        PrintStage("creating D3D12Core");
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Core creation failed; skipping camera2plus test: "
                  << exception.what() << "\n";
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping camera2plus test\n";
        return 77;
    }

    const bool hardwareTrigger = IC4ExtTest::EnvInt("IC4EXT_TEST_HW_TRIGGER", 0) != 0;
    const char* triggerSourceEnvironment = IC4ExtTest::Env("IC4EXT_TEST_TRIGGER_SOURCE");
    const std::string triggerSource = triggerSourceEnvironment
                                          ? triggerSourceEnvironment
                                          : "Line1";
    const std::uint64_t toleranceNs = IC4ExtTest::EnvUInt64(
        "IC4EXT_TEST_SYNC_TOLERANCE_NS",
        hardwareTrigger ? 4'000'000ull : 10'000'000ull);
    const int targetSets =
        std::max(1, IC4ExtTest::EnvInt("IC4EXT_TEST_SYNC_SETS", 100));
    const int timeoutSeconds =
        std::max(5, IC4ExtTest::EnvInt("IC4EXT_TEST_SYNC_TIMEOUT_SECONDS", 30));
    const int interCameraDelayMs =
        std::max(0, IC4ExtTest::EnvInt("IC4EXT_TEST_INTER_CAMERA_DELAY_MS", 1000));

    std::cerr << "[camera2plus] mode=" << (hardwareTrigger ? "hardware" : "free-run")
              << " triggerSource=" << triggerSource
              << " toleranceNs=" << toleranceNs
              << " targetSets=" << targetSets
              << " timeoutSeconds=" << timeoutSeconds
              << " interCameraDelayMs=" << interCameraDelayMs
              << std::endl;

    auto config0 = IC4ExtTest::MakeCameraConfig("d3d12", 0);
    auto config1 = IC4ExtTest::MakeCameraConfig("d3d12", 1);

    config0.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;
    config1.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;
    config0.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config1.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config0.maxPendingBuffers = 32;
    config1.maxPendingBuffers = 32;

    if (hardwareTrigger) {
        IC4Ext::ConfigureHardwareTriggerSync(config0, triggerSource);
        IC4Ext::ConfigureHardwareTriggerSync(config1, triggerSource);
    } else {
        auto gate = IC4Ext::MakeSoftwareTriggerSyncConfig();
        gate.setExposureAutoOff = false;
        IC4Ext::ConfigureCameraSync(config0, gate);
        IC4Ext::ConfigureCameraSync(config1, gate);
    }

    IC4Ext::IC4DeviceSelector selector0;
    selector0.deviceIndex = 0;
    IC4Ext::IC4DeviceSelector selector1;
    selector1.deviceIndex = 1;

    IC4Ext::D3D12CameraCapture capture0;
    IC4Ext::D3D12CameraCapture capture1;

    PrintStage("opening camera 0");
    if (!capture0.open(selector0, config0, backend)) {
        std::cerr << "camera0 open failed: " << capture0.lastError().where << ": "
                  << capture0.lastError().message << "\n";
        return 1;
    }
    PrintStage("pausing camera 0 with AcquisitionStop command");
    if (!capture0.setIC4Property("AcquisitionStop", std::string("execute"))) {
        std::cerr << "camera0 AcquisitionStop command failed: "
                  << capture0.lastError().where << ": "
                  << capture0.lastError().message << "\n";
        return 1;
    }

    if (interCameraDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interCameraDelayMs));
    }

    PrintStage("opening camera 1");
    if (!capture1.open(selector1, config1, backend)) {
        std::cerr << "camera1 open failed: " << capture1.lastError().where << ": "
                  << capture1.lastError().message << "\n";
        capture0.close();
        return 1;
    }
    PrintStage("pausing camera 1 with AcquisitionStop command");
    if (!capture1.setIC4Property("AcquisitionStop", std::string("execute"))) {
        std::cerr << "camera1 AcquisitionStop command failed: "
                  << capture1.lastError().where << ": "
                  << capture1.lastError().message << "\n";
        capture0.close();
        capture1.close();
        return 1;
    }

    ThreadKit::Queues::QueueOptions inputOptions;
    inputOptions.maxSize = 256;
    auto inputQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = 32;
    auto outputQueue = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

    IC4Ext::FrameSyncOptions syncOptions;
    syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
    syncOptions.cameraIndices = {0, 1};
    syncOptions.maxTimestampDiffNs = toleranceNs;
    syncOptions.maxBufferedFramesPerCamera = 32;

    IC4Ext::D3D12FrameSyncThread sync(inputQueue, outputQueue, syncOptions);
    PrintStage("starting sync thread");
    if (!sync.start()) {
        std::cerr << "D3D12FrameSyncThread start failed: " << sync.lastError().where
                  << ": " << sync.lastError().message << "\n";
        return 1;
    }

    IC4Ext::CameraThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 1000;
    threadOptions.copyPerOutputQueue = false;
    threadOptions.stopOnReadError = false;

    IC4Ext::D3D12CameraCaptureThread camera0(std::move(capture0), backend, threadOptions);
    IC4Ext::D3D12CameraCaptureThread camera1(std::move(capture1), backend, threadOptions);
    camera0.addOutputQueue(0, inputQueue);
    camera1.addOutputQueue(1, inputQueue);

    PrintStage("starting camera workers");
    if (!camera0.start() || !camera1.start()) {
        std::cerr << "camera worker start failed\n";
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        sync.stopAndJoin();
        return 1;
    }

    if (!hardwareTrigger) {
        PrintStage("releasing free-run trigger gates");
        if (!camera0.setIC4Property("TriggerMode", std::string("Off")) ||
            !camera1.setIC4Property("TriggerMode", std::string("Off"))) {
            std::cerr << "Failed to release free-run trigger gate\n";
            camera0.stopAndJoin();
            camera1.stopAndJoin();
            sync.stopAndJoin();
            return 1;
        }
    }

    PrintStage("starting camera acquisitions with AcquisitionStart commands");
    if (!camera0.startAcquisition()) {
        std::cerr << "camera0 startAcquisition failed: " << camera0.lastError().where
                  << ": " << camera0.lastError().message << "\n";
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        sync.stopAndJoin();
        return 1;
    }
    if (!camera1.startAcquisition()) {
        std::cerr << "camera1 startAcquisition failed: " << camera1.lastError().where
                  << ": " << camera1.lastError().message << "\n";
        camera0.stopAcquisition();
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        sync.stopAndJoin();
        return 1;
    }

    PrintStage("camera acquisitions started; waiting for initial frames");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    PrintStats(camera0, camera1, sync, 0);

    int receivedSets = 0;
    std::uint64_t maximumObservedDiffNs = 0;
    std::string validationFailure;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    auto nextStatsPrint = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    PrintStage("collecting synchronized sets");
    while (receivedSets < targetSets && std::chrono::steady_clock::now() < deadline) {
        auto set = outputQueue->waitPopFor(std::chrono::milliseconds(200));

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatsPrint) {
            PrintStats(camera0, camera1, sync, receivedSets);
            nextStatsPrint = now + std::chrono::seconds(1);
        }

        if (!set) continue;

        if (set->frames.size() != 2 ||
            set->frames[0].cameraIndex != 0 ||
            set->frames[1].cameraIndex != 1) {
            validationFailure = "Unexpected synced frame set layout";
            break;
        }
        if (!set->frames[0].frame.texture || !set->frames[1].frame.texture) {
            validationFailure = "Synced set contains an empty GPU texture";
            break;
        }

        const std::uint64_t timestamp0 = HostTimestampNs(set->frames[0]);
        const std::uint64_t timestamp1 = HostTimestampNs(set->frames[1]);
        if (timestamp0 == 0 || timestamp1 == 0) {
            validationFailure = "Synced set has no host receive timestamp";
            break;
        }

        const std::uint64_t differenceNs = AbsDiff(timestamp0, timestamp1);
        maximumObservedDiffNs = std::max(maximumObservedDiffNs, differenceNs);
        if (differenceNs > toleranceNs) {
            validationFailure = "Host timestamp difference exceeded tolerance";
            break;
        }
        ++receivedSets;
    }

    PrintStats(camera0, camera1, sync, receivedSets);
    PrintStage("stopping acquisitions");
    const bool camera0Stopped = camera0.stopAcquisition();
    const bool camera1Stopped = camera1.stopAcquisition();
    PrintStage("joining workers");
    camera0.stopAndJoin();
    camera1.stopAndJoin();
    sync.stopAndJoin();
    core->WaitIdle();

    const auto camera0Stats = camera0.stats();
    const auto camera1Stats = camera1.stats();
    const auto syncStats = sync.stats();

    if (!validationFailure.empty()) {
        std::cerr << validationFailure << "\n";
        return 1;
    }
    if (!camera0Stopped || !camera1Stopped) {
        std::cerr << "One or more cameras failed AcquisitionStop command\n";
        return 1;
    }
    if (receivedSets < targetSets) {
        std::cerr << "Timed out while waiting for synced sets: received="
                  << receivedSets << " target=" << targetSets << "\n";
        return 1;
    }
    if (camera0Stats.readErrors != 0 || camera1Stats.readErrors != 0) {
        std::cerr << "Camera read errors were recorded: camera0="
                  << camera0Stats.readErrors << " camera1=" << camera1Stats.readErrors << "\n";
        return 1;
    }

    std::cout << "test_camera2plus_frame_sync_smoke passed"
              << " mode=" << (hardwareTrigger ? "hardware" : "free-run")
              << " sets=" << receivedSets
              << " toleranceNs=" << toleranceNs
              << " maxObservedDiffNs=" << maximumObservedDiffNs
              << " syncInput=" << syncStats.inputFrames
              << " syncEmitted=" << syncStats.emittedSets
              << " syncDropped=" << syncStats.droppedFrames
              << " camera0Read=" << camera0Stats.readFrames
              << " camera1Read=" << camera1Stats.readFrames
              << "\n";
    return 0;
}
