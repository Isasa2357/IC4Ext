#pragma once

#include "IC4Ext/V2/Core/FrameSyncTypes.hpp"
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrame.hpp"
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrameSet.hpp"

#include <ThreadKit/Queues/BlockingQueue.hpp>

namespace IC4Ext::V2 {

struct D3D12IndexedReadOnlyCameraFrame
{
    CameraId cameraId = 0;
    D3D12ReadOnlyFrame frame;
};

using D3D12IndexedReadOnlyFrameQueue =
    ThreadKit::Queues::BlockingQueue<D3D12IndexedReadOnlyCameraFrame>;

using D3D12ReadOnlyFrameSetQueue =
    ThreadKit::Queues::BlockingQueue<D3D12ReadOnlyFrameSet>;

} // namespace IC4Ext::V2
