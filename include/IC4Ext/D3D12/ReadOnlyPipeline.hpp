#pragma once

// Public D3D12 read-only frame pipeline API.
//
// New code should include this header, or the more focused headers next to it,
// and use the IC4Ext::D3D12 namespace.

#include "IC4Ext/D3D12/CameraCapture.hpp"
#include "IC4Ext/D3D12/CameraCaptureThread.hpp"
#include "IC4Ext/D3D12/FramePool.hpp"
#include "IC4Ext/D3D12/FrameQueues.hpp"
#include "IC4Ext/D3D12/FrameSyncOutputConfig.hpp"
#include "IC4Ext/D3D12/FrameSyncThread.hpp"
#include "IC4Ext/D3D12/FrameSyncTypes.hpp"
#include "IC4Ext/D3D12/PooledFrameConverter.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameLifetimeTracker.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameSet.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameSource.hpp"
