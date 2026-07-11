#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>

namespace {

IC4Ext::D3D12CameraFrame MakeFrame(D3D12CoreLib::D3D12Core& core)
{
    IC4Ext::D3D12CameraFrame frame;
    frame.textureResource = D3D12CoreLib::CreateTexture2D(
        core,
        4,
        4,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    frame.texture = frame.textureResource.Get();
    frame.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    frame.resourceState = D3D12_RESOURCE_STATE_COMMON;
    frame.timing.frameNumber = 42;
    frame.timing.deviceTimestampNs = 123456;
    frame.format.width = 4;
    frame.format.height = 4;
    frame.format.actualInputFormat = IC4Ext::CameraPixelFormat::BGRa8;
    frame.format.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    frame.chunkMetadata.hasGain = true;
    frame.chunkMetadata.gain = 3.5;
    return frame;
}

void CheckDimensions(const IC4Ext::D3D12IndexedCameraFrame& indexed,
                     std::uint32_t width,
                     std::uint32_t height,
                     std::uint32_t cameraIndex)
{
    assert(indexed.cameraIndex == cameraIndex);
    assert(indexed.frame.texture);
    const auto desc = indexed.frame.texture->GetDesc();
    assert(desc.Width == width);
    assert(desc.Height == height);
    assert(indexed.frame.format.width == static_cast<int>(width));
    assert(indexed.frame.format.height == static_cast<int>(height));
    assert(indexed.frame.timing.frameNumber == 42);
    assert(indexed.frame.chunkMetadata.hasGain);
    assert(indexed.frame.chunkMetadata.gain == 3.5);
}

} // namespace

int main()
{
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    IC4Ext::D3D12BackendContext backend;
    try {
        D3D12CoreLib::D3D12CoreConfig config;
        config.enableDebugLayer = false;
        config.enableInfoQueue = false;
        config.enableDred = false;
        core = D3D12CoreLib::D3D12Core::CreateShared(config);
        backend = IC4Ext::D3D12BackendContext::FromCore(
            core, IC4Ext::D3D12BackendQueueKind::Direct);
        if (!backend.resolve()) {
            std::cerr << "D3D12 backend resolution failed; skipping\n";
            return 77;
        }
    } catch (const std::exception& e) {
        std::cerr << "D3D12Core creation failed; skipping: " << e.what() << '\n';
        return 77;
    }

    ThreadKit::Queues::QueueOptions options;
    options.maxSize = 8;
    auto sourceQueue = std::make_shared<IC4Ext::D3D12FrameQueue>(options);
    auto source = std::make_shared<IC4Ext::D3D12DummyCameraCapture>(
        0, sourceQueue, std::weak_ptr<IC4Ext::ID3D12CameraControlSink>{});

    IC4Ext::CameraThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 20;
    IC4Ext::D3D12CameraCaptureThread captureThread(source, backend, threadOptions);

    auto passthrough = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(options);
    auto resizedA = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(options);
    auto resizedB = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(options);

    captureThread.addOutputQueue(0, passthrough);
    captureThread.addOutputQueue(
        1, resizedA,
        IC4Ext::CameraOutputResizeOptions{2, 3, IC4Ext::CameraOutputResizeFilter::Linear});
    captureThread.addOutputQueue(
        2, resizedB,
        IC4Ext::CameraOutputResizeOptions{3, 2, IC4Ext::CameraOutputResizeFilter::Point});

    assert(captureThread.start());
    try {
        sourceQueue->push(MakeFrame(*core));
    } catch (const std::exception& e) {
        captureThread.stopAndJoin();
        std::cerr << "D3D12 test texture creation failed; skipping: " << e.what() << '\n';
        return 77;
    }

    auto original = passthrough->waitPopFor(std::chrono::seconds(3));
    auto outputA = resizedA->waitPopFor(std::chrono::seconds(3));
    auto outputB = resizedB->waitPopFor(std::chrono::seconds(3));
    assert(original.has_value());
    assert(outputA.has_value());
    assert(outputB.has_value());

    CheckDimensions(*original, 4, 4, 0);
    CheckDimensions(*outputA, 2, 3, 1);
    CheckDimensions(*outputB, 3, 2, 2);
    assert(outputA->frame.ready.isValid());
    assert(outputB->frame.ready.isValid());
    assert(outputA->frame.ready.wait(INFINITE));
    assert(outputB->frame.ready.wait(INFINITE));

    const auto stats = captureThread.stats();
    assert(stats.resizedFrames == 2);
    assert(stats.resizeFailures == 0);
    assert(stats.pushedFrames == 3);

    captureThread.stopAndJoin();
    std::cout << "test_d3d12_camera_capture_thread_resize passed\n";
    return 0;
}
