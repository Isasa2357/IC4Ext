#include <IC4Ext/D3D11/D3D11FenceManager.hpp>
#include <IC4Ext/D3D11/D3D11FrameReadback.hpp>
#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Core/D3D11CoreConfig.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {

std::vector<std::uint8_t> ExpectedRgba(
    const std::vector<std::uint8_t>& gray)
{
    std::vector<std::uint8_t> rgba;
    rgba.reserve(gray.size() * 4u);
    for (const auto value : gray) {
        rgba.push_back(value);
        rgba.push_back(value);
        rgba.push_back(value);
        rgba.push_back(255u);
    }
    return rgba;
}

} // namespace

int main()
{
    namespace Pipe = IC4Ext::D3D11;

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

    IC4Ext::D3D11FenceManager fenceManager;
    if (!fenceManager.initialize(backend.device, backend.immediateContext)) {
        std::cerr << "D3D11 fence is unavailable; skipping\n";
        return 77;
    }

    IC4Ext::ShaderLoadConfig shaderConfig;
    shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / "d3d11";
    shaderConfig.inputKind = IC4Ext::ShaderInputKind::HlslFile;
    shaderConfig.entryPoint = "main";
    shaderConfig.target = "cs_5_0";

    IC4Ext::D3D11FrameConverter baseConverter;
    if (!baseConverter.initialize(core.get(), &fenceManager, shaderConfig)) {
        const auto error = baseConverter.lastError();
        std::cerr << "D3D11FrameConverter initialization failed: "
                  << error.where << ": " << error.message << '\n';
        return 1;
    }

    Pipe::PooledFrameConverter converter;
    assert(converter.initialize(baseConverter));

    constexpr std::uint32_t width = 8;
    constexpr std::uint32_t height = 8;
    constexpr std::size_t pixelCount = width * height;

    Pipe::FramePoolConfig poolConfig;
    poolConfig.width = width;
    poolConfig.height = height;
    poolConfig.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    poolConfig.createSrv = true;
    poolConfig.createUav = true;
    poolConfig.initialCapacity = 1;
    poolConfig.maxCapacity = 1;

    Pipe::FramePool pool;
    assert(pool.initialize(backend, poolConfig));

    std::vector<std::uint8_t> pixels(pixelCount);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<std::uint8_t>((index * 17u) & 0xffu);
    }
    const auto expected = ExpectedRgba(pixels);

    IC4Ext::FrameOutputSpec outputSpec;
    outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    outputSpec.createSrv = true;
    outputSpec.createUav = true;

    IC4Ext::D3D11FrameReadback readback;
    assert(readback.initialize(core.get()));

    for (std::uint64_t iteration = 0; iteration < 8; ++iteration) {
        IC4Ext::CpuFrameView input;
        input.data = pixels.data();
        input.dataSize = pixels.size();
        input.timing.frameNumber = iteration + 1;
        input.timing.deviceTimestampNs = (iteration + 1) * 1'000'000ull;
        input.timing.hostReceivedTime = std::chrono::steady_clock::now();
        input.format.requestedFormat = IC4Ext::CameraPixelFormat::Mono8;
        input.format.actualInputFormat = IC4Ext::CameraPixelFormat::Mono8;
        input.format.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
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
        assert(frame.waitReady(5000));

        D3D11_TEXTURE2D_DESC description{};
        frame.texture()->GetDesc(&description);
        assert(description.Width == width);
        assert(description.Height == height);
        assert(description.Format == DXGI_FORMAT_R8G8B8A8_UNORM);

        if (iteration == 0 || iteration == 7) {
            IC4Ext::CpuFrame cpu;
            assert(readback.readback(
                frame,
                IC4Ext::CpuFrameFormat::RGBA8,
                cpu,
                5000));
            assert(cpu.width == width);
            assert(cpu.height == height);
            assert(cpu.rowPitch == width * 4u);
            assert(cpu.data == expected);
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
    assert(stats.cachedInputBufferBytes == 4 * pixelCount);

    std::cout << "test_d3d11_pooled_converter_device passed\n";
    return 0;
}
