#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>

namespace {

std::shared_ptr<IC4Ext::D3D11IndexedFrameQueue> MakeInputQueue()
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = 32;
    return std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(options);
}

std::shared_ptr<IC4Ext::D3D11SyncedFrameQueue> MakeOutputQueue()
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = 32;
    return std::make_shared<IC4Ext::D3D11SyncedFrameQueue>(options);
}

IC4Ext::D3D11IndexedCameraFrame MakeFrame(std::uint32_t cameraIndex,
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

void TestPassThrough()
{
    auto input = MakeInputQueue();
    auto output = MakeOutputQueue();

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::PassThroughSingleCamera;
    options.cameraIndices = {2};

    IC4Ext::D3D11FrameSyncThread sync(input, output, options);
    assert(sync.start());

    input->push(MakeFrame(1, 10, 1000));
    input->push(MakeFrame(2, 11, 1100));

    auto set = output->waitPopFor(std::chrono::milliseconds(1000));
    assert(set.has_value());
    assert(set->frames.size() == 1);
    assert(set->frames[0].cameraIndex == 2);
    assert(set->frames[0].frame.timing.frameNumber == 11);
    assert(set->syncGroupId == 1);

    sync.stopAndJoin();
    const auto stats = sync.stats();
    assert(stats.inputFrames >= 2);
    assert(stats.ignoredFrames >= 1);
    assert(stats.emittedSets >= 1);
}

void TestInvalidOptions()
{
    auto input = MakeInputQueue();
    auto output = MakeOutputQueue();

    IC4Ext::FrameSyncOptions unsupported;
    unsupported.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    unsupported.cameraIndices = {0, 1};
    IC4Ext::D3D11FrameSyncThread sync(input, output, unsupported);
    assert(!sync.start());
    assert(sync.lastError().code == static_cast<int>(IC4Ext::ErrorCode::InvalidArgument));
}

} // namespace

int main()
{
    TestPassThrough();
    TestInvalidOptions();

    std::cout << "test_d3d11_frame_sync_thread passed\n";
    return 0;
}
