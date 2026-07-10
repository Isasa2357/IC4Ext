#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"

#include <d3d11.h>

namespace IC4Ext {

class D3D11FenceManager;

class D3D11FrameCopier
{
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, D3D11FenceManager* fenceManager);
    bool copyFrame(const D3D11CameraFrame& src, D3D11CameraFrame& dst);
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    D3D11FenceManager* fenceManager_ = nullptr;
    ErrorInfo lastError_;
};

} // namespace IC4Ext
