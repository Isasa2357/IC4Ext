#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/D3D12/D3D12ReadyToken.hpp"

#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorHeap.hpp>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include <ThreadKit/Queues/BlockingQueue.hpp>

namespace IC4Ext {

struct D3D12CameraFrame
{
    // Preferred D3D12Helper-owned resource wrapper. texture is kept for API compatibility.
    D3D12CoreLib::D3D12Resource textureResource;
    D3D12CoreLib::D3D12Resource inputBufferResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    // Resources that must stay alive until ready fence completes.
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadKeepAlive;
    Microsoft::WRL::ComPtr<ID3D12Resource> inputBufferKeepAlive;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocatorKeepAlive;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandListKeepAlive;
    D3D12CoreLib::D3D12DescriptorHeap srvHeapHelper;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle{};
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;

    D3D12ReadyToken ready;
    FrameTiming timing;
    FrameFormatMetadata format;
    FrameChunkMetadata chunkMetadata;
};

struct D3D12IndexedCameraFrame
{
    std::uint32_t cameraIndex = 0;
    D3D12CameraFrame frame;
};

struct D3D12SyncedFrameSet
{
    std::vector<D3D12IndexedCameraFrame> frames;
    std::uint64_t syncGroupId = 0;
    std::chrono::steady_clock::time_point emittedTime{};
};

using D3D12FrameQueue = ThreadKit::Queues::BlockingQueue<D3D12CameraFrame>;
using D3D12IndexedFrameQueue = ThreadKit::Queues::BlockingQueue<D3D12IndexedCameraFrame>;
using D3D12SyncedFrameQueue = ThreadKit::Queues::BlockingQueue<D3D12SyncedFrameSet>;

struct D3D12ReadResult
{
    bool ok = false;
    D3D12CameraFrame frame;
    ErrorInfo error;

    explicit operator bool() const noexcept { return ok; }
};

} // namespace IC4Ext
