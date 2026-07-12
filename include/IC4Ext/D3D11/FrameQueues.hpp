#pragma once

#include "IC4Ext/D3D11/FrameSyncTypes.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrameSet.hpp"

#include <ThreadKit/Queues/BlockingQueue.hpp>

namespace IC4Ext::D3D11 {

struct D3D11IndexedReadOnlyCameraFrame
{
    CameraId cameraId = 0;
    D3D11ReadOnlyFrame frame;
};

using D3D11IndexedReadOnlyFrameQueue =
    ThreadKit::Queues::BlockingQueue<D3D11IndexedReadOnlyCameraFrame>;
using D3D11ReadOnlyFrameSetQueue =
    ThreadKit::Queues::BlockingQueue<D3D11ReadOnlyFrameSet>;

using IndexedReadOnlyCameraFrame = D3D11IndexedReadOnlyCameraFrame;
using IndexedReadOnlyFrameQueue = D3D11IndexedReadOnlyFrameQueue;
using ReadOnlyFrameSetQueue = D3D11ReadOnlyFrameSetQueue;

} // namespace IC4Ext::D3D11
