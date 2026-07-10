#pragma once

#include "IC4Ext/Core/CpuFrame.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ReadbackBuffer.hpp>

#include <cstdint>

namespace IC4Ext {

class D3D12FrameReadback
{
public:
    bool initialize(const D3D12BackendContext& backend);

    bool readback(const D3D12CameraFrame& frame,
                  CpuFrameFormat dstFormat,
                  CpuFrame& out,
                  std::uint32_t waitTimeoutMs = 1000);

    void resetCache() noexcept;
    FrameReadbackCacheStats cacheStats() const noexcept { return cacheStats_; }

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    bool validateFrame(const D3D12CameraFrame& frame, GpuFrameFormat& srcFormat, D3D12_RESOURCE_DESC& desc);
    bool ensureReadbackBuffer(UINT64 totalBytes);
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
