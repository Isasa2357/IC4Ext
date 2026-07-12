#pragma once

#include "IC4Ext/D3D12/ReadOnlyFrameSet.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12FrameQueues.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using IndexedReadOnlyCameraFrame = D3D12IndexedReadOnlyCameraFrame;
using IndexedReadOnlyFrameQueue = D3D12IndexedReadOnlyFrameQueue;
using ReadOnlyFrameSetQueue = D3D12ReadOnlyFrameSetQueue;

} // namespace IC4Ext::D3D12
