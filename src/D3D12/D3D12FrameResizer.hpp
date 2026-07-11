#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Processing/D3D12ProcessingContext.hpp>
#include <D3D12Helper/D3D12Processing/D3D12Resize.hpp>

#include <array>
#include <cstddef>
#include <filesystem>

namespace IC4Ext {

class D3D12FrameResizer
{
public:
    ~D3D12FrameResizer();

    bool initialize(const D3D12BackendContext& backend,
                    D3D12FenceManager* fenceManager,
                    const std::filesystem::path& shaderDirectory);

    bool resizeFrame(const D3D12CameraFrame& src,
                     const CameraOutputResizeOptions& options,
                     D3D12CameraFrame& dst);

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    struct FrameSlot
    {
        D3D12CoreLib::D3D12DescriptorAllocator cbvSrvUav;
        D3D12CoreLib::D3D12DescriptorAllocator sampler;
        D3D12CoreLib::Processing::D3D12ProcessingContext processingContext;
        D3D12CoreLib::Processing::D3D12Resizer resizer;
        D3D12CoreLib::D3D12CommandContext commandContext;
        D3D12ReadyToken inFlight;
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
    bool initialized_ = false;
    ErrorInfo lastError_;
};

} // namespace IC4Ext
