#include "IC4Ext/D3D12/ReadOnlyPipeline.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

int main()
{
    using namespace IC4Ext::D3D12;

    static_assert(!std::is_copy_constructible_v<FrameWriter>);
    static_assert(std::is_move_constructible_v<FrameWriter>);
    static_assert(!std::is_copy_constructible_v<PooledFrameConverter>);
    static_assert(std::is_move_constructible_v<PooledFrameConverter>);
    static_assert(!std::is_copy_constructible_v<CameraCapture>);
    static_assert(std::is_move_constructible_v<CameraCapture>);
    static_assert(!std::is_copy_constructible_v<CameraCaptureThread>);
    static_assert(!std::is_move_constructible_v<CameraCaptureThread>);
    static_assert(!std::is_copy_constructible_v<ReadOnlyFrameLifetimeTracker>);

    ReadOnlyFrame emptyFrame;
    assert(!emptyFrame.valid());
    assert(!emptyFrame.hasResource());
    assert(!emptyFrame.hasSrv());
    assert(emptyFrame.isReady());
    assert(emptyFrame.waitReady(0));
    assert(emptyFrame.useCount() == 0);
    assert(!emptyFrame.unique());

    FramePoolStats emptyPoolStats;
    assert(emptyPoolStats.inFlight() == 0);
    assert(emptyPoolStats.availableRatio() == 0.0);
    assert(emptyPoolStats.inFlightRatio() == 0.0);
    assert(emptyPoolStats.exhausted());

    emptyPoolStats.capacity = 8;
    emptyPoolStats.maxCapacity = 16;
    emptyPoolStats.available = 3;
    emptyPoolStats.writing = 2;
    emptyPoolStats.published = 1;
    assert(emptyPoolStats.inFlight() == 3);
    assert(emptyPoolStats.availableRatio() == 3.0 / 8.0);
    assert(emptyPoolStats.inFlightRatio() == 3.0 / 8.0);
    assert(!emptyPoolStats.exhausted());

    assert(FrameRateLimit::Maximum().isValid());
    assert(FrameRateLimit::Fixed(60.0).isValid());
    assert(!FrameRateLimit::Fixed(0.0).isValid());

    FrameSyncConfig config;
    config.cameraIds = {0, 1};
    config.timestampSource = FrameSyncTimestampSource::Auto;
    config.maxTimestampDiffNs = 1'000'000;
    assert(config.isValid());

    config.maxTimestampDiffNs = 0;
    assert(!config.isValid());
    config.maxTimestampDiffNs = 1'000'000;

    config.cameraIds = {0, 0};
    assert(!config.isValid());

    FramePoolConfig poolConfig;
    poolConfig.width = 1536;
    poolConfig.height = 1536;
    poolConfig.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    assert(poolConfig.isValid());
    poolConfig.createSrv = false;
    assert(!poolConfig.isValid());

    CameraCaptureOptions captureOptions;
    assert(captureOptions.isValid());
    captureOptions.maxFramePoolCapacity = 0;
    assert(!captureOptions.isValid());

    CameraCaptureThreadOptions threadOptions;
    assert(threadOptions.isValid());
    threadOptions.readTimeoutMs = 0;
    assert(!threadOptions.isValid());

    ReadOnlyFrameSet::FrameList frames;
    frames.push_back(IndexedReadOnlyFrame{1, {}});
    frames.push_back(IndexedReadOnlyFrame{0, {}});

    const auto completed = std::chrono::steady_clock::now();
    auto set = ReadOnlyFrameSet::Create(42, 123456, completed, std::move(frames));
    assert(set.valid());
    assert(set.syncGroupId() == 42);
    assert(set.referenceTimestampNs() == 123456);
    assert(set.completedTime() == completed);
    assert(set.size() == 2);
    assert(set[0].cameraId == 1);
    assert(set[1].cameraId == 0);
    assert(set.contains(1));
    assert(set.contains(0));
    assert(!set.contains(2));

    FrameSyncOutputConfig output;
    output.requiredCameras = {1, 0};
    output.frameRate = FrameRateLimit::Fixed(30.0);
    output.priority = 100;
    assert(output.frameRate.isValid());

    ReadOnlyFrameLifetimeTracker lifetimeTracker;
    assert(lifetimeTracker.retainedFrameCount() == 0);
    assert(lifetimeTracker.collectCompleted() == 0);
    assert(lifetimeTracker.waitAllAndClear(0));
    assert(!lifetimeTracker.retainUntil(ReadOnlyFrame{}, ::IC4Ext::D3D12ReadyToken{}));
    assert(!lifetimeTracker.retainUntil(set, ::IC4Ext::D3D12ReadyToken{}));
    const auto lifetimeStats = lifetimeTracker.stats();
    assert(lifetimeStats.retainedFrames == 0);
    assert(lifetimeStats.retainedTotal == 0);

    CameraCapture capture;
    assert(!capture.isOpened());

    auto ingress = std::make_shared<IndexedReadOnlyFrameQueue>(4);
    CameraCaptureThread captureThread(7, std::move(capture));
    captureThread.setOutputQueue(ingress);
    assert(captureThread.cameraId() == 7);
    assert(captureThread.outputQueue() == ingress);
    assert(!captureThread.isOpened());
    assert(!captureThread.isRunning());

    return 0;
}
