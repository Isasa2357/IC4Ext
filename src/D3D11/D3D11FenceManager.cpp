#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <exception>

namespace IC4Ext {

void D3D11FenceManager::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FenceManager::initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    lastError_ = NoError();
    context_ = nullptr;
    if (!device || !context) {
        setError(ErrorCode::InvalidArgument, "D3D11FenceManager::initialize", "device/context is null");
        return false;
    }

    try {
        fence_.Initialize(device, D3D11_FENCE_FLAG_NONE);
        context_ = context;
        nextValue_.store(1);
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::initialize / D3D11Helper::D3D11Fence", e.what());
        return false;
    }
}

D3D11ReadyToken D3D11FenceManager::signal()
{
    D3D11ReadyToken token;
    if (!context_ || !fence_.IsInitialized()) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::signal", "Fence manager is not initialized");
        return token;
    }

    const std::uint64_t value = nextValue_.fetch_add(1);
    try {
        fence_.Signal(context_, value);
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::signal / D3D11Helper::D3D11Fence", e.what());
        return token;
    }

    token.fence = fence_.Get();
    token.value = value;
    return token;
}

bool D3D11FenceManager::wait(const D3D11ReadyToken& token, std::uint32_t timeoutMs) const
{
    return token.wait(timeoutMs);
}

bool D3D11FenceManager::isReady(const D3D11ReadyToken& token) const noexcept
{
    return token.isReady();
}

} // namespace IC4Ext
