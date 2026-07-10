#include <IC4Ext/IC4Ext.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <cassert>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

int main()
{
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& e) {
        std::cerr << "D3D12Core creation failed; skipping: " << e.what() << "\n";
        return 77;
    }

    IC4Ext::D3D12BackendContext backend = IC4Ext::D3D12BackendContext::FromCore(core, IC4Ext::D3D12BackendQueueKind::Direct);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping\n";
        return 77;
    }

    const UINT width = 2;
    const UINT height = 2;
    const std::vector<std::uint8_t> rgba = {
        10,20,30,255,  40,50,60,255,
        70,80,90,255,  1,2,3,255,
    };

    IC4Ext::D3D12CameraFrame gpuFrame;
    try {
        gpuFrame.textureResource = D3D12CoreLib::CreateTexture2DFromMemory(*core,
                                                                            rgba.data(),
                                                                            width,
                                                                            height,
                                                                            DXGI_FORMAT_R8G8B8A8_UNORM,
                                                                            width * 4,
                                                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } catch (const std::exception& e) {
        std::cerr << "CreateTexture2DFromMemory failed; skipping: " << e.what() << "\n";
        return 77;
    }
    gpuFrame.texture = gpuFrame.textureResource.Get();
    gpuFrame.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpuFrame.resourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    gpuFrame.format.width = static_cast<int>(width);
    gpuFrame.format.height = static_cast<int>(height);
    gpuFrame.format.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    gpuFrame.timing.frameNumber = 123;
    gpuFrame.timing.deviceTimestampNs = 456;
    gpuFrame.chunkMetadata.hasExposureTime = true;
    gpuFrame.chunkMetadata.exposureTimeUs = 2000.0;
    gpuFrame.chunkMetadata.hasGain = true;
    gpuFrame.chunkMetadata.gain = 12.0;

    IC4Ext::D3D12FrameReadback readback;
    assert(readback.initialize(backend));

    IC4Ext::CpuFrame cpu;
    assert(readback.readback(gpuFrame, IC4Ext::CpuFrameFormat::BGR8, cpu, 5000));
    assert(cpu.width == width);
    assert(cpu.height == height);
    assert(cpu.rowPitch == width * 3);
    assert(cpu.timing.frameNumber == 123);
    assert(cpu.chunkMetadata.hasExposureTime && cpu.chunkMetadata.exposureTimeUs == 2000.0);
    assert(cpu.chunkMetadata.hasGain && cpu.chunkMetadata.gain == 12.0);
    assert((cpu.data == std::vector<std::uint8_t>{30,20,10, 60,50,40, 90,80,70, 3,2,1}));

    auto stats = readback.cacheStats();
    assert(stats.readbacks == 1);
    assert(stats.cacheMisses == 1);
    assert(stats.cacheHits == 0);
    assert(stats.resourceRebuilds == 1);
    assert(stats.bytesAllocated >= width * height * 4);

    assert(readback.readback(gpuFrame, IC4Ext::CpuFrameFormat::Gray8, cpu, 5000));
    assert(cpu.rowPitch == width);
    assert(cpu.chunkMetadata.hasExposureTime && cpu.chunkMetadata.exposureTimeUs == 2000.0);
    assert(cpu.data[0] == static_cast<std::uint8_t>((77u * 10u + 150u * 20u + 29u * 30u + 128u) >> 8u));

    stats = readback.cacheStats();
    assert(stats.readbacks == 2);
    assert(stats.cacheMisses == 1);
    assert(stats.cacheHits == 1);
    assert(stats.resourceRebuilds == 1);

    readback.resetCache();
    stats = readback.cacheStats();
    assert(stats.readbacks == 0);
    assert(stats.cacheHits == 0);
    assert(stats.cacheMisses == 0);
    assert(stats.resourceRebuilds == 0);

    std::cout << "test_d3d12_frame_readback passed\n";
    return 0;
}
