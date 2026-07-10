#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

int EnvInt(const char* name, int fallback)
{
    if (const char* value = std::getenv(name)) {
        const int parsed = std::atoi(value);
        return parsed > 0 ? parsed : fallback;
    }
    return fallback;
}

#if IC4EXT_ENABLE_D3D11
std::shared_ptr<IC4Ext::D3D11IndexedFrameQueue> MakeD3D11InputQueue(std::size_t maxSize)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = maxSize;
    return std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(options);
}

std::shared_ptr<IC4Ext::D3D11SyncedFrameQueue> MakeD3D11OutputQueue(std::size_t maxSize)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = maxSize;
    return std::make_shared<IC4Ext::D3D11SyncedFrameQueue>(options);
}

IC4Ext::D3D11IndexedCameraFrame MakeD3D11Frame(std::uint32_t cameraIndex,
                                               std::uint64_t frameNumber,
                                               std::uint64_t timestampNs)
{
    IC4Ext::D3D11IndexedCameraFrame frame;
    frame.cameraIndex = cameraIndex;
    frame.frame.timing.frameNumber = frameNumber;
    frame.frame.timing.deviceTimestampNs = timestampNs;
    frame.frame.format.width = 2;
    frame.frame.format.height = 2;
    return frame;
}

void TestD3D11PassThroughStress(int frameCount)
{
    auto input = MakeD3D11InputQueue(static_cast<std::size_t>(frameCount) * 2u + 64u);
    auto output = MakeD3D11OutputQueue(static_cast<std::size_t>(frameCount) + 64u);

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::PassThroughSingleCamera;
    options.cameraIndices = {2};

    IC4Ext::D3D11FrameSyncThread sync(input, output, options);
    assert(sync.start());

    for (int i = 0; i < frameCount; ++i) {
        input->push(MakeD3D11Frame(1, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i) * 1000ull));
        input->push(MakeD3D11Frame(2, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i) * 1000ull + 10ull));
    }

    int emitted = 0;
    while (emitted < frameCount) {
        auto set = output->waitPopFor(std::chrono::milliseconds(5000));
        assert(set.has_value());
        assert(set->frames.size() == 1);
        assert(set->frames[0].cameraIndex == 2);
        ++emitted;
    }

    sync.stopAndJoin();
    const auto stats = sync.stats();
    assert(stats.inputFrames >= static_cast<std::uint64_t>(frameCount));
    assert(stats.emittedSets >= static_cast<std::uint64_t>(frameCount));
    assert(stats.ignoredFrames >= static_cast<std::uint64_t>(frameCount));
}
#endif

#if IC4EXT_ENABLE_D3D12
std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue> MakeD3D12InputQueue(std::size_t maxSize)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = maxSize;
    return std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(options);
}

std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> MakeD3D12OutputQueue(std::size_t maxSize)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = maxSize;
    return std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(options);
}

IC4Ext::D3D12IndexedCameraFrame MakeD3D12Frame(std::uint32_t cameraIndex,
                                               std::uint64_t frameNumber,
                                               std::uint64_t timestampNs)
{
    IC4Ext::D3D12IndexedCameraFrame frame;
    frame.cameraIndex = cameraIndex;
    frame.frame.timing.frameNumber = frameNumber;
    frame.frame.timing.deviceTimestampNs = timestampNs;
    frame.frame.format.width = 2;
    frame.frame.format.height = 2;
    return frame;
}

void DrainD3D12Sets(const std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue>& output,
                    int expectedSets,
                    std::size_t expectedFrames)
{
    int emitted = 0;
    while (emitted < expectedSets) {
        auto set = output->waitPopFor(std::chrono::milliseconds(5000));
        assert(set.has_value());
        assert(set->frames.size() == expectedFrames);
        ++emitted;
    }
}

void TestD3D12FrameNumberExactStress(int frameCount)
{
    auto input = MakeD3D12InputQueue(static_cast<std::size_t>(frameCount) * 2u + 64u);
    auto output = MakeD3D12OutputQueue(static_cast<std::size_t>(frameCount) + 64u);

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::FrameNumberExact;
    options.cameraIndices = {0, 1};
    options.maxBufferedFramesPerCamera = static_cast<std::uint32_t>(frameCount + 16);

    IC4Ext::D3D12FrameSyncThread sync(input, output, options);
    assert(sync.start());

    for (int i = 1; i <= frameCount; ++i) {
        input->push(MakeD3D12Frame(0, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i) * 1000000ull));
        input->push(MakeD3D12Frame(1, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i) * 1000000ull + 1000ull));
    }

    DrainD3D12Sets(output, frameCount, 2);
    sync.stopAndJoin();

    const auto stats = sync.stats();
    assert(stats.inputFrames >= static_cast<std::uint64_t>(frameCount * 2));
    assert(stats.emittedSets >= static_cast<std::uint64_t>(frameCount));
}

void TestD3D12TimestampNearestStress(int frameCount)
{
    auto input = MakeD3D12InputQueue(static_cast<std::size_t>(frameCount) * 2u + 64u);
    auto output = MakeD3D12OutputQueue(static_cast<std::size_t>(frameCount) + 64u);

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    options.cameraIndices = {0, 1};
    options.maxTimestampDiffNs = 50000;
    options.maxBufferedFramesPerCamera = static_cast<std::uint32_t>(frameCount + 16);

    IC4Ext::D3D12FrameSyncThread sync(input, output, options);
    assert(sync.start());

    for (int i = 1; i <= frameCount; ++i) {
        const std::uint64_t base = static_cast<std::uint64_t>(i) * 1000000ull;
        input->push(MakeD3D12Frame(0, static_cast<std::uint64_t>(i), base));
        input->push(MakeD3D12Frame(1, static_cast<std::uint64_t>(i), base + 10000ull));
    }

    DrainD3D12Sets(output, frameCount, 2);
    sync.stopAndJoin();

    const auto stats = sync.stats();
    assert(stats.inputFrames >= static_cast<std::uint64_t>(frameCount * 2));
    assert(stats.emittedSets >= static_cast<std::uint64_t>(frameCount));
}

void TestD3D12RestartStress()
{
    for (int i = 0; i < 5; ++i) {
        auto input = MakeD3D12InputQueue(8);
        auto output = MakeD3D12OutputQueue(8);

        IC4Ext::FrameSyncOptions options;
        options.policy = IC4Ext::FrameSyncPolicy::PassThroughSingleCamera;
        options.cameraIndices = {0};

        IC4Ext::D3D12FrameSyncThread sync(input, output, options);
        assert(sync.start());
        input->push(MakeD3D12Frame(0, static_cast<std::uint64_t>(i), static_cast<std::uint64_t>(i) * 1000ull));
        auto set = output->waitPopFor(std::chrono::milliseconds(1000));
        assert(set.has_value());
        sync.stopAndJoin();
        assert(sync.stats().emittedSets == 1);
    }
}
#endif

} // namespace

int main()
{
    const int frameCount = EnvInt("IC4EXT_TEST_SYNTH_STRESS_FRAMES", 1000);
    assert(frameCount > 0);

#if IC4EXT_ENABLE_D3D11
    TestD3D11PassThroughStress(frameCount);
#endif

#if IC4EXT_ENABLE_D3D12
    TestD3D12FrameNumberExactStress(frameCount);
    TestD3D12TimestampNearestStress(frameCount);
    TestD3D12RestartStress();
#endif

    std::cout << "test_no_camera_pipeline_stress passed frames=" << frameCount << "\n";
    return 0;
}
