#include "IC4Ext/D3D12/D3D12FrameCopier.hpp"

#include <D3D12Helper/D3D12Core/D3D12Barrier.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <exception>
#include <sstream>

namespace IC4Ext {

void D3D12FrameCopier::setError(ErrorCode code, const char* where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12FrameCopier::initialize(const D3D12BackendContext& backendIn, D3D12FenceManager* fenceManager)
{
    lastError_ = NoError();
    backend_ = backendIn;
    if (!backend_.resolve() || !backend_.corePtr || !backend_.queue || !fenceManager) {
        setError(ErrorCode::InvalidArgument, "D3D12FrameCopier::initialize", "D3D12 backend must be created from D3D12Helper D3D12Core and queue");
        return false;
    }
    core_ = backend_.corePtr;
    queue_ = backend_.queue;
    device_ = backend_.device;
    fenceManager_ = fenceManager;

    for (auto& slot : slots_) {
        slot.commandContext.Initialize(device_, queue_->GetType());
        slot.initialized = true;
    }
    nextSlot_ = 0;
    return true;
}

bool D3D12FrameCopier::initialize(ID3D12Device* device, ID3D12CommandQueue* queue, D3D12FenceManager* fenceManager)
{
    (void)device;
    (void)queue;
    (void)fenceManager;
    setError(ErrorCode::InvalidArgument,
             "D3D12FrameCopier::initialize",
             "Raw ID3D12Device/ID3D12CommandQueue initialization is intentionally unsupported in the helper-integrated backend. Use D3D12BackendContext::FromCore(...).");
    return false;
}

D3D12FrameCopier::FrameSlot* D3D12FrameCopier::acquireSlot()
{
    auto& slot = slots_[nextSlot_ % slots_.size()];
    nextSlot_ = (nextSlot_ + 1) % slots_.size();
    if (slot.inFlight.isValid()) {
        slot.inFlight.wait(INFINITE);
        slot.inFlight = {};
    }
    slot.commandContext.Reset();
    return &slot;
}

bool D3D12FrameCopier::createSrv(D3D12CameraFrame& frame)
{
    if (!core_ || !frame.textureResource.Get()) return false;
    try {
        frame.srvHeapHelper.Initialize(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
        frame.srvHeap = frame.srvHeapHelper.Get();
        auto handle = frame.srvHeapHelper.GetHandle(0);
        D3D12CoreLib::CreateTexture2DSrv(*core_, frame.textureResource, handle.cpu, frame.dxgiFormat);
        frame.srvCpuHandle = handle.cpu;
        frame.srvGpuHandle = handle.gpu;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameCopier::createSrv", e.what());
        return false;
    }
}

bool D3D12FrameCopier::copyFrameNoSignal(const D3D12CameraFrame& src, D3D12CameraFrame& dst)
{
    lastError_ = NoError();
    dst = {};
    if (!core_ || !queue_ || !device_ || !fenceManager_) {
        setError(ErrorCode::InvalidArgument, "D3D12FrameCopier::copyFrameNoSignal", "copier is not initialized");
        return false;
    }
    if (!src.texture) {
        setError(ErrorCode::InvalidArgument, "D3D12FrameCopier::copyFrameNoSignal", "src.texture is null");
        return false;
    }

    if (src.ready.isValid()) {
        src.ready.wait(INFINITE);
    }

    auto* slot = acquireSlot();
    if (!slot) {
        setError(ErrorCode::D3D12Error, "D3D12FrameCopier::copyFrameNoSignal", "No copy frame slot is available");
        return false;
    }

    try {
        const D3D12_RESOURCE_DESC desc = src.texture->GetDesc();
        dst.textureResource = D3D12CoreLib::CreateTexture2D(*core_,
                                                            static_cast<UINT>(desc.Width),
                                                            desc.Height,
                                                            desc.Format,
                                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                                            desc.Flags,
                                                            desc.DepthOrArraySize,
                                                            desc.MipLevels);
        dst.texture = dst.textureResource.Get();

        auto* cmd = slot->commandContext.GetCommandList();
        if (src.resourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto toSrc = D3D12CoreLib::MakeTransitionBarrier(src.texture.Get(), src.resourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            slot->commandContext.ResourceBarrier(toSrc);
        }
        cmd->CopyResource(dst.texture.Get(), src.texture.Get());
        if (src.resourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto back = D3D12CoreLib::MakeTransitionBarrier(src.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, src.resourceState);
            slot->commandContext.ResourceBarrier(back);
        }
        auto dstFinal = D3D12CoreLib::MakeTransitionBarrier(dst.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, src.resourceState);
        slot->commandContext.ResourceBarrier(dstFinal);

        dst.textureResource.SetState(src.resourceState);
        dst.dxgiFormat = src.dxgiFormat;
        dst.resourceState = src.resourceState;
        dst.timing = src.timing;
        dst.format = src.format;

        slot->commandContext.Close();
        ID3D12CommandList* lists[] = { slot->commandContext.GetCommandList() };
        queue_->ExecuteCommandLists(1, lists);
        const std::uint64_t fenceValue = queue_->Signal();
        slot->inFlight = fenceManager_->makeToken(fenceValue);

        return createSrv(dst);
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameCopier::copyFrameNoSignal", e.what());
        return false;
    }
}

bool D3D12FrameCopier::copyFrame(const D3D12CameraFrame& src, D3D12CameraFrame& dst)
{
    if (!copyFrameNoSignal(src, dst)) return false;
    dst.ready = fenceManager_ ? fenceManager_->signal() : D3D12ReadyToken{};
    if (!dst.ready.isValid()) {
        setError(ErrorCode::D3D12Error, "D3D12FrameCopier::copyFrame / signal", fenceManager_ ? fenceManager_->lastError().message : "Fence manager is null");
        return false;
    }
    return true;
}

} // namespace IC4Ext
