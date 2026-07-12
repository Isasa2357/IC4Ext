#pragma once

#include "IC4Ext/Core/CpuFrame.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"
#include "IC4Ext/D3D11/D3D11ContextSynchronization.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace IC4Ext {

class D3D11FrameReadback
{
public:
    bool initialize(D3D11CoreLib::D3D11Core* core);
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    bool readback(const D3D11CameraFrame& frame,
                  CpuFrameFormat dstFormat,
                  CpuFrame& out,
                  std::uint32_t waitTimeoutMs = 1000);

    bool readback(const D3D11::ReadOnlyFrame& frame,
                  CpuFrameFormat dstFormat,
                  CpuFrame& out,
                  std::uint32_t waitTimeoutMs = 1000);

    void resetCache() noexcept;
    FrameReadbackCacheStats cacheStats() const noexcept { return cacheStats_; }
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    bool validateTexture(ID3D11Texture2D* texture,
                         const char* where,
                         GpuFrameFormat& srcFormat,
                         D3D11_TEXTURE2D_DESC& desc);
    bool ensureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc);
    bool copyAndConvert(ID3D11Texture2D* texture,
                        const D3D11_TEXTURE2D_DESC& desc,
                        GpuFrameFormat srcFormat,
                        CpuFrameFormat dstFormat,
                        const FrameTiming& timing,
                        const FrameChunkMetadata& chunkMetadata,
                        CpuFrame& out);
    void setError(ErrorCode code,
                  const std::string& where,
                  const std::string& message);

    D3D11CoreLib::D3D11Core* core_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::shared_ptr<std::recursive_mutex> contextMutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture_;
    D3D11_TEXTURE2D_DESC stagingDesc_{};
    bool hasStagingDesc_ = false;
    FrameReadbackCacheStats cacheStats_{};
    ErrorInfo lastError_;
};

} // namespace IC4Ext
