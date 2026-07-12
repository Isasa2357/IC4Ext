#pragma once

#include "IC4Ext/D3D12/FrameSyncTypes.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrameSet.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using IndexedReadOnlyFrame = D3D12IndexedReadOnlyFrame;
using ReadOnlyFrameSet = D3D12ReadOnlyFrameSet;

} // namespace IC4Ext::D3D12
