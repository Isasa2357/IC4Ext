#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/D3D12/D3D12ReadyToken.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <memory>

namespace IC4Ext::V2 {

class D3D12FramePool;
class D3D12FrameWriter;

class D3D12ReadOnlyFrame final
{
public:
    D3D12ReadOnlyFrame() noexcept = default;

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    ID3D12Resource* resource() const noexcept;
    ID3D12DescriptorHeap* descriptorHeap() const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle() const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle() const noexcept;
    DXGI_FORMAT dxgiFormat() const noexcept;
    D3D12_RESOURCE_STATES publishedState() const noexcept;

    const D3D12ReadyToken& readyToken() const noexcept;
    const FrameTiming& timing() const noexcept;
    const FrameFormatMetadata& format() const noexcept;
    const FrameChunkMetadata& chunkMetadata() const noexcept;

private:
    struct Storage;

    explicit D3D12ReadOnlyFrame(std::shared_ptr<const Storage> storage) noexcept;

    std::shared_ptr<const Storage> storage_;

    friend class D3D12FramePool;
    friend class D3D12FrameWriter;
};

} // namespace IC4Ext::V2
