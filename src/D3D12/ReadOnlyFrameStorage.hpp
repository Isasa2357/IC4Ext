#pragma once

#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"

#include <functional>

namespace IC4Ext::D3D12 {

struct D3D12ReadOnlyFrame::Storage
{
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    DXGI_FORMAT formatValue = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES publishedStateValue = D3D12_RESOURCE_STATE_COMMON;
    ::IC4Ext::D3D12ReadyToken ready;
    FrameTiming timingValue;
    FrameFormatMetadata frameFormat;
    FrameChunkMetadata chunk;
    std::function<void()> releaseCallback;

    ~Storage()
    {
        if (releaseCallback) {
            releaseCallback();
        }
    }
};

inline const ::IC4Ext::D3D12ReadyToken& EmptyReadyToken() noexcept
{
    static const ::IC4Ext::D3D12ReadyToken value{};
    return value;
}

inline const FrameTiming& EmptyFrameTiming() noexcept
{
    static const FrameTiming value{};
    return value;
}

inline const FrameFormatMetadata& EmptyFrameFormat() noexcept
{
    static const FrameFormatMetadata value{};
    return value;
}

inline const FrameChunkMetadata& EmptyFrameChunk() noexcept
{
    static const FrameChunkMetadata value{};
    return value;
}

} // namespace IC4Ext::D3D12
