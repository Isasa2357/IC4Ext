#include <IC4Ext/IC4Ext.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Float4
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;
};

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream ifs(path, std::ios::binary);
    assert(ifs);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool Contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::uint8_t LoadByte(const std::vector<std::uint8_t>& data,
                      int width,
                      int height,
                      int rowPitch,
                      int bytesPerPixel,
                      int x,
                      int y,
                      int component)
{
    if (x < 0) x = 0;
    if (x >= width) x = width - 1;
    if (y < 0) y = 0;
    if (y >= height) y = height - 1;
    return data[static_cast<std::size_t>(y * rowPitch + x * bytesPerPixel + component)];
}

int BayerColorAt(IC4Ext::CameraPixelFormat fmt, int x, int y)
{
    const int px = x & 1;
    const int py = y & 1;
    switch (fmt) {
    case IC4Ext::CameraPixelFormat::BayerBG8: // BGGR: B G / G R
        return (py == 0) ? ((px == 0) ? 2 : 1) : ((px == 0) ? 1 : 0);
    case IC4Ext::CameraPixelFormat::BayerGB8: // GBRG: G B / R G
        return (py == 0) ? ((px == 0) ? 1 : 2) : ((px == 0) ? 0 : 1);
    case IC4Ext::CameraPixelFormat::BayerGR8: // GRBG: G R / B G
        return (py == 0) ? ((px == 0) ? 1 : 0) : ((px == 0) ? 2 : 1);
    case IC4Ext::CameraPixelFormat::BayerRG8: // RGGB: R G / G B
    default:
        return (py == 0) ? ((px == 0) ? 0 : 1) : ((px == 0) ? 1 : 2);
    }
}

Float4 ConvertBayerReference(const std::vector<std::uint8_t>& data,
                             int width,
                             int height,
                             int rowPitch,
                             IC4Ext::CameraPixelFormat fmt,
                             int x,
                             int y)
{
    auto sample = [&](int sx, int sy) -> double {
        return static_cast<double>(LoadByte(data, width, height, rowPitch, 1, sx, sy, 0)) / 255.0;
    };

    const int c = BayerColorAt(fmt, x, y);
    const double center = sample(x, y);
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    if (c == 0) {
        r = center;
        g = 0.25 * (sample(x - 1, y) + sample(x + 1, y) + sample(x, y - 1) + sample(x, y + 1));
        b = 0.25 * (sample(x - 1, y - 1) + sample(x + 1, y - 1) + sample(x - 1, y + 1) + sample(x + 1, y + 1));
    } else if (c == 2) {
        b = center;
        g = 0.25 * (sample(x - 1, y) + sample(x + 1, y) + sample(x, y - 1) + sample(x, y + 1));
        r = 0.25 * (sample(x - 1, y - 1) + sample(x + 1, y - 1) + sample(x - 1, y + 1) + sample(x + 1, y + 1));
    } else {
        g = center;
        const int leftColor = BayerColorAt(fmt, std::max(x - 1, 0), y);
        const int rightColor = BayerColorAt(fmt, std::min(x + 1, width - 1), y);
        if (leftColor == 0 || rightColor == 0) {
            r = 0.5 * (sample(x - 1, y) + sample(x + 1, y));
            b = 0.5 * (sample(x, y - 1) + sample(x, y + 1));
        } else {
            b = 0.5 * (sample(x - 1, y) + sample(x + 1, y));
            r = 0.5 * (sample(x, y - 1) + sample(x, y + 1));
        }
    }
    return {r, g, b, 1.0};
}

bool Near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

void TestShaderSourceConstants()
{
    const std::filesystem::path sourceDir = std::filesystem::path(IC4EXT_SOURCE_DIR);
    const std::string rgba = ReadAll(sourceDir / "shaders" / "d3d12" / "IC4Ext_D3D12_Convert_To_RGBA8.hlsl");
    const std::string r8 = ReadAll(sourceDir / "shaders" / "d3d12" / "IC4Ext_D3D12_Convert_To_R8.hlsl");

    assert(Contains(rgba, "static const uint FMT_MONO8    = 1u;"));
    assert(Contains(rgba, "static const uint FMT_BAYERRG8 = 2u;"));
    assert(Contains(rgba, "static const uint FMT_BAYERGR8 = 3u;"));
    assert(Contains(rgba, "static const uint FMT_BAYERGB8 = 4u;"));
    assert(Contains(rgba, "static const uint FMT_BAYERBG8 = 5u;"));
    assert(Contains(rgba, "static const uint FMT_BGR8     = 6u;"));
    assert(Contains(rgba, "static const uint FMT_BGRA8    = 7u;"));
    assert(Contains(rgba, "gInputRowPitchBytes"));
    assert(Contains(rgba, "ByteAddressBuffer"));
    assert(Contains(rgba, "RWTexture2D<float4>"));
    assert(Contains(r8, "RWTexture2D<float>"));
    assert(Contains(r8, "gInputRowPitchBytes"));

    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::Mono8) == 1u, "shader enum mismatch");
    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::BayerRG8) == 2u, "shader enum mismatch");
    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::BayerGR8) == 3u, "shader enum mismatch");
    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::BayerGB8) == 4u, "shader enum mismatch");
    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::BayerBG8) == 5u, "shader enum mismatch");
    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::BGR8) == 6u, "shader enum mismatch");
    static_assert(static_cast<std::uint32_t>(IC4Ext::CameraPixelFormat::BGRa8) == 7u, "shader enum mismatch");
}

void TestPackedFormatsRespectRowPitch()
{
    const int width = 2;
    const int height = 2;
    const int bgrRowPitch = 8;
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(bgrRowPitch * height), 0xee);
    // pixel (1, 1): B=10, G=20, R=30; padding remains 0xee and must not be read.
    bgr[static_cast<std::size_t>(1 * bgrRowPitch + 1 * 3 + 0)] = 10;
    bgr[static_cast<std::size_t>(1 * bgrRowPitch + 1 * 3 + 1)] = 20;
    bgr[static_cast<std::size_t>(1 * bgrRowPitch + 1 * 3 + 2)] = 30;
    assert(LoadByte(bgr, width, height, bgrRowPitch, 3, 1, 1, 2) == 30);
    assert(LoadByte(bgr, width, height, bgrRowPitch, 3, 1, 1, 1) == 20);
    assert(LoadByte(bgr, width, height, bgrRowPitch, 3, 1, 1, 0) == 10);

    const int bgraRowPitch = 12;
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(bgraRowPitch * height), 0xee);
    bgra[static_cast<std::size_t>(0 * bgraRowPitch + 1 * 4 + 0)] = 1;
    bgra[static_cast<std::size_t>(0 * bgraRowPitch + 1 * 4 + 1)] = 2;
    bgra[static_cast<std::size_t>(0 * bgraRowPitch + 1 * 4 + 2)] = 3;
    bgra[static_cast<std::size_t>(0 * bgraRowPitch + 1 * 4 + 3)] = 4;
    assert(LoadByte(bgra, width, height, bgraRowPitch, 4, 1, 0, 2) == 3);
    assert(LoadByte(bgra, width, height, bgraRowPitch, 4, 1, 0, 3) == 4);
}

void TestBayerReferenceKnownValues()
{
    const int width = 2;
    const int height = 2;
    const int rowPitch = 4; // includes 2 padding bytes per row
    std::vector<std::uint8_t> raw = {
        100, 50, 0xee, 0xee,
         60, 10, 0xee, 0xee,
    };

    const Float4 p00 = ConvertBayerReference(raw, width, height, rowPitch, IC4Ext::CameraPixelFormat::BayerRG8, 0, 0);
    assert(Near(p00.r, 100.0 / 255.0));
    assert(Near(p00.g, 77.5 / 255.0));
    assert(Near(p00.b, 55.0 / 255.0));
    assert(Near(p00.a, 1.0));

    const Float4 p11 = ConvertBayerReference(raw, width, height, rowPitch, IC4Ext::CameraPixelFormat::BayerRG8, 1, 1);
    assert(Near(p11.r, 55.0 / 255.0));
    assert(Near(p11.g, 32.5 / 255.0));
    assert(Near(p11.b, 10.0 / 255.0));

    // Same numeric layout but different Bayer order: pixel (0,0) is blue for BGGR.
    const Float4 bg00 = ConvertBayerReference(raw, width, height, rowPitch, IC4Ext::CameraPixelFormat::BayerBG8, 0, 0);
    assert(Near(bg00.r, 55.0 / 255.0));
    assert(Near(bg00.g, 77.5 / 255.0));
    assert(Near(bg00.b, 100.0 / 255.0));
}

} // namespace

int main()
{
    TestShaderSourceConstants();
    TestPackedFormatsRespectRowPitch();
    TestBayerReferenceKnownValues();
    std::cout << "test_d3d12_shader_reference passed\n";
    return 0;
}
