#include "IC4Ext/Config.hpp"

namespace IC4Ext {

const char* ToString(CameraPixelFormat fmt) noexcept
{
    switch (fmt) {
    case CameraPixelFormat::Mono8: return "Mono8";
    case CameraPixelFormat::BayerRG8: return "BayerRG8";
    case CameraPixelFormat::BayerGR8: return "BayerGR8";
    case CameraPixelFormat::BayerGB8: return "BayerGB8";
    case CameraPixelFormat::BayerBG8: return "BayerBG8";
    case CameraPixelFormat::BGR8: return "BGR8";
    case CameraPixelFormat::BGRa8: return "BGRa8";
    default: return "Unknown";
    }
}

const char* ToString(GpuFrameFormat fmt) noexcept
{
    switch (fmt) {
    case GpuFrameFormat::R8: return "R8";
    case GpuFrameFormat::RGBA8: return "RGBA8";
    default: return "Unknown";
    }
}

std::size_t BytesPerPixel(CameraPixelFormat fmt) noexcept
{
    switch (fmt) {
    case CameraPixelFormat::Mono8:
    case CameraPixelFormat::BayerRG8:
    case CameraPixelFormat::BayerGR8:
    case CameraPixelFormat::BayerGB8:
    case CameraPixelFormat::BayerBG8:
        return 1;
    case CameraPixelFormat::BGR8:
        return 3;
    case CameraPixelFormat::BGRa8:
        return 4;
    default:
        return 0;
    }
}

bool IsSupportedConversion(CameraPixelFormat input, GpuFrameFormat output) noexcept
{
    if (input == CameraPixelFormat::Mono8) {
        return output == GpuFrameFormat::R8 || output == GpuFrameFormat::RGBA8;
    }
    if (input == CameraPixelFormat::BGR8 || input == CameraPixelFormat::BGRa8) {
        return output == GpuFrameFormat::RGBA8;
    }
    if (input == CameraPixelFormat::BayerRG8 || input == CameraPixelFormat::BayerGR8 ||
        input == CameraPixelFormat::BayerGB8 || input == CameraPixelFormat::BayerBG8) {
        return output == GpuFrameFormat::RGBA8;
    }
    return false;
}

bool ParseCameraPixelFormat(const std::string& text, CameraPixelFormat& out) noexcept
{
    if (text == "Mono8") { out = CameraPixelFormat::Mono8; return true; }
    if (text == "BayerRG8") { out = CameraPixelFormat::BayerRG8; return true; }
    if (text == "BayerGR8") { out = CameraPixelFormat::BayerGR8; return true; }
    if (text == "BayerGB8") { out = CameraPixelFormat::BayerGB8; return true; }
    if (text == "BayerBG8") { out = CameraPixelFormat::BayerBG8; return true; }
    if (text == "BGR8") { out = CameraPixelFormat::BGR8; return true; }
    if (text == "BGRa8" || text == "BGRA8") { out = CameraPixelFormat::BGRa8; return true; }
    return false;
}

} // namespace IC4Ext
