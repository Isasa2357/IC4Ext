#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {

class DummyReadOnlyCamera final : public IC4Ext::D3D12::ReadOnlyFrameSource
{
public:
    bool initialize(IC4Ext::D3D12BackendContext backend,
                    std::uint64_t frameCount)
    {
        frameCount_ = frameCount;

        IC4Ext::D3D12::FramePoolConfig config;
        config.width = 2;
        config.height = 2;
        config.format = DXGI_FORMAT_R8_UNORM;
        config.resourceFlags = D3D12_RESOURCE_FLAG_NONE;
        config.writeState = D3D12_RESOURCE_STATE_COMMON;
        config.publishedState = D3D12_RESOURCE_STATE_COMMON;
        config.createSrv = true;
        config.createUav = false;
        config.initialCapacity = static_cast<std::size_t>(frameCount + 2);
        config.maxCapacity = config.initialCapacity;

        opened_ = pool_.initialize(std::move(backend), config);
        if (!opened_) {
            error_ = pool_.lastError();
        }
        return opened_;
    }

    bool isOpened() const noexcept override
    {
        return opened_;
    }

    bool read(const IC4Ext::CameraReadOptions&,
              IC4Ext::D3D12::ReadOnlyFrame& outFrame,
              IC4Ext::ErrorInfo& outError) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outFrame = {};

        if (!opened_) {
            outError = IC4Ext::MakeError(
                IC4Ext::ErrorCode::NotOpened,
                "DummyReadOnlyCamera::read",
                "Dummy source is not initialized");
            error_ = outError;
            return false;
        }

        if (nextFrame_ >= frameCount_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            outError = IC4Ext::MakeError(
                IC4Ext::ErrorCode::Timeout,
                "DummyReadOnlyCamera::read",
                "No more synthetic frames");
            error_ = outError;
            return false;
        }

        auto writer = pool_.acquire();
        if (!writer) {
            outError = pool_.lastError();
            if (!outError) {
                outError = IC4Ext::MakeError(
                    IC4Ext::ErrorCode::InternalError,
                    "DummyReadOnlyCamera::read",
                    "Synthetic frame pool is exhausted");
            }
            error_ = outError;
            return false;
        }

        const std::uint64_t sequence = ++nextFrame_;

        IC4Ext::FrameTiming timing;
        timing.frameNumber = sequence;
        timing.deviceTimestampNs = sequence * 1'000'000ull;
        timing.hostReceivedTime = std::chrono::steady_clock::now();

        IC4Ext::FrameFormatMetadata format;
        format.requestedFormat = IC4Ext::CameraPixelFormat::Mono8;
        format.actualInputFormat = IC4Ext::CameraPixelFormat::Mono8;
        format.outputFormat = IC4Ext::GpuFrameFormat::R8;
        format.width = 2;
        format.height = 2;
        format.inputRowPitchBytes = 2;

        outFrame = writer.publish({}, timing, format, {});
        if (!outFrame) {
            outError = IC4Ext::MakeError(
                IC4Ext::ErrorCode::InternalError,
                "DummyReadOnlyCamera::read",
                "Failed to publish synthetic read-only frame");
            error_ = outError;
            return false;
        }

        outError = IC4Ext::NoError();
        error_ = outError;
        return true;
    }

    IC4Ext::ErrorInfo lastError() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    IC4Ext::D3D12::FramePoolStats poolStats() const
    {
        return pool_.stats();
    }

private:
    mutable std::mutex mutex_;
    IC4Ext::D3D12::FramePool pool_;
    std::uint64_t frameCount_ = 0;
    std::uint64_t nextFrame_ = 0;
    bool opened_ = false;
    IC4Ext::ErrorInfo error_;
};

} // namespace

int main()
{
    namespace Pipe = IC4Ext::D3D12;

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Core creation failed; skipping: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(
        core,
        IC4Ext::D3D12BackendQueueKind::Direct);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping\n";
        return 77;
    }

    constexpr std::uint64_t frameCount = 24;
    auto source0 = std::make_shared<DummyReadOnlyCamera>();
    auto source1 = std::make_shared<DummyReadOnlyCamera>();
    if (!source0->initialize(backend, frameCount) ||
        !source1->initialize(backend, frameCount)) {
        std::cerr << "Synthetic frame pool initialization failed; skipping\n";
        return 77;
    }

    ThreadKit::Queues::QueueOptions ingressOptions;
    ingressOptions.maxSize = 128;
    ingressOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(
        ingressOptions);

    Pipe::FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::Device;
    syncConfig.maxTimestampDiffNs = 1;
    syncConfig.maxBufferedFramesPerCamera = 32;
    syncConfig.groupTimeout = std::chrono::seconds(5);

    Pipe::FrameSyncThread syncThread(ingress, syncConfig);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = 64;
    outputOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto output = std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);

    Pipe::FrameSyncOutputConfig outputConfig;
    outputConfig.requiredCameras = {0, 1};
    outputConfig.frameRate = Pipe::FrameRateLimit::Maximum();
    outputConfig.priority = 100;
    const auto outputId = syncThread.registerOutput(output, outputConfig);
    assert(outputId != Pipe::InvalidFrameSyncOutputId);

    Pipe::CameraCaptureThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 10;
    threadOptions.stopOnReadError = false;

    Pipe::CameraCaptureThread camera0(0, source0, threadOptions);
    Pipe::CameraCaptureThread camera1(1, source1, threadOptions);
    camera0.setOutputQueue(ingress);
    camera1.setOutputQueue(ingress);

    assert(syncThread.start());
    assert(camera0.start());
    assert(camera1.start());

    std::uint64_t received = 0;
    std::uint64_t previousGroup = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(10);

    while (received < frameCount &&
           std::chrono::steady_clock::now() < deadline) {
        auto frameSet = output->waitPopFor(std::chrono::milliseconds(250));
        if (!frameSet) continue;

        assert(frameSet->size() == 2);
        assert(frameSet->syncGroupId() > previousGroup);
        previousGroup = frameSet->syncGroupId();

        const auto* frame0 = frameSet->find(0);
        const auto* frame1 = frameSet->find(1);
        assert(frame0 && frame1);
        assert(*frame0);
        assert(*frame1);
        assert(frame0->timing().deviceTimestampNs ==
               frame1->timing().deviceTimestampNs);
        ++received;
    }

    camera0.stopAndJoin();
    camera1.stopAndJoin();
    syncThread.stopAndJoin();

    assert(received == frameCount);

    const auto camera0Stats = camera0.stats();
    const auto camera1Stats = camera1.stats();
    assert(camera0Stats.readFrames == frameCount);
    assert(camera1Stats.readFrames == frameCount);
    assert(camera0Stats.pushedFrames == frameCount);
    assert(camera1Stats.pushedFrames == frameCount);
    assert(camera0Stats.pushFailures == 0);
    assert(camera1Stats.pushFailures == 0);

    const auto syncStats = syncThread.stats();
    assert(syncStats.completedSets == frameCount);
    assert(syncStats.droppedFrames == 0);
    assert(syncStats.totalOutputSets == frameCount);
    assert(syncStats.totalOutputQueueDrops == 0);

    const auto perOutput = syncThread.outputStats(outputId);
    assert(perOutput.has_value());
    assert(perOutput->emittedSets == frameCount);
    assert(perOutput->queueDrops == 0);

    const auto pool0 = source0->poolStats();
    const auto pool1 = source1->poolStats();
    assert(pool0.published == 0);
    assert(pool1.published == 0);
    assert(pool0.available == pool0.capacity);
    assert(pool1.available == pool1.capacity);

    std::cout << "test_d3d12_dummy_capture_sync_integration passed\n";
    return 0;
}
