#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "IC4Ext/Core/MultiCameraStartup.hpp"
#include "IC4Ext/D3D12/CameraCapture.hpp"
#include "IC4Ext/D3D12/CameraCaptureThread.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/FrameQueues.hpp"

namespace IC4Ext::D3D12 {

using MultiCameraStartupOptions = ::IC4Ext::MultiCameraStartupOptions;

// Configuration for a camera that remains a CameraCapture and is returned to
// the caller for direct read() calls.
struct CameraCaptureStartupConfig
{
    CameraId cameraId = 0;
    IC4DeviceSelector selector;
    CameraCaptureConfig captureConfig;
    CameraCaptureOptions captureOptions;

    // Direct and threaded configurations are merged and opened in ascending
    // openOrder. Equal values preserve the helper's stable insertion order.
    std::uint64_t openOrder = 0;
};

// Configuration for a camera whose prepared CameraCapture is moved into a
// CameraCaptureThread. The helper starts the worker and acquisition before it
// returns. The output queue is supplied by the caller; FrameSyncThread is not
// owned or otherwise managed by this helper.
struct CameraCaptureThreadStartupConfig
{
    CameraCaptureStartupConfig capture;
    CameraCaptureThreadOptions threadOptions;
    std::shared_ptr<IndexedReadOnlyFrameQueue> outputQueue;
};

class StartedCameraCapture final
{
public:
    StartedCameraCapture() = default;

    StartedCameraCapture(CameraId id, CameraCapture&& value)
        : cameraId(id), capture(std::move(value))
    {
    }

    StartedCameraCapture(const StartedCameraCapture&) = delete;
    StartedCameraCapture& operator=(const StartedCameraCapture&) = delete;
    StartedCameraCapture(StartedCameraCapture&&) = default;
    StartedCameraCapture& operator=(StartedCameraCapture&&) = default;

    CameraId cameraId = 0;
    CameraCapture capture;
};

struct MultiCameraStartupResult
{
    MultiCameraStartupResult() = default;
    MultiCameraStartupResult(const MultiCameraStartupResult&) = delete;
    MultiCameraStartupResult& operator=(const MultiCameraStartupResult&) = delete;
    MultiCameraStartupResult(MultiCameraStartupResult&&) = default;
    MultiCameraStartupResult& operator=(MultiCameraStartupResult&&) = default;

    bool ok = false;

    // Cameras configured for direct CameraCapture::read() access.
    std::vector<StartedCameraCapture> captures;

    // CameraCaptureThread is intentionally non-movable, so instances are
    // returned through unique ownership.
    std::vector<std::unique_ptr<CameraCaptureThread>> captureThreads;

    ErrorInfo error;

    explicit operator bool() const noexcept { return ok; }
};

// Opens every requested camera with Immediate acquisition, pauses each camera,
// starts all requested CameraCaptureThread workers, and finally starts every
// acquisition in the merged openOrder. On failure, all partially started work
// is stopped and the returned resource vectors are empty.
inline MultiCameraStartupResult OpenAndStartMultiCameraGroup(
    const D3D12BackendContext& backend,
    const std::vector<CameraCaptureStartupConfig>& captureConfigs,
    const std::vector<CameraCaptureThreadStartupConfig>& captureThreadConfigs,
    MultiCameraStartupOptions options = {})
{
    return ::IC4Ext::Detail::OpenAndStartMultiCameraGroupImpl<
        D3D12BackendContext,
        CameraCapture,
        CameraCaptureThread,
        CameraCaptureStartupConfig,
        CameraCaptureThreadStartupConfig,
        StartedCameraCapture,
        MultiCameraStartupResult>(
            backend,
            captureConfigs,
            captureThreadConfigs,
            options);
}

} // namespace IC4Ext::D3D12
