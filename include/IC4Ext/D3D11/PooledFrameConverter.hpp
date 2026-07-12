#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11FrameConverter.hpp"
#include "IC4Ext/D3D11/FramePool.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace IC4Ext::D3D11 {

struct D3D11PooledFrameConverterStats
{
    std::uint64_t conversions = 0;
    std::uint64_t inputBufferAllocations = 0;
    std::uint64_t inputBufferReuses = 0;
    std::size_t cachedInputBufferCount = 0;
    std::uint64_t cachedInputBufferBytes = 0;
};

class D3D11PooledFrameConverter final
{
public:
    D3D11PooledFrameConverter();
    ~D3D11PooledFrameConverter();

    D3D11PooledFrameConverter(const D3D11PooledFrameConverter&) = delete;
    D3D11PooledFrameConverter& operator=(const D3D11PooledFrameConverter&) = delete;
    D3D11PooledFrameConverter(D3D11PooledFrameConverter&&) noexcept;
    D3D11PooledFrameConverter& operator=(D3D11PooledFrameConverter&&) noexcept;

    bool initialize(::IC4Ext::D3D11FrameConverter& converter);

    bool convert(
        const ::IC4Ext::CpuFrameView& input,
        const FrameOutputSpec& outputSpec,
        D3D11FrameWriter writer,
        FrameChunkMetadata chunkMetadata,
        D3D11ReadOnlyFrame& outFrame);

    D3D11PooledFrameConverterStats stats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

using PooledFrameConverterStats = D3D11PooledFrameConverterStats;
using PooledFrameConverter = D3D11PooledFrameConverter;

} // namespace IC4Ext::D3D11
