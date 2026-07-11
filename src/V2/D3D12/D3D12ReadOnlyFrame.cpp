#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrame.hpp"

#include "D3D12ReadOnlyFrameStorage.hpp"

#include <utility>

namespace IC4Ext::V2 {

D3D12ReadOnlyFrame::D3D12ReadOnlyFrame(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage))
{
}

bool D3D12ReadOnlyFrame::valid() const noexcept
{
    return storage_ && storage_->texture;
}

ID3D12Resource* D3D12ReadOnlyFrame::resource() const noexcept
{
    return storage_ ? storage_->texture.Get() : nullptr;
}

ID3D12DescriptorHeap* D3D12ReadOnlyFrame::descriptorHeap() const noexcept
{
    return storage_ ? storage_->srvHeap.Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12ReadOnlyFrame::srvCpuHandle() const noexcept
{
    return storage_ ? storage_->srvCpu : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12ReadOnlyFrame::srvGpuHandle() const noexcept
{
    return storage_ ? storage_->srvGpu : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

DXGI_FORMAT D3D12ReadOnlyFrame::dxgiFormat() const noexcept
{
    return storage_ ? storage_->formatValue : DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_STATES D3D12ReadOnlyFrame::publishedState() const noexcept
{
    return storage_ ? storage_->publishedStateValue : D3D12_RESOURCE_STATE_COMMON;
}

const D3D12ReadyToken& D3D12ReadOnlyFrame::readyToken() const noexcept
{
    return storage_ ? storage_->ready : EmptyReadyToken();
}

const FrameTiming& D3D12ReadOnlyFrame::timing() const noexcept
{
    return storage_ ? storage_->timingValue : EmptyFrameTiming();
}

const FrameFormatMetadata& D3D12ReadOnlyFrame::format() const noexcept
{
    return storage_ ? storage_->frameFormat : EmptyFrameFormat();
}

const FrameChunkMetadata& D3D12ReadOnlyFrame::chunkMetadata() const noexcept
{
    return storage_ ? storage_->chunk : EmptyFrameChunk();
}

} // namespace IC4Ext::V2
