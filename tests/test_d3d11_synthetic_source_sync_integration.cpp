#include <IC4Ext/D3D11/D3D11FrameReadback.hpp>
#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Core/D3D11CoreConfig.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>

namespace {

std::uint64_t Checksum(const IC4Ext::CpuFrame& frame)
{
    std::uint64_t value = 1469598103934665603ull;
    for (const auto byte : frame.data) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value;
}

void ValidateRgbaFrame(
    const IC4Ext::CpuFrame& frame,
    std::uint32_t width,
    std::uint32_t height)
{
    assert(frame.width == width);
    assert(frame.height == height);
    assert(frame.format == IC4Ext::CpuFrameFormat::RGBA8);
    assert(frame.rowPitch == width * 4u);
    assert(frame.data.size() ==
           static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t index = 3; index < frame.data.size(); index += 4) {
        assert(frame.data[index] == 255u);
    }
}

} // namespace

int main()
{
    namespace Pipe = IC4Ext::D3D11;
    using Clock = std::chrono::steady_clock;

    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        D3D11CoreLib::D3D11CoreConfig coreConfig;
        coreConfig.enableDebugLayer = false;
        coreConfig.enableInfoQueue = false;
        coreConfig.enableMultithreadProtection = true;
        coreConfig.allowWarpAdapter = true;
        core = D3D11CoreLib::D3D11Core::CreateShared(coreConfig);
    } catch (const std::exception& exception) {
        std::cerr << "D3D11Core creation failed; skipping: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D11BackendContext::FromCore(core, true);
    if (!backend.resolve()) {
        std::cerr << "D3D11 backend resolve failed; skipping\n";
        return 77;
    }

    constexpr std::uint32_t width = 96;
    constexpr std::uint32_t height = 64;
    constexpr double fps = 120.0;
    constexpr std::uint64_t frameCount = 24;
    constexpr std::int64_t timestampOffsetNs = 500'000;

    Pipe::SyntheticFrameSourceConfig sourceConfig0;
    sourceConfig0.width = width;
    sourceConfig0.height = height;
    sourceConfig0.fps = fps;
    sourceConfig0.pattern = Pipe::SyntheticFramePattern::HashNoise;
    sourceConfig0.seed = 0x0123456789abcdefull;
    sourceConfig0.deviceTimestampOriginNs = 2'000'000'000ull;
    sourceConfig0.deviceTimestampOffsetNs = 0;
    sourceConfig0.frameLimit = frameCount;
    sourceConfig0.initialFramePoolCapacity = frameCount + 4;
    sourceConfig0.maxFramePoolCapacity = frameCount + 4;

    auto sourceConfig1 = sourceConfig0;
    sourceConfig1.seed = 0xfedcba9876543210ull;
    sourceConfig1.deviceTimestampOffsetNs = timestampOffsetNs;

    assert(sourceConfig0.isValid());
    assert(sourceConfig1.isValid());
    const auto framePeriodNs = sourceConfig0.framePeriodNs();
    assert(framePeriodNs > 0);

    auto source0 = std::make_shared<Pipe::SyntheticFrameSource>();
    auto source1 = std::make_shared<Pipe::SyntheticFrameSource>();
    if (!source0->initialize(backend, sourceConfig0)) {
        const auto error = source0->lastError();
        std::cerr << "source0 initialization failed: "
                  << error.where << ": " << error.message << '\n';
        return error.code == static_cast<int>(IC4Ext::ErrorCode::D3D11Error)
            ? 77
            : 1;
    }
    if (!source1->initialize(backend, sourceConfig1)) {
        const auto error = source1->lastError();
        std::cerr << "source1 initialization failed: "
                  << error.where << ": " << error.message << '\n';
        return error.code == static_cast<int>(IC4Ext::ErrorCode::D3D11Error)
            ? 77
            : 1;
    }

    ThreadKit::Queues::QueueOptions ingressOptions;
    ingressOptions.maxSize = 128;
    ingressOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::RejectNew;
    auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(
        ingressOptions);

    Pipe::FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::Device;
    syncConfig.maxTimestampDiffNs =
        static_cast<std::uint64_t>(timestampOffsetNs) + 1u;
    syncConfig.maxBufferedFramesPerCamera = 32;
    syncConfig.groupTimeout = std::chrono::seconds(5);
    assert(syncConfig.isValid());

    Pipe::FrameSyncThread syncThread(ingress, syncConfig);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = 64;
    outputOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::RejectNew;
    auto output = std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);

    Pipe::FrameSyncOutputConfig outputConfig;
    outputConfig.requiredCameras = {0, 1};
    outputConfig.frameRate = Pipe::FrameRateLimit::Maximum();
    outputConfig.priority = 100;
    const auto outputId = syncThread.registerOutput(output, outputConfig);
    assert(outputId != Pipe::InvalidFrameSyncOutputId);

    Pipe::CameraCaptureThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 20;
    threadOptions.stopOnReadError = false;

    Pipe::CameraCaptureThread camera0(0, source0, threadOptions);
    Pipe::CameraCaptureThread camera1(1, source1, threadOptions);
    camera0.setOutputQueue(ingress);
    camera1.setOutputQueue(ingress);

    IC4Ext::D3D11FrameReadback readback;
    if (!readback.initialize(core.get())) {
        const auto error = readback.lastError();
        std::cerr << "readback initialization failed: "
                  << error.where << ": " << error.message << '\n';
        return 1;
    }

    assert(syncThread.start());
    const auto startTime = Clock::now();
    assert(camera0.start());
    assert(camera1.start());

    std::uint64_t received = 0;
    std::uint64_t previousTimestamp0 = 0;
    std::uint64_t previousTimestamp1 = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(10);

    while (received < frameCount && Clock::now() < deadline) {
        auto frameSet = output->waitPopFor(std::chrono::milliseconds(250));
        if (!frameSet) continue;

        assert(frameSet->size() == 2);
        const auto* frame0 = frameSet->find(0);
        const auto* frame1 = frameSet->find(1);
        assert(frame0 && frame1);
        assert(frame0->hasResource() && frame1->hasResource());
        assert(frame0->hasSrv() && frame1->hasSrv());
        assert(frame0->dxgiFormat() == DXGI_FORMAT_R8G8B8A8_UNORM);
        assert(frame1->dxgiFormat() == DXGI_FORMAT_R8G8B8A8_UNORM);

        D3D11_TEXTURE2D_DESC description0{};
        D3D11_TEXTURE2D_DESC description1{};
        frame0->texture()->GetDesc(&description0);
        frame1->texture()->GetDesc(&description1);
        assert(description0.Width == width && description0.Height == height);
        assert(description1.Width == width && description1.Height == height);

        const auto timing0 = frame0->timing();
        const auto timing1 = frame1->timing();
        assert(timing0.frameNumber == sourceConfig0.firstFrameNumber + received);
        assert(timing1.frameNumber == sourceConfig1.firstFrameNumber + received);
        assert(timing1.deviceTimestampNs - timing0.deviceTimestampNs ==
               static_cast<std::uint64_t>(timestampOffsetNs));

        if (previousTimestamp0 != 0) {
            assert(timing0.deviceTimestampNs - previousTimestamp0 == framePeriodNs);
            assert(timing1.deviceTimestampNs - previousTimestamp1 == framePeriodNs);
        }
        previousTimestamp0 = timing0.deviceTimestampNs;
        previousTimestamp1 = timing1.deviceTimestampNs;

        if (received < 2) {
            IC4Ext::CpuFrame cpu0;
            IC4Ext::CpuFrame cpu1;
            assert(readback.readback(
                *frame0,
                IC4Ext::CpuFrameFormat::RGBA8,
                cpu0,
                5000));
            assert(readback.readback(
                *frame1,
                IC4Ext::CpuFrameFormat::RGBA8,
                cpu1,
                5000));
            ValidateRgbaFrame(cpu0, width, height);
            ValidateRgbaFrame(cpu1, width, height);
            assert(Checksum(cpu0) != 0);
            assert(Checksum(cpu1) != 0);
        }

        ++received;
    }

    const auto elapsed = Clock::now() - startTime;
    camera0.stopAndJoin();
    camera1.stopAndJoin();
    syncThread.stopAndJoin();
    ingress->close();
    output->clear();

    assert(received == frameCount);
    assert(elapsed >= std::chrono::milliseconds(100));
    assert(elapsed < std::chrono::seconds(10));

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

    const auto perOutputStats = syncThread.outputStats(outputId);
    assert(perOutputStats.has_value());
    assert(perOutputStats->emittedSets == frameCount);
    assert(perOutputStats->queueDrops == 0);

    const auto sourceStats0 = source0->stats();
    const auto sourceStats1 = source1->stats();
    assert(sourceStats0.generatedFrames == frameCount);
    assert(sourceStats1.generatedFrames == frameCount);
    assert(sourceStats0.gpuGenerationFailures == 0);
    assert(sourceStats1.gpuGenerationFailures == 0);
    assert(sourceStats0.poolAcquireFailures == 0);
    assert(sourceStats1.poolAcquireFailures == 0);

    const auto pool0 = source0->framePoolStats();
    const auto pool1 = source1->framePoolStats();
    assert(pool0.published == 0);
    assert(pool1.published == 0);
    assert(pool0.available == pool0.capacity);
    assert(pool1.available == pool1.capacity);

    std::cout << "test_d3d11_synthetic_source_sync_integration passed: "
              << received << " synchronized RGBA frames at " << fps << " fps\n";
    return 0;
}
