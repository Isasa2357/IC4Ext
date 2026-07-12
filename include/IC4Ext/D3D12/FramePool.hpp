#pragma once

#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"
#include "IC4Ext/V2/D3D12/D3D12FramePool.hpp"

namespace IC4Ext::D3D12 {

using FramePoolExhaustionPolicy = ::IC4Ext::V2::FramePoolExhaustionPolicy;
using FramePoolConfig = ::IC4Ext::V2::D3D12FramePoolConfig;
using FramePoolStats = ::IC4Ext::V2::D3D12FramePoolStats;
using FrameWriter = ::IC4Ext::V2::D3D12FrameWriter;
using FramePool = ::IC4Ext::V2::D3D12FramePool;

} // namespace IC4Ext::D3D12
