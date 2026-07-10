#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include <ThreadKit/Queues/BlockingQueue.hpp>

namespace IC4Ext {

struct D3D11CameraFrame
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;

    D3D11ReadyToken ready;
    FrameTiming timing;
    FrameFormatMetadata format;
    FrameChunkMetadata chunkMetadata;
};

struct D3D11IndexedCameraFrame
{
    std::uint32_t cameraIndex = 0;
    D3D11CameraFrame frame;
};

struct D3D11SyncedFrameSet
{
    std::vector<D3D11IndexedCameraFrame> frames;
    std::uint64_t syncGroupId = 0;
    std::chrono::steady_clock::time_point emittedTime{};
};

using D3D11FrameQueue = ThreadKit::Queues::BlockingQueue<D3D11CameraFrame>;
using D3D11IndexedFrameQueue = ThreadKit::Queues::BlockingQueue<D3D11IndexedCameraFrame>;
using D3D11SyncedFrameQueue = ThreadKit::Queues::BlockingQueue<D3D11SyncedFrameSet>;

struct ReadResult
{
    bool ok = false;
    D3D11CameraFrame frame;
    ErrorInfo error;

    explicit operator bool() const noexcept { return ok; }
};

} // namespace IC4Ext
