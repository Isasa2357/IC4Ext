#pragma once

#include "IC4Ext/D3D12/FramePool.hpp"
#include "IC4Ext/D3D12/PooledFrameConverter.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12CameraCapture.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using CameraCaptureOptions = D3D12CameraCaptureOptions;
using ReadResult = D3D12ReadOnlyReadResult;
using CameraCapture = D3D12CameraCapture;

} // namespace IC4Ext::D3D12
