#pragma once

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <D3D12Helper/D3D12Core/D3D12Queue.hpp>

#include <d3d12.h>
#include <memory>
#include <utility>

namespace IC4Ext {

enum class D3D12BackendQueueKind
{
    Direct,
    ComputeIfAvailable
};

struct D3D12BackendContext
{
    // Preferred ownership path: share one D3D12Helper core across IC4Ext and the caller.
    std::shared_ptr<D3D12CoreLib::D3D12Core> core;

    // Non-owning core pointer used internally. When `core` is set, this is resolved
    // from core.get(). FromCore(D3D12Core&) uses this field without taking ownership.
    D3D12CoreLib::D3D12Core* corePtr = nullptr;

    // Non-owning helper queue used for conversion/copy commands.
    // Defaults to core/corePtr DirectQueue() when resolved.
    D3D12CoreLib::D3D12Queue* queue = nullptr;

    // Compatibility/cache fields. They are resolved from core/queue by resolve().
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;

    static D3D12BackendContext FromCore(std::shared_ptr<D3D12CoreLib::D3D12Core> sharedCore,
                                        D3D12BackendQueueKind kind = D3D12BackendQueueKind::Direct)
    {
        D3D12BackendContext ctx;
        ctx.core = std::move(sharedCore);
        ctx.corePtr = ctx.core.get();
        if (ctx.corePtr) {
            if (kind == D3D12BackendQueueKind::ComputeIfAvailable && ctx.corePtr->ComputeQueue()) {
                ctx.queue = ctx.corePtr->ComputeQueue();
            } else {
                ctx.queue = &ctx.corePtr->DirectQueue();
            }
            ctx.device = ctx.corePtr->GetDevice();
            ctx.commandQueue = ctx.queue ? ctx.queue->Get() : nullptr;
        }
        return ctx;
    }

    static D3D12BackendContext FromCore(D3D12CoreLib::D3D12Core& coreRef,
                                        D3D12BackendQueueKind kind = D3D12BackendQueueKind::Direct)
    {
        D3D12BackendContext ctx;
        ctx.corePtr = &coreRef;
        if (kind == D3D12BackendQueueKind::ComputeIfAvailable && coreRef.ComputeQueue()) {
            ctx.queue = coreRef.ComputeQueue();
        } else {
            ctx.queue = &coreRef.DirectQueue();
        }
        ctx.device = coreRef.GetDevice();
        ctx.commandQueue = ctx.queue ? ctx.queue->Get() : nullptr;
        return ctx;
    }

    bool resolve() noexcept
    {
        if (!corePtr && core) {
            corePtr = core.get();
        }
        if (!queue && corePtr) {
            queue = &corePtr->DirectQueue();
        }
        if (!device && corePtr) {
            device = corePtr->GetDevice();
        }
        if (!commandQueue && queue) {
            commandQueue = queue->Get();
        }
        return corePtr != nullptr && device != nullptr && commandQueue != nullptr && queue != nullptr;
    }
};

} // namespace IC4Ext
