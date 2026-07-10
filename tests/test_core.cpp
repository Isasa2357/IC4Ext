#include <IC4Ext/IC4Ext.hpp>
#include <cassert>
#include <iostream>

int main()
{
    assert(IC4Ext::BytesPerPixel(IC4Ext::CameraPixelFormat::Mono8) == 1);
    assert(IC4Ext::BytesPerPixel(IC4Ext::CameraPixelFormat::BGR8) == 3);
    assert(IC4Ext::BytesPerPixel(IC4Ext::CameraPixelFormat::BGRa8) == 4);
    assert(IC4Ext::IsSupportedConversion(IC4Ext::CameraPixelFormat::Mono8, IC4Ext::GpuFrameFormat::R8));
    assert(IC4Ext::IsSupportedConversion(IC4Ext::CameraPixelFormat::BayerRG8, IC4Ext::GpuFrameFormat::RGBA8));
    assert(!IC4Ext::IsSupportedConversion(IC4Ext::CameraPixelFormat::BGR8, IC4Ext::GpuFrameFormat::R8));

    IC4Ext::CameraPixelFormat parsed{};
    assert(IC4Ext::ParseCameraPixelFormat("BayerRG8", parsed));
    assert(parsed == IC4Ext::CameraPixelFormat::BayerRG8);
    assert(IC4Ext::ParseCameraPixelFormat("BGRa8", parsed));
    assert(parsed == IC4Ext::CameraPixelFormat::BGRa8);
    assert(!IC4Ext::ParseCameraPixelFormat("BayerRG10p", parsed));

    IC4Ext::CameraCaptureConfig cfg;
    cfg.streamRequest.offsetX = 10;
    cfg.streamRequest.offsetY = 20;
    cfg.propertyOverrides.push_back(IC4Ext::IC4PropertyOverride{"ExposureAuto", std::string("Off")});
    cfg.propertyOverrides.push_back(IC4Ext::IC4PropertyOverride{"ExposureTime", 2000.0});
    assert(cfg.streamRequest.offsetX.value() == 10);
    assert(cfg.propertyOverrides.size() == 2);

#if IC4EXT_ENABLE_D3D11
    IC4Ext::D3D11ReadyToken invalid;
    assert(!invalid.isValid());
    assert(!invalid.isReady());
    assert(!invalid.wait(0));
#endif

    std::cout << "test_core passed\n";
    return 0;
}
