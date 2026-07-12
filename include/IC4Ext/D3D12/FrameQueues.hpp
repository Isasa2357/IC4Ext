#pragma once

#include "IC4Ext/D3D12/FrameSyncTypes.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameSet.hpp"

#include <ThreadKit/Queues/BlockingQueue.hpp>

namespace IC4Ext::D3D12 {

struct D3D12IndexedReadOnlyCameraFrame
{
    CameraId cameraId = 0;
    D3D12ReadOnlyFrame frame;
};

using D3D12IndexedReadOnlyFrameQueue =
    ThreadKit::Queues::BlockingQueue<D3D12IndexedReadOnlyCameraFrame>;

using D3D12ReadOnlyFrameSetQueue =
    ThreadKit::Queues::BlockingQueue<D3D12ReadOnlyFrameSet>;

using IndexedReadOnlyCameraFrame = D3D12IndexedReadOnlyCameraFrame;
using IndexedReadOnlyFrameQueue = D3D12IndexedReadOnlyFrameQueue;
using ReadOnlyFrameSetQueue = D3D12ReadOnlyFrameSetQueue;

} // namespace IC4Ext::D3D12
