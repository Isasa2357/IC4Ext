#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>

#include <cassert>
#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

int main()
{
    using namespace IC4Ext::D3D11;

    static_assert(!std::is_copy_constructible_v<FrameWriter>);
    static_assert(std::is_move_constructible_v<FrameWriter>);
    static_assert(!std::is_copy_constructible_v<PooledFrameConverter>);
    static_assert(std::is_move_constructible_v<PooledFrameConverter>);
    static_assert(!std::is_copy_constructible_v<CameraCapture>);
    static_assert(std::is_move_constructible_v<CameraCapture>);
    static_assert(!std::is_copy_constructible_v<CameraCaptureThread>);
    static_assert(!std::is_move_constructible_v<CameraCaptureThread>);
    static_assert(!std::is_copy_constructible_v<ReadOnlyFrameLifetimeTracker>);
    static_assert(!std::is_copy_constructible_v<SyntheticFrameSource>);
    static_assert(!std::is_move_constructible_v<SyntheticFrameSource>);

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
    assert(!emptyPoolStats.exhausted());

    emptyPoolStats.capacity = 8;
    emptyPoolStats.maxCapacity = 16;
    emptyPoolStats.available = 3;
    emptyPoolStats.writing = 2;
    emptyPoolStats.published = 1;
    assert(emptyPoolStats.inFlight() == 3);
    assert(emptyPoolStats.availableRatio() == 3.0 / 8.0);
    assert(emptyPoolStats.inFlightRatio() == 3.0 / 8.0);
    assert(!emptyPoolStats.exhausted());

    emptyPoolStats.capacity = 16;
    emptyPoolStats.available = 0;
    assert(emptyPoolStats.exhausted());

    assert(FrameRateLimit::Maximum().isValid());
    assert(FrameRateLimit::Fixed(60.0).isValid());
    assert(!FrameRateLimit::Fixed(0.0).isValid());

    FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.timestampSource = FrameSyncTimestampSource::Auto;
    syncConfig.maxTimestampDiffNs = 1'000'000;
    assert(syncConfig.isValid());
    syncConfig.maxTimestampDiffNs = 0;
    assert(!syncConfig.isValid());
    syncConfig.maxTimestampDiffNs = 1'000'000;
    syncConfig.cameraIds = {0, 0};
    assert(!syncConfig.isValid());

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

    SyntheticFrameSourceConfig syntheticConfig;
    syntheticConfig.width = 1920;
    syntheticConfig.height = 1080;
    syntheticConfig.fps = 160.0;
    assert(syntheticConfig.isValid());
    assert(syntheticConfig.framePeriodNs() == 6'250'000ull);
    syntheticConfig.width = 0;
    assert(!syntheticConfig.isValid());
    syntheticConfig.width = 1920;
    syntheticConfig.fps = 0.0;
    assert(!syntheticConfig.isValid());
    syntheticConfig.fps = 2'000'000'000.0;
    assert(!syntheticConfig.isValid());
    syntheticConfig.fps = 160.0;
    syntheticConfig.initialFramePoolCapacity = 33;
    syntheticConfig.maxFramePoolCapacity = 32;
    assert(!syntheticConfig.isValid());

    ReadOnlyFrameSet::FrameList frames;
    frames.push_back(IndexedReadOnlyFrame{1, {}});
    frames.push_back(IndexedReadOnlyFrame{0, {}});
    const auto completed = std::chrono::steady_clock::now();
    auto frameSet = ReadOnlyFrameSet::Create(
        42,
        123456,
        completed,
        std::move(frames));
    assert(frameSet.valid());
    assert(frameSet.syncGroupId() == 42);
    assert(frameSet.referenceTimestampNs() == 123456);
    assert(frameSet.completedTime() == completed);
    assert(frameSet.size() == 2);
    assert(frameSet[0].cameraId == 1);
    assert(frameSet[1].cameraId == 0);
    assert(frameSet.contains(0));
    assert(frameSet.contains(1));
    assert(!frameSet.contains(2));

    FrameSyncOutputConfig outputConfig;
    outputConfig.requiredCameras = {1, 0};
    outputConfig.frameRate = FrameRateLimit::Fixed(30.0);
    outputConfig.priority = 100;
    assert(outputConfig.frameRate.isValid());

    ReadOnlyFrameLifetimeTracker lifetimeTracker;
    assert(lifetimeTracker.retainedFrameCount() == 0);
    assert(lifetimeTracker.collectCompleted() == 0);
    assert(lifetimeTracker.waitAllAndClear(0));
    assert(!lifetimeTracker.retainUntil(
        ReadOnlyFrame{},
        ::IC4Ext::D3D11ReadyToken{}));
    assert(!lifetimeTracker.retainUntil(
        frameSet,
        ::IC4Ext::D3D11ReadyToken{}));
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
