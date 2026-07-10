#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <sstream>

namespace IC4Ext {

namespace {
std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}
}

void D3D11FenceManager::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FenceManager::initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    lastError_ = NoError();
    if (!device || !context) {
        setError(ErrorCode::InvalidArgument, "D3D11FenceManager::initialize", "device/context is null");
        return false;
    }

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device5_));
    if (FAILED(hr) || !device5_) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::initialize / ID3D11Device5", "ID3D11Device5 is required. " + HrToString(hr));
        return false;
    }

    hr = context->QueryInterface(IID_PPV_ARGS(&context4_));
    if (FAILED(hr) || !context4_) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::initialize / ID3D11DeviceContext4", "ID3D11DeviceContext4 is required. " + HrToString(hr));
        return false;
    }

    hr = device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr) || !fence_) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::initialize / CreateFence", "ID3D11Device5::CreateFence failed. " + HrToString(hr));
        return false;
    }

    nextValue_.store(1);
    return true;
}

D3D11ReadyToken D3D11FenceManager::signal()
{
    D3D11ReadyToken token;
    if (!context4_ || !fence_) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::signal", "Fence manager is not initialized");
        return token;
    }

    const std::uint64_t value = nextValue_.fetch_add(1);
    HRESULT hr = context4_->Signal(fence_.Get(), value);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FenceManager::signal", "ID3D11DeviceContext4::Signal failed. " + HrToString(hr));
        return token;
    }

    token.fence = fence_;
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
