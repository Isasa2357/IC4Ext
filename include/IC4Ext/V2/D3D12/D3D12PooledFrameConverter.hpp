#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12FrameConverter.hpp"
#include "IC4Ext/V2/D3D12/D3D12FramePool.hpp"
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace IC4Ext::V2 {

struct D3D12PooledFrameConverterStats
{
    std::uint64_t conversions = 0;
    std::uint64_t inputBufferAllocations = 0;
    std::uint64_t inputBufferReuses = 0;
    std::size_t cachedInputBufferCount = 0;
    std::uint64_t cachedInputBufferBytes = 0;
};

// Reuses the validated D3D12FrameConverter pipelines, upload rings and command
// slots, but records conversion into a D3D12FramePool writer instead of
// allocating a new output texture for every camera frame. Each command slot
// also retains a default-heap input buffer and grows it only when necessary.
class D3D12PooledFrameConverter final
{
public:
    D3D12PooledFrameConverter();
    ~D3D12PooledFrameConverter();

    D3D12PooledFrameConverter(const D3D12PooledFrameConverter&) = delete;
    D3D12PooledFrameConverter& operator=(const D3D12PooledFrameConverter&) = delete;
    D3D12PooledFrameConverter(D3D12PooledFrameConverter&&) noexcept;
    D3D12PooledFrameConverter& operator=(D3D12PooledFrameConverter&&) noexcept;

    // converter must already have been initialized with the D3D12 backend and
    // fence manager. Its lifetime must exceed this adapter's lifetime.
    bool initialize(IC4Ext::D3D12FrameConverter& converter);

    bool convert(const IC4Ext::D3D12CpuFrameView& input,
                 const FrameOutputSpec& outputSpec,
                 D3D12FrameWriter writer,
                 FrameChunkMetadata chunkMetadata,
                 D3D12ReadOnlyFrame& outFrame);

    D3D12PooledFrameConverterStats stats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace IC4Ext::V2
