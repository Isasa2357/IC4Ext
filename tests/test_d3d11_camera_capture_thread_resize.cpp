#include <IC4Ext/IC4Ext.hpp>
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

template <class Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

IC4Ext::D3D11CameraFrame MakeFrame(D3D11CoreLib::D3D11Core& core)
{
    constexpr UINT width = 4;
    constexpr UINT height = 4;
    std::vector<std::uint8_t> pixels(width * height * 4u, 0);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = static_cast<std::uint8_t>(i);
        pixels[i + 1] = static_cast<std::uint8_t>(255u - i);
        pixels[i + 2] = 64;
        pixels[i + 3] = 255;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels.data();
    init.SysMemPitch = width * 4u;

    IC4Ext::D3D11CameraFrame frame;
    const HRESULT hr = core.GetDevice()->CreateTexture2D(
        &desc, &init, frame.texture.GetAddressOf());
    if (FAILED(hr)) return {};
    frame.timing.frameNumber = 42;
    frame.timing.deviceTimestampNs = 123456;
    frame.format.width = width;
    frame.format.height = height;
    frame.format.actualInputFormat = IC4Ext::CameraPixelFormat::BGRa8;
    frame.format.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    frame.chunkMetadata.hasGain = true;
    frame.chunkMetadata.gain = 3.5;
    return frame;
}

void CheckDimensions(const IC4Ext::D3D11IndexedCameraFrame& indexed,
                     std::uint32_t width,
                     std::uint32_t height,
                     std::uint32_t cameraIndex)
{
    assert(indexed.cameraIndex == cameraIndex);
    assert(indexed.frame.texture);
    D3D11_TEXTURE2D_DESC desc{};
    indexed.frame.texture->GetDesc(&desc);
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
    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        D3D11CoreLib::D3D11CoreConfig config;
        config.enableDebugLayer = false;
        config.enableInfoQueue = false;
        core = D3D11CoreLib::D3D11Core::CreateShared(config);
    } catch (const std::exception& e) {
        std::cerr << "D3D11Core creation failed; skipping: " << e.what() << '\n';
        return 77;
    }

    ThreadKit::Queues::QueueOptions options;
    options.maxSize = 8;
    auto sourceQueue = std::make_shared<IC4Ext::D3D11FrameQueue>(options);
    auto source = std::make_shared<IC4Ext::D3D11DummyCameraCapture>(
        0, sourceQueue, std::weak_ptr<IC4Ext::ID3D11CameraControlSink>{});

    IC4Ext::CameraThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 20;
    IC4Ext::D3D11CameraCaptureThread captureThread(source, core.get(), threadOptions);

    auto passthrough = std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(options);
    auto resizedA = std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(options);
    auto resizedB = std::make_shared<IC4Ext::D3D11IndexedFrameQueue>(options);

    captureThread.addOutputQueue(0, passthrough);
    captureThread.addOutputQueue(
        1, resizedA,
        IC4Ext::CameraOutputResizeOptions{2, 3, IC4Ext::CameraOutputResizeFilter::Linear});
    captureThread.addOutputQueue(
        2, resizedB,
        IC4Ext::CameraOutputResizeOptions{3, 2, IC4Ext::CameraOutputResizeFilter::Point});

    assert(captureThread.start());
    auto frame = MakeFrame(*core);
    if (!frame.texture) {
        captureThread.stopAndJoin();
        std::cerr << "D3D11 test texture creation failed; skipping\n";
        return 77;
    }
    sourceQueue->push(std::move(frame));

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

    assert(WaitUntil(
        [&captureThread] {
            const auto stats = captureThread.stats();
            return stats.resizedFrames == 2 && stats.pushedFrames == 3;
        },
        std::chrono::seconds(1)));
    const auto stats = captureThread.stats();
    assert(stats.resizedFrames == 2);
    assert(stats.resizeFailures == 0);
    assert(stats.pushedFrames == 3);

    captureThread.stopAndJoin();
    std::cout << "test_d3d11_camera_capture_thread_resize passed\n";
    return 0;
}
