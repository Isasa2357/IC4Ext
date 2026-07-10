#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace IC4Ext {

enum class CpuFrameFormat : std::uint32_t
{
    Unknown = 0,
    Gray8,
    RGBA8,
    RGB8,
    BGR8,
};

struct CpuFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CpuFrameFormat format = CpuFrameFormat::Unknown;

    // CpuFrame is always tightly packed, even if the backend readback buffer
    // uses an API-specific aligned row pitch internally.
    std::uint32_t rowPitch = 0;
    std::vector<std::uint8_t> data;

    FrameTiming timing{};
    FrameChunkMetadata chunkMetadata{};

    bool empty() const noexcept { return data.empty() || width == 0 || height == 0 || format == CpuFrameFormat::Unknown; }
};

const char* ToString(CpuFrameFormat fmt) noexcept;
std::size_t BytesPerPixel(CpuFrameFormat fmt) noexcept;
std::uint32_t TightRowPitch(std::uint32_t width, CpuFrameFormat fmt) noexcept;
std::uint64_t TightFrameByteSize(std::uint32_t width, std::uint32_t height, CpuFrameFormat fmt) noexcept;

bool ConvertPackedGpuFrameToCpuFrame(const std::uint8_t* src,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint32_t srcRowPitch,
                                     GpuFrameFormat srcFormat,
                                     CpuFrameFormat dstFormat,
                                     FrameTiming timing,
                                     CpuFrame& out,
                                     ErrorInfo* error = nullptr);

} // namespace IC4Ext
