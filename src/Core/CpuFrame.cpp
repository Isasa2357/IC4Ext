#include "IC4Ext/Core/CpuFrame.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace IC4Ext {

namespace {

std::uint8_t LumaFromRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    // Integer approximation of round(0.299 R + 0.587 G + 0.114 B).
    return static_cast<std::uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8u);
}

void SetError(ErrorInfo* error, ErrorCode code, const char* where, const std::string& message)
{
    if (error) *error = MakeError(code, where, message);
}

bool ValidateConvertArgs(const std::uint8_t* src,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t srcRowPitch,
                         GpuFrameFormat srcFormat,
                         CpuFrameFormat dstFormat,
                         ErrorInfo* error)
{
    if (error) *error = NoError();
    if (!src) {
        SetError(error, ErrorCode::InvalidArgument, "ConvertPackedGpuFrameToCpuFrame", "source pointer is null");
        return false;
    }
    if (width == 0 || height == 0) {
        SetError(error, ErrorCode::InvalidArgument, "ConvertPackedGpuFrameToCpuFrame", "width and height must be non-zero");
        return false;
    }
    const std::size_t srcBpp = (srcFormat == GpuFrameFormat::R8) ? 1u : (srcFormat == GpuFrameFormat::RGBA8 ? 4u : 0u);
    if (srcBpp == 0) {
        SetError(error, ErrorCode::UnsupportedFormat, "ConvertPackedGpuFrameToCpuFrame", "unsupported GPU frame format");
        return false;
    }
    if (BytesPerPixel(dstFormat) == 0) {
        SetError(error, ErrorCode::UnsupportedFormat, "ConvertPackedGpuFrameToCpuFrame", "unsupported CPU frame format");
        return false;
    }
    if (srcRowPitch < static_cast<std::uint64_t>(width) * srcBpp) {
        SetError(error, ErrorCode::InvalidArgument, "ConvertPackedGpuFrameToCpuFrame", "source row pitch is smaller than one tight row");
        return false;
    }
    return true;
}

bool ResizeOutput(std::uint32_t width,
                  std::uint32_t height,
                  CpuFrameFormat dstFormat,
                  FrameTiming timing,
                  CpuFrame& out,
                  ErrorInfo* error)
{
    const std::uint64_t size = TightFrameByteSize(width, height, dstFormat);
    if (size == 0 || size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        SetError(error, ErrorCode::InvalidArgument, "ConvertPackedGpuFrameToCpuFrame", "destination frame size is invalid or too large");
        return false;
    }

    out.width = width;
    out.height = height;
    out.format = dstFormat;
    out.rowPitch = TightRowPitch(width, dstFormat);
    out.timing = timing;
    out.data.assign(static_cast<std::size_t>(size), 0);
    return true;
}

} // namespace

const char* ToString(CpuFrameFormat fmt) noexcept
{
    switch (fmt) {
    case CpuFrameFormat::Gray8: return "Gray8";
    case CpuFrameFormat::RGBA8: return "RGBA8";
    case CpuFrameFormat::RGB8: return "RGB8";
    case CpuFrameFormat::BGR8: return "BGR8";
    default: return "Unknown";
    }
}

std::size_t BytesPerPixel(CpuFrameFormat fmt) noexcept
{
    switch (fmt) {
    case CpuFrameFormat::Gray8: return 1;
    case CpuFrameFormat::RGBA8: return 4;
    case CpuFrameFormat::RGB8: return 3;
    case CpuFrameFormat::BGR8: return 3;
    default: return 0;
    }
}

std::uint32_t TightRowPitch(std::uint32_t width, CpuFrameFormat fmt) noexcept
{
    const std::size_t bpp = BytesPerPixel(fmt);
    const std::uint64_t pitch = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(bpp);
    if (pitch > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) return 0;
    return static_cast<std::uint32_t>(pitch);
}

std::uint64_t TightFrameByteSize(std::uint32_t width, std::uint32_t height, CpuFrameFormat fmt) noexcept
{
    const std::uint32_t pitch = TightRowPitch(width, fmt);
    return static_cast<std::uint64_t>(pitch) * static_cast<std::uint64_t>(height);
}

bool ConvertPackedGpuFrameToCpuFrame(const std::uint8_t* src,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint32_t srcRowPitch,
                                     GpuFrameFormat srcFormat,
                                     CpuFrameFormat dstFormat,
                                     FrameTiming timing,
                                     CpuFrame& out,
                                     ErrorInfo* error)
{
    if (!ValidateConvertArgs(src, width, height, srcRowPitch, srcFormat, dstFormat, error)) return false;
    if (!ResizeOutput(width, height, dstFormat, timing, out, error)) return false;

    if (srcFormat == GpuFrameFormat::R8) {
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* srcRow = src + static_cast<std::size_t>(y) * srcRowPitch;
            std::uint8_t* dstRow = out.data.data() + static_cast<std::size_t>(y) * out.rowPitch;
            if (dstFormat == CpuFrameFormat::Gray8) {
                std::copy(srcRow, srcRow + width, dstRow);
            } else if (dstFormat == CpuFrameFormat::RGBA8) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint8_t v = srcRow[x];
                    dstRow[x * 4u + 0u] = v;
                    dstRow[x * 4u + 1u] = v;
                    dstRow[x * 4u + 2u] = v;
                    dstRow[x * 4u + 3u] = 255;
                }
            } else if (dstFormat == CpuFrameFormat::RGB8 || dstFormat == CpuFrameFormat::BGR8) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint8_t v = srcRow[x];
                    dstRow[x * 3u + 0u] = v;
                    dstRow[x * 3u + 1u] = v;
                    dstRow[x * 3u + 2u] = v;
                }
            }
        }
        return true;
    }

    if (srcFormat == GpuFrameFormat::RGBA8) {
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* srcRow = src + static_cast<std::size_t>(y) * srcRowPitch;
            std::uint8_t* dstRow = out.data.data() + static_cast<std::size_t>(y) * out.rowPitch;
            if (dstFormat == CpuFrameFormat::RGBA8) {
                std::copy(srcRow, srcRow + static_cast<std::size_t>(width) * 4u, dstRow);
            } else if (dstFormat == CpuFrameFormat::RGB8) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    dstRow[x * 3u + 0u] = srcRow[x * 4u + 0u];
                    dstRow[x * 3u + 1u] = srcRow[x * 4u + 1u];
                    dstRow[x * 3u + 2u] = srcRow[x * 4u + 2u];
                }
            } else if (dstFormat == CpuFrameFormat::BGR8) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    dstRow[x * 3u + 0u] = srcRow[x * 4u + 2u];
                    dstRow[x * 3u + 1u] = srcRow[x * 4u + 1u];
                    dstRow[x * 3u + 2u] = srcRow[x * 4u + 0u];
                }
            } else if (dstFormat == CpuFrameFormat::Gray8) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    dstRow[x] = LumaFromRgb(srcRow[x * 4u + 0u], srcRow[x * 4u + 1u], srcRow[x * 4u + 2u]);
                }
            }
        }
        return true;
    }

    SetError(error, ErrorCode::UnsupportedFormat, "ConvertPackedGpuFrameToCpuFrame", "unsupported conversion");
    return false;
}

} // namespace IC4Ext
