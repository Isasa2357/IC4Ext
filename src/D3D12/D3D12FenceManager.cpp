#include "IC4Ext/D3D12/D3D12FenceManager.hpp"

namespace IC4Ext {

void D3D12FenceManager::setError(ErrorCode code, const char* where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12FenceManager::initialize(const D3D12BackendContext& backendIn)
{
    lastError_ = NoError();
    auto backend = backendIn;
    if (!backend.resolve() || !backend.queue) {
        setError(ErrorCode::InvalidArgument,
                 "D3D12FenceManager::initialize",
                 "D3D12BackendContext must contain a D3D12Helper D3D12Queue");
        return false;
    }
    helperQueue_ = backend.queue;
    return true;
}

bool D3D12FenceManager::initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    (void)device;
    (void)queue;
    helperQueue_ = nullptr;
    setError(ErrorCode::InvalidArgument,
             "D3D12FenceManager::initialize",
             "Raw ID3D12Device/ID3D12CommandQueue initialization is intentionally unsupported in the helper-integrated backend. Use D3D12BackendContext::FromCore(...).");
    return false;
}

D3D12ReadyToken D3D12FenceManager::makeToken(std::uint64_t fenceValue) const
{
    D3D12ReadyToken token;
    token.value = fenceValue;
    if (helperQueue_) {
        token.fence = helperQueue_->Fence().Get();
    }
    return token;
}

D3D12ReadyToken D3D12FenceManager::signal()
{
    lastError_ = NoError();
    if (!helperQueue_) {
        setError(ErrorCode::D3D12Error, "D3D12FenceManager::signal", "Fence manager is not initialized with D3D12Helper queue");
        return {};
    }
    return makeToken(helperQueue_->Signal());
}

bool D3D12FenceManager::wait(const D3D12ReadyToken& token, std::uint32_t timeoutMs) const
{
    return token.wait(timeoutMs);
}

bool D3D12FenceManager::isReady(const D3D12ReadyToken& token) const noexcept
{
    return token.isReady();
}

} // namespace IC4Ext
