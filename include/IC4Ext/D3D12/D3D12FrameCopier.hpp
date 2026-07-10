#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>

#include <array>
#include <cstddef>

namespace IC4Ext {

class D3D12FrameCopier
{
public:
    bool initialize(const D3D12BackendContext& backend, D3D12FenceManager* fenceManager);
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, D3D12FenceManager* fenceManager);

    // Enqueue a copy without assigning a public ready token to dst. This is used by
    // DummyCameraCaptureGenerator, which signals one shared token after all copies.
    bool copyFrameNoSignal(const D3D12CameraFrame& src, D3D12CameraFrame& dst);

    // Enqueue copy and assign a ready token immediately.
    bool copyFrame(const D3D12CameraFrame& src, D3D12CameraFrame& dst);

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    struct FrameSlot
    {
        D3D12CoreLib::D3D12CommandContext commandContext;
        D3D12ReadyToken inFlight;
        bool initialized = false;
    };

    FrameSlot* acquireSlot();
    bool createSrv(D3D12CameraFrame& frame);
    void setError(ErrorCode code, const char* where, const std::string& message);

    D3D12BackendContext backend_;
    D3D12CoreLib::D3D12Core* core_ = nullptr;
    D3D12CoreLib::D3D12Queue* queue_ = nullptr;
    ID3D12Device* device_ = nullptr;
    D3D12FenceManager* fenceManager_ = nullptr;
    static constexpr std::size_t kFrameSlotCount = 4;
    std::array<FrameSlot, kFrameSlotCount> slots_{};
    std::size_t nextSlot_ = 0;
    ErrorInfo lastError_;
};

} // namespace IC4Ext
