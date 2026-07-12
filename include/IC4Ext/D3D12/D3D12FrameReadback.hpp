#pragma once

#include "IC4Ext/Core/CpuFrame.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ReadbackBuffer.hpp>

#include <cstdint>

namespace IC4Ext {

class D3D12FrameReadback
{
public:
    bool initialize(const D3D12BackendContext& backend);

    // Legacy mutable-frame path. The implementation may transition the source
    // resource to COPY_SOURCE and back around the copy.
    bool readback(const D3D12CameraFrame& frame,
                  CpuFrameFormat dstFormat,
                  CpuFrame& out,
                  std::uint32_t waitTimeoutMs = 1000);

    // Immutable shared-frame path. The source resource is never transitioned.
    // The published state must already include COPY_SOURCE (GENERIC_READ does),
    // and the readback queue waits on the producer-ready fence on the GPU.
    // A separate D3D12FrameReadback instance/queue should be used by each
    // concurrently executing CPU pipeline.
    bool readback(const D3D12::ReadOnlyFrame& frame,
                  CpuFrameFormat dstFormat,
                  CpuFrame& out,
                  std::uint32_t waitTimeoutMs = 1000);

    void resetCache() noexcept;
    FrameReadbackCacheStats cacheStats() const noexcept { return cacheStats_; }

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    bool validateFrame(const D3D12CameraFrame& frame,
                       GpuFrameFormat& srcFormat,
                       D3D12_RESOURCE_DESC& desc);
    bool validateReadOnlyFrame(const D3D12::ReadOnlyFrame& frame,
                               GpuFrameFormat& srcFormat,
                               D3D12_RESOURCE_DESC& desc);
    bool ensureReadbackBuffer(UINT64 totalBytes);
    bool copyAndConvert(ID3D12Resource* source,
                        const D3D12_RESOURCE_DESC& desc,
                        GpuFrameFormat srcFormat,
                        CpuFrameFormat dstFormat,
                        const FrameTiming& timing,
                        const FrameChunkMetadata& chunkMetadata,
                        CpuFrame& out,
                        std::uint32_t waitTimeoutMs,
                        D3D12_RESOURCE_STATES sourceState,
                        bool transitionSource);
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    D3D12BackendContext backend_;
    D3D12CoreLib::D3D12Core* core_ = nullptr;
    D3D12CoreLib::D3D12Queue* queue_ = nullptr;
    ID3D12Device* device_ = nullptr;
    D3D12CoreLib::D3D12CommandContext commandContext_;

    D3D12CoreLib::D3D12ReadbackBuffer readbackBuffer_;
    UINT64 readbackBufferSizeBytes_ = 0;
    FrameReadbackCacheStats cacheStats_{};

    ErrorInfo lastError_;
};

} // namespace IC4Ext
