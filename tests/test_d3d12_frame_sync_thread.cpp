#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>

namespace {

std::shared_ptr<IC4Ext::D3D12IndexedFrameQueue> MakeInputQueue()
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = 32;
    return std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(options);
}

std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> MakeOutputQueue()
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = 32;
    return std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(options);
}

IC4Ext::D3D12IndexedCameraFrame MakeFrame(std::uint32_t cameraIndex,
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

void TestPassThrough()
{
    auto input = MakeInputQueue();
    auto output = MakeOutputQueue();

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::PassThroughSingleCamera;
    options.cameraIndices = {2};

    IC4Ext::D3D12FrameSyncThread sync(input, output, options);
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

void TestFrameNumberExact()
{
    auto input = MakeInputQueue();
    auto output = MakeOutputQueue();

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::FrameNumberExact;
    options.cameraIndices = {0, 1};
    options.maxBufferedFramesPerCamera = 4;

    IC4Ext::D3D12FrameSyncThread sync(input, output, options);
    assert(sync.start());

    input->push(MakeFrame(0, 1, 1000));
    input->push(MakeFrame(1, 1, 1010));

    auto set1 = output->waitPopFor(std::chrono::milliseconds(1000));
    assert(set1.has_value());
    assert(set1->frames.size() == 2);
    assert(set1->frames[0].cameraIndex == 0);
    assert(set1->frames[1].cameraIndex == 1);
    assert(set1->frames[0].frame.timing.frameNumber == 1);
    assert(set1->frames[1].frame.timing.frameNumber == 1);

    input->push(MakeFrame(1, 3, 3000));
    input->push(MakeFrame(0, 2, 2000));
    input->push(MakeFrame(0, 3, 3010));

    auto set2 = output->waitPopFor(std::chrono::milliseconds(1000));
    assert(set2.has_value());
    assert(set2->frames.size() == 2);
    assert(set2->frames[0].frame.timing.frameNumber == 3);
    assert(set2->frames[1].frame.timing.frameNumber == 3);

    sync.stopAndJoin();
    const auto stats = sync.stats();
    assert(stats.emittedSets >= 2);
    assert(stats.droppedFrames >= 1);
}

void TestTimestampNearest()
{
    auto input = MakeInputQueue();
    auto output = MakeOutputQueue();

    IC4Ext::FrameSyncOptions options;
    options.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    options.cameraIndices = {0, 1};
    options.maxTimestampDiffNs = 100;
    options.maxBufferedFramesPerCamera = 4;

    IC4Ext::D3D12FrameSyncThread sync(input, output, options);
    assert(sync.start());

    input->push(MakeFrame(0, 1, 1000));
    input->push(MakeFrame(1, 1, 1300));
    input->push(MakeFrame(0, 2, 1250));

    auto set = output->waitPopFor(std::chrono::milliseconds(1000));
    assert(set.has_value());
    assert(set->frames.size() == 2);
    assert(set->frames[0].cameraIndex == 0);
    assert(set->frames[1].cameraIndex == 1);
    assert(set->frames[0].frame.timing.frameNumber == 2);
    assert(set->frames[1].frame.timing.frameNumber == 1);

    sync.stopAndJoin();
    const auto stats = sync.stats();
    assert(stats.emittedSets >= 1);
    assert(stats.droppedFrames >= 1);
}

void TestInvalidOptions()
{
    auto input = MakeInputQueue();
    auto output = MakeOutputQueue();

    IC4Ext::FrameSyncOptions duplicate;
    duplicate.policy = IC4Ext::FrameSyncPolicy::FrameNumberExact;
    duplicate.cameraIndices = {0, 0};
    IC4Ext::D3D12FrameSyncThread dupSync(input, output, duplicate);
    assert(!dupSync.start());
    assert(dupSync.lastError().code == static_cast<int>(IC4Ext::ErrorCode::InvalidArgument));

    IC4Ext::FrameSyncOptions oneCamera;
    oneCamera.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    oneCamera.cameraIndices = {0};
    IC4Ext::D3D12FrameSyncThread oneSync(input, output, oneCamera);
    assert(!oneSync.start());
    assert(oneSync.lastError().code == static_cast<int>(IC4Ext::ErrorCode::InvalidArgument));
}

} // namespace

int main()
{
    TestPassThrough();
    TestFrameNumberExact();
    TestTimestampNearest();
    TestInvalidOptions();

    std::cout << "test_d3d12_frame_sync_thread passed\n";
    return 0;
}
