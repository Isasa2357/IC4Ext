#pragma once

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <utility>

namespace IC4Ext {

// Shared/non-owning D3D11 device context bundle used by the D3D11 ReadOnly
// pipeline. resolve() also enables ID3D11Multithread protection by default so
// independent camera/source worker threads may safely submit to the immediate
// context.
struct D3D11BackendContext
{
    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    D3D11CoreLib::D3D11Core* corePtr = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediateContext = nullptr;
    bool enableMultithreadProtection = true;

    static D3D11BackendContext FromCore(
        std::shared_ptr<D3D11CoreLib::D3D11Core> sharedCore,
        bool protectImmediateContext = true)
    {
        D3D11BackendContext result;
        result.core = std::move(sharedCore);
        result.corePtr = result.core.get();
        result.enableMultithreadProtection = protectImmediateContext;
        result.resolve();
        return result;
    }

    static D3D11BackendContext FromCore(
        D3D11CoreLib::D3D11Core& coreReference,
        bool protectImmediateContext = true)
    {
        D3D11BackendContext result;
        result.corePtr = &coreReference;
        result.enableMultithreadProtection = protectImmediateContext;
        result.resolve();
        return result;
    }

    bool resolve() noexcept
    {
        if (!corePtr && core) corePtr = core.get();
        if (!device && corePtr) device = corePtr->GetDevice();
        if (!immediateContext && corePtr) {
            immediateContext = corePtr->GetImmediateContext();
        }

        if (!device || !immediateContext) return false;

        if (enableMultithreadProtection) {
            Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
            if (SUCCEEDED(immediateContext->QueryInterface(IID_PPV_ARGS(&multithread))) &&
                multithread) {
                multithread->SetMultithreadProtected(TRUE);
            }
        }
        return true;
    }
};

} // namespace IC4Ext
