#include <IC4Ext/IC4Ext.hpp>

#include <d3d11.h>
#include <wrl/client.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   0,
                                   nullptr,
                                   0,
                                   D3D11_SDK_VERSION,
                                   device.GetAddressOf(),
                                   &level,
                                   context.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed; skipping\n";
        return 77;
    }

    const std::uint32_t width = 2;
    const std::uint32_t height = 2;
    const std::vector<std::uint8_t> rgba = {
        10,20,30,255,  40,50,60,255,
        70,80,90,255,  1,2,3,255,
    };

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba.data();
    init.SysMemPitch = width * 4;

    IC4Ext::D3D11CameraFrame gpuFrame;
    hr = device->CreateTexture2D(&desc, &init, gpuFrame.texture.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "CreateTexture2D failed; skipping\n";
        return 77;
    }
    gpuFrame.timing.frameNumber = 123;
    gpuFrame.timing.deviceTimestampNs = 456;
    gpuFrame.chunkMetadata.hasExposureTime = true;
    gpuFrame.chunkMetadata.exposureTimeUs = 2000.0;
    gpuFrame.chunkMetadata.hasGain = true;
    gpuFrame.chunkMetadata.gain = 12.0;

    IC4Ext::D3D11FrameReadback readback;
    assert(readback.initialize(device.Get(), context.Get()));

    IC4Ext::CpuFrame cpu;
    assert(readback.readback(gpuFrame, IC4Ext::CpuFrameFormat::BGR8, cpu));
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

    assert(readback.readback(gpuFrame, IC4Ext::CpuFrameFormat::RGBA8, cpu));
    assert(cpu.rowPitch == width * 4);
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

    std::cout << "test_d3d11_frame_readback passed\n";
    return 0;
}
