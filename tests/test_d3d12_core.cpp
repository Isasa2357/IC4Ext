#include <IC4Ext/IC4Ext.hpp>

#include <cassert>
#include <iostream>

int main()
{
    IC4Ext::D3D12ReadyToken invalidToken;
    assert(!invalidToken.isValid());
    assert(!invalidToken.isReady());
    assert(!invalidToken.wait(0));

    IC4Ext::D3D12FrameConverter converter;
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::Mono8, IC4Ext::GpuFrameFormat::R8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::Mono8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::BayerRG8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::BayerGR8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::BayerGB8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::BayerBG8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::BGR8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(converter.isSupported(IC4Ext::CameraPixelFormat::BGRa8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(!converter.isSupported(IC4Ext::CameraPixelFormat::BGR8, IC4Ext::GpuFrameFormat::R8));
    assert(!converter.isSupported(IC4Ext::CameraPixelFormat::BGRa8, IC4Ext::GpuFrameFormat::R8));
    assert(!converter.isSupported(IC4Ext::CameraPixelFormat::BayerRG8, IC4Ext::GpuFrameFormat::R8));

    assert(!converter.initialize(nullptr, nullptr, nullptr));
    assert(converter.lastError().code == static_cast<int>(IC4Ext::ErrorCode::InvalidArgument));

    IC4Ext::D3D12CpuFrameView view;
    view.format.actualInputFormat = IC4Ext::CameraPixelFormat::Mono8;
    view.format.outputFormat = IC4Ext::GpuFrameFormat::R8;
    view.format.width = 16;
    view.format.height = 16;
    view.format.inputRowPitchBytes = 16;
    assert(view.format.inputRowPitchBytes == static_cast<std::size_t>(view.format.width));

    std::cout << "test_d3d12_core passed\n";
    return 0;
}
