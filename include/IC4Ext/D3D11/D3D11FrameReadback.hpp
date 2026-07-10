#pragma once

#include "IC4Ext/Core/CpuFrame.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

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

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    bool validateFrame(const D3D11CameraFrame& frame, GpuFrameFormat& srcFormat);
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    D3D11CoreLib::D3D11Core* core_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    ErrorInfo lastError_;
};

} // namespace IC4Ext
