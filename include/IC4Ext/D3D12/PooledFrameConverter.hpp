#pragma once

#include "IC4Ext/D3D12/FramePool.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12PooledFrameConverter.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using PooledFrameConverterStats = D3D12PooledFrameConverterStats;
using PooledFrameConverter = D3D12PooledFrameConverter;

} // namespace IC4Ext::D3D12
