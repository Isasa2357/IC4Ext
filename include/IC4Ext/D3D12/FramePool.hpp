#pragma once

#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12FramePool.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using FramePoolExhaustionPolicy = FramePoolExhaustionPolicy;
using FramePoolConfig = D3D12FramePoolConfig;
using FramePoolStats = D3D12FramePoolStats;
using FrameWriter = D3D12FrameWriter;
using FramePool = D3D12FramePool;

} // namespace IC4Ext::D3D12
