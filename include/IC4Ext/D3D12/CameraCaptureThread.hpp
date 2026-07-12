#pragma once

#include "IC4Ext/D3D12/CameraCapture.hpp"
#include "IC4Ext/D3D12/FrameQueues.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameSource.hpp"

#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12CameraCaptureThread.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using CameraCaptureThreadOptions = D3D12CameraCaptureThreadOptions;
using CameraCaptureThreadStats = D3D12CameraCaptureThreadStats;
using CameraCaptureThread = D3D12CameraCaptureThread;

} // namespace IC4Ext::D3D12
