#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"

#include <D3D11Helper/D3D11Interop/D3D11Fence.hpp>

#include <atomic>
#include <d3d11_4.h>
#include <wrl/client.h>

namespace IC4Ext {

class D3D11FenceManager
{
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    D3D11ReadyToken signal();
    bool wait(const D3D11ReadyToken& token, std::uint32_t timeoutMs = INFINITE) const;
    bool isReady(const D3D11ReadyToken& token) const noexcept;

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    ID3D11DeviceContext* context_ = nullptr;
    D3D11CoreLib::D3D11Fence fence_;
    std::atomic<std::uint64_t> nextValue_{1};
    ErrorInfo lastError_;
};

} // namespace IC4Ext
