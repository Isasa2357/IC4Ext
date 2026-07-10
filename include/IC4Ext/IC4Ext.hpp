#pragma once

#include "IC4Ext/BackendConfig.hpp"
#include "IC4Ext/Version.hpp"
#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/Core/CameraControl.hpp"
#include "IC4Ext/Core/CpuFrame.hpp"

#if IC4EXT_ENABLE_D3D11
#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"
#include "IC4Ext/D3D11/D3D11Camera.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"
#include "IC4Ext/D3D11/D3D11FrameConverter.hpp"
#include "IC4Ext/D3D11/D3D11FrameCopier.hpp"
#include "IC4Ext/D3D11/D3D11FrameReadback.hpp"
#include "IC4Ext/D3D11/D3D11CameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11DummyCameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11DummyCameraCaptureGenerator.hpp"
#include "IC4Ext/D3D11/D3D11CameraCaptureThread.hpp"
#include "IC4Ext/D3D11/D3D11FrameSyncThread.hpp"
#endif

#if IC4EXT_ENABLE_D3D12
#include "IC4Ext/D3D12/D3D12ReadyToken.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12Camera.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"
#include "IC4Ext/D3D12/D3D12FrameConverter.hpp"
#include "IC4Ext/D3D12/D3D12FrameCopier.hpp"
#include "IC4Ext/D3D12/D3D12FrameReadback.hpp"
#include "IC4Ext/D3D12/D3D12CameraCapture.hpp"
#include "IC4Ext/D3D12/D3D12CameraCaptureThread.hpp"
#include "IC4Ext/D3D12/D3D12DummyCameraCapture.hpp"
#include "IC4Ext/D3D12/D3D12DummyCameraCaptureGenerator.hpp"
#include "IC4Ext/D3D12/D3D12FrameSyncThread.hpp"
#endif
