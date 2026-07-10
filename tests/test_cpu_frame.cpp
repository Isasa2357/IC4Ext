#include <IC4Ext/IC4Ext.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

void TestProperties()
{
    assert(IC4Ext::BytesPerPixel(IC4Ext::CpuFrameFormat::Gray8) == 1);
    assert(IC4Ext::BytesPerPixel(IC4Ext::CpuFrameFormat::RGBA8) == 4);
    assert(IC4Ext::BytesPerPixel(IC4Ext::CpuFrameFormat::RGB8) == 3);
    assert(IC4Ext::BytesPerPixel(IC4Ext::CpuFrameFormat::BGR8) == 3);
    assert(IC4Ext::TightRowPitch(7, IC4Ext::CpuFrameFormat::Gray8) == 7);
    assert(IC4Ext::TightRowPitch(7, IC4Ext::CpuFrameFormat::RGBA8) == 28);
    assert(IC4Ext::TightFrameByteSize(7, 3, IC4Ext::CpuFrameFormat::RGB8) == 63);
}

void TestR8Conversions()
{
    const std::uint32_t width = 3;
    const std::uint32_t height = 2;
    const std::uint32_t rowPitch = 5;
    const std::vector<std::uint8_t> src = {
        1, 2, 3, 0xee, 0xee,
        4, 5, 6, 0xee, 0xee,
    };

    IC4Ext::CpuFrame frame;
    IC4Ext::ErrorInfo error;
    assert(IC4Ext::ConvertPackedGpuFrameToCpuFrame(src.data(), width, height, rowPitch,
                                                   IC4Ext::GpuFrameFormat::R8,
                                                   IC4Ext::CpuFrameFormat::Gray8,
                                                   {}, frame, &error));
    assert(frame.rowPitch == 3);
    assert((frame.data == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}));

    assert(IC4Ext::ConvertPackedGpuFrameToCpuFrame(src.data(), width, height, rowPitch,
                                                   IC4Ext::GpuFrameFormat::R8,
                                                   IC4Ext::CpuFrameFormat::RGBA8,
                                                   {}, frame, &error));
    assert(frame.rowPitch == 12);
    assert(frame.data[0] == 1 && frame.data[1] == 1 && frame.data[2] == 1 && frame.data[3] == 255);
    assert(frame.data[20] == 6 && frame.data[21] == 6 && frame.data[22] == 6 && frame.data[23] == 255);
}

void TestRgbaConversions()
{
    const std::uint32_t width = 2;
    const std::uint32_t height = 2;
    const std::uint32_t rowPitch = 12;
    const std::vector<std::uint8_t> src = {
        10, 20, 30, 40,  50, 60, 70, 80,  0xee, 0xee, 0xee, 0xee,
        90, 80, 70, 60,  30, 20, 10,  0,  0xee, 0xee, 0xee, 0xee,
    };

    IC4Ext::CpuFrame frame;
    IC4Ext::ErrorInfo error;
    assert(IC4Ext::ConvertPackedGpuFrameToCpuFrame(src.data(), width, height, rowPitch,
                                                   IC4Ext::GpuFrameFormat::RGBA8,
                                                   IC4Ext::CpuFrameFormat::RGB8,
                                                   {}, frame, &error));
    assert((frame.data == std::vector<std::uint8_t>{10,20,30, 50,60,70, 90,80,70, 30,20,10}));

    assert(IC4Ext::ConvertPackedGpuFrameToCpuFrame(src.data(), width, height, rowPitch,
                                                   IC4Ext::GpuFrameFormat::RGBA8,
                                                   IC4Ext::CpuFrameFormat::BGR8,
                                                   {}, frame, &error));
    assert((frame.data == std::vector<std::uint8_t>{30,20,10, 70,60,50, 70,80,90, 10,20,30}));

    assert(IC4Ext::ConvertPackedGpuFrameToCpuFrame(src.data(), width, height, rowPitch,
                                                   IC4Ext::GpuFrameFormat::RGBA8,
                                                   IC4Ext::CpuFrameFormat::Gray8,
                                                   {}, frame, &error));
    assert(frame.data[0] == static_cast<std::uint8_t>((77u * 10u + 150u * 20u + 29u * 30u + 128u) >> 8u));
    assert(frame.data[3] == static_cast<std::uint8_t>((77u * 30u + 150u * 20u + 29u * 10u + 128u) >> 8u));
}

} // namespace

int main()
{
    TestProperties();
    TestR8Conversions();
    TestRgbaConversions();
    std::cout << "test_cpu_frame passed\n";
    return 0;
}
