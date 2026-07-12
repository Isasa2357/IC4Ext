#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <IC4Ext/D3D12/D3D12CameraFrame.hpp>
#include <IC4Ext/D3D12/D3D12FenceManager.hpp>
#include <IC4Ext/D3D12/D3D12FrameConverter.hpp>
#include <IC4Ext/D3D12/D3D12FrameReadback.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <cassert>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

IC4Ext::D3D12CameraFrame MakeReadbackFrame(
    const IC4Ext::D3D12::ReadOnlyFrame& frame)
{
    IC4Ext::D3D12CameraFrame legacy;
    legacy.texture = frame.resource();
    legacy.srvHeap = frame.descriptorHeap();
    legacy.srvCpuHandle = frame.srvCpuHandle();
    legacy.srvGpuHandle = frame.srvGpuHandle();
    legacy.dxgiFormat = frame.dxgiFormat();
    legacy.resourceState = frame.publishedState();
    legacy.ready = frame.readyToken();
    legacy.timing = frame.timing();
    legacy.format = frame.format();
    legacy.chunkMetadata = frame.chunkMetadata();
    return legacy;
}

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

    IC4Ext::D3D12FenceManager fenceManager;
    assert(fenceManager.initialize(backend));

    IC4Ext::D3D12FrameConverter baseConverter;
    assert(baseConverter.initialize(backend, &fenceManager));

    Pipe::PooledFrameConverter converter;
    assert(converter.initialize(baseConverter));

    constexpr std::uint32_t width = 8;
    constexpr std::uint32_t height = 8;
    constexpr std::size_t bytes = width * height;

    Pipe::FramePoolConfig poolConfig;
    poolConfig.width = width;
    poolConfig.height = height;
    poolConfig.format = DXGI_FORMAT_R8_UNORM;
    poolConfig.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    poolConfig.writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    poolConfig.publishedState = D3D12_RESOURCE_STATE_GENERIC_READ;
    poolConfig.createSrv = true;
    poolConfig.createUav = true;
    poolConfig.initialCapacity = 1;
    poolConfig.maxCapacity = 1;

    Pipe::FramePool pool;
    assert(pool.initialize(backend, poolConfig));

    std::vector<std::uint8_t> pixels(bytes);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint8_t>((i * 17u) & 0xffu);
    }

    IC4Ext::FrameOutputSpec outputSpec;
    outputSpec.outputFormat = IC4Ext::GpuFrameFormat::R8;
    outputSpec.createSrv = true;
    outputSpec.createUav = true;

    IC4Ext::D3D12FrameReadback readback;
    assert(readback.initialize(backend));

    for (std::uint64_t iteration = 0; iteration < 8; ++iteration) {
        IC4Ext::D3D12CpuFrameView input;
        input.data = pixels.data();
        input.dataSize = pixels.size();
        input.timing.frameNumber = iteration + 1;
        input.timing.deviceTimestampNs = (iteration + 1) * 1'000'000ull;
        input.timing.hostReceivedTime = std::chrono::steady_clock::now();
        input.format.requestedFormat = IC4Ext::CameraPixelFormat::Mono8;
        input.format.actualInputFormat = IC4Ext::CameraPixelFormat::Mono8;
        input.format.outputFormat = IC4Ext::GpuFrameFormat::R8;
        input.format.width = static_cast<int>(width);
        input.format.height = static_cast<int>(height);
        input.format.inputRowPitchBytes = width;

        auto writer = pool.acquire();
        assert(writer);

        Pipe::ReadOnlyFrame frame;
        assert(converter.convert(
            input,
            outputSpec,
            std::move(writer),
            {},
            frame));
        assert(frame);
        assert(frame.readyToken().wait(5000));

        const auto description = frame.resource()->GetDesc();
        assert(description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        assert(description.Width == width);
        assert(description.Height == height);
        assert(description.Format == DXGI_FORMAT_R8_UNORM);

        if (iteration == 0 || iteration == 7) {
            auto legacy = MakeReadbackFrame(frame);
            IC4Ext::CpuFrame cpu;
            assert(readback.readback(
                legacy,
                IC4Ext::CpuFrameFormat::Gray8,
                cpu,
                5000));
            assert(cpu.width == width);
            assert(cpu.height == height);
            assert(cpu.rowPitch == width);
            assert(cpu.data == pixels);
        }

        frame = {};
        const auto poolStats = pool.stats();
        assert(poolStats.capacity == 1);
        assert(poolStats.available == 1);
        assert(poolStats.writing == 0);
        assert(poolStats.published == 0);
    }

    const auto stats = converter.stats();
    assert(stats.conversions == 8);
    assert(stats.inputBufferAllocations == 4);
    assert(stats.inputBufferReuses == 4);
    assert(stats.cachedInputBufferCount == 4);
    assert(stats.cachedInputBufferBytes == 4 * bytes);

    std::cout << "test_d3d12_pooled_converter_device passed\n";
    return 0;
}
