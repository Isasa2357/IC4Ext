#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12ReadyToken.hpp"

#include <D3D12Helper/D3D12Core/D3D12Queue.hpp>

namespace IC4Ext {

class D3D12FenceManager
{
public:
    bool initialize(const D3D12BackendContext& backend);
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue);

    D3D12ReadyToken signal();
    D3D12ReadyToken makeToken(std::uint64_t fenceValue) const;
    bool wait(const D3D12ReadyToken& token, std::uint32_t timeoutMs = INFINITE) const;
    bool isReady(const D3D12ReadyToken& token) const noexcept;

    D3D12CoreLib::D3D12Queue* helperQueue() const noexcept { return helperQueue_; }
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    void setError(ErrorCode code, const char* where, const std::string& message);

    D3D12CoreLib::D3D12Queue* helperQueue_ = nullptr;
    ErrorInfo lastError_;
};

} // namespace IC4Ext
