#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    if (!IC4ExtTest::RequireCameraCount(2)) {
        return 77;
    }

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D12Core creation failed; skipping camera2plus test: " << e.what() << "\n";
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping camera2plus test\n";
        return 77;
    }

    ThreadKit::Queues::QueueOptions inputOptions;
    inputOptions.maxSize = 64;
    auto inputQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = 16;
    auto outputQueue = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

    IC4Ext::FrameSyncOptions syncOptions;
    syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    syncOptions.cameraIndices = {0, 1};
    syncOptions.maxTimestampDiffNs = std::numeric_limits<std::uint64_t>::max();
    syncOptions.maxBufferedFramesPerCamera = 16;

    IC4Ext::D3D12FrameSyncThread sync(inputQueue, outputQueue, syncOptions);
    if (!sync.start()) {
        std::cerr << "D3D12FrameSyncThread start failed: " << sync.lastError().where
                  << ": " << sync.lastError().message << "\n";
        return 1;
    }

    IC4Ext::CameraThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 1000;
    threadOptions.copyPerOutputQueue = true;
    threadOptions.stopOnReadError = false;

    IC4Ext::IC4DeviceSelector selector0;
    selector0.deviceIndex = 0;
    IC4Ext::IC4DeviceSelector selector1;
    selector1.deviceIndex = 1;

    IC4Ext::D3D12CameraCaptureThread camera0(selector0,
                                             IC4ExtTest::MakeCameraConfig("d3d12", 0),
                                             backend,
                                             threadOptions);
    IC4Ext::D3D12CameraCaptureThread camera1(selector1,
                                             IC4ExtTest::MakeCameraConfig("d3d12", 1),
                                             backend,
                                             threadOptions);
    camera0.addOutputQueue(0, inputQueue);
    camera1.addOutputQueue(1, inputQueue);

    bool camera0Started = false;
    bool camera1Started = false;

    camera0Started = camera0.start();
    if (!camera0Started) {
        std::cerr << "camera0 start/open failed; skipping camera2plus test: "
                  << camera0.lastError().where << ": " << camera0.lastError().message << "\n";
        sync.stopAndJoin();
        return 77;
    }

    IC4ExtTest::SleepAfterCameraAccess();

    camera1Started = camera1.start();
    if (!camera1Started) {
        std::cerr << "camera1 start/open failed; skipping camera2plus test: "
                  << camera1.lastError().where << ": " << camera1.lastError().message << "\n";
        camera0.stopAndJoin();
        sync.stopAndJoin();
        return 77;
    }

    auto set = outputQueue->waitPopFor(std::chrono::milliseconds(10000));

    camera0.stopAndJoin();
    camera1.stopAndJoin();
    sync.stopAndJoin();

    if (!set) {
        const auto s0 = camera0.stats();
        const auto s1 = camera1.stats();
        const auto ss = sync.stats();
        std::cerr << "Timed out waiting for synced two-camera frame set\n"
                  << "camera0 read=" << s0.readFrames << " pushed=" << s0.pushedFrames << " errors=" << s0.readErrors << "\n"
                  << "camera1 read=" << s1.readFrames << " pushed=" << s1.pushedFrames << " errors=" << s1.readErrors << "\n"
                  << "sync input=" << ss.inputFrames << " emitted=" << ss.emittedSets << " dropped=" << ss.droppedFrames << "\n";
        return 1;
    }

    if (set->frames.size() != 2) {
        std::cerr << "Expected two synced frames, got " << set->frames.size() << "\n";
        return 1;
    }
    if (set->frames[0].cameraIndex != 0 || set->frames[1].cameraIndex != 1) {
        std::cerr << "Unexpected camera indices in synced set: "
                  << set->frames[0].cameraIndex << ", " << set->frames[1].cameraIndex << "\n";
        return 1;
    }
    if (!set->frames[0].frame.texture || !set->frames[1].frame.texture) {
        std::cerr << "Synced set contains an empty GPU texture\n";
        return 1;
    }

    std::cout << "test_camera2plus_frame_sync_smoke passed\n";
    return 0;
}
