#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

namespace {

template <class Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

IC4Ext::D3D11CameraFrame MakeFrame(std::uint64_t frameNumber)
{
    IC4Ext::D3D11CameraFrame frame;
    frame.timing.frameNumber = frameNumber;
    frame.format.width = 2;
    frame.format.height = 2;
    frame.format.actualInputFormat = IC4Ext::CameraPixelFormat::Mono8;
    frame.format.outputFormat = IC4Ext::GpuFrameFormat::R8;
    return frame;
}

} // namespace

int main()
{
    ThreadKit::Queues::QueueOptions queueOptions;
    queueOptions.maxSize = 8;

    auto sourceQueue = std::make_shared<IC4Ext::D3D11FrameQueue>(queueOptions);
    auto source = std::make_shared<IC4Ext::D3D11DummyCameraCapture>(
        0,
        sourceQueue,
        std::weak_ptr<IC4Ext::ID3D11CameraControlSink>{});

    IC4Ext::CameraThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 20;

    IC4Ext::D3D11CameraCaptureThread captureThread(source, threadOptions);
    assert(captureThread.start());
    assert(captureThread.outputQueueCount() == 0);

    auto output1 = std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(queueOptions);

    // The same binding may be registered more than once. Removal deletes all
    // matching cameraIndex + queue registrations.
    captureThread.addOutputQueue(7, output1);
    captureThread.addOutputQueue(7, output1);
    assert(captureThread.outputQueueCount() == 2);
    assert(captureThread.removeOutputQueue(7, output1) == 2);
    assert(captureThread.outputQueueCount() == 0);
    assert(captureThread.removeOutputQueue(7, output1) == 0);

    // Register after the worker has already started.
    captureThread.addOutputQueue(7, output1);
    assert(captureThread.outputQueueCount() == 1);

    sourceQueue->push(MakeFrame(1));
    auto received1 = output1->waitPopFor(std::chrono::seconds(1));
    assert(received1.has_value());
    assert(received1->cameraIndex == 7);
    assert(received1->frame.timing.frameNumber == 1);

    // Remove while the worker is running and verify later frames are not sent.
    assert(captureThread.removeOutputQueue(7, output1) == 1);
    assert(captureThread.outputQueueCount() == 0);

    sourceQueue->push(MakeFrame(2));
    assert(WaitUntil(
        [&captureThread] { return captureThread.stats().readFrames >= 2; },
        std::chrono::seconds(1)));
    assert(!output1->tryPop().has_value());

    auto output2 = std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(queueOptions);
    captureThread.addOutputQueue(11, output2);
    assert(captureThread.outputQueueCount() == 1);

    sourceQueue->push(MakeFrame(3));
    auto received2 = output2->waitPopFor(std::chrono::seconds(1));
    assert(received2.has_value());
    assert(received2->cameraIndex == 11);
    assert(received2->frame.timing.frameNumber == 3);

    assert(captureThread.clearOutputQueues() == 1);
    assert(captureThread.outputQueueCount() == 0);

    sourceQueue->push(MakeFrame(4));
    assert(WaitUntil(
        [&captureThread] { return captureThread.stats().readFrames >= 4; },
        std::chrono::seconds(1)));
    assert(!output2->tryPop().has_value());

    captureThread.stopAndJoin();

    std::cout << "test_d3d11_camera_capture_thread_outputs passed\n";
    return 0;
}
