#pragma once

#include "IC4Ext/D3D12/FrameQueues.hpp"
#include "IC4Ext/D3D12/FrameSyncOutputConfig.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12FrameSyncThread.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using FrameSyncThread = D3D12FrameSyncThread;

} // namespace IC4Ext::D3D12
