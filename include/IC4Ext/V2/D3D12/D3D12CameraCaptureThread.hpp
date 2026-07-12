#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/V2/Core/FrameSyncTypes.hpp"
#include "IC4Ext/V2/D3D12/D3D12CameraCapture.hpp"
#include "IC4Ext/V2/D3D12/D3D12FrameQueues.hpp"

#include <cstdint>
#include <memory>

namespace IC4Ext::V2 {

struct D3D12CameraCaptureThreadOptions
{
    std::uint32_t readTimeoutMs = 1000;
    bool stopOnReadError = false;

    bool isValid() const noexcept { return readTimeoutMs > 0; }
};

struct D3D12CameraCaptureThreadStats
{
    std::uint64_t readFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t pushedFrames = 0;
    std::uint64_t droppedOldestAndPushed = 0;
    std::uint64_t pushFailures = 0;
    std::uint64_t noOutputDrops = 0;
};

// Continuously reads immutable frames from one v2 capture and publishes them to
// the single central FrameSyncThread ingress queue. There is intentionally no
// per-output GPU copy or fan-out in this class; fan-out belongs to the central
// D3D12FrameSyncThread after a complete synchronized set has been assembled.
class D3D12CameraCaptureThread final
{
public:
    D3D12CameraCaptureThread(
        CameraId cameraId,
        IC4DeviceSelector selector,
        CameraCaptureConfig config,
        D3D12BackendContext backend,
        D3D12CameraCaptureOptions captureOptions = {},
        D3D12CameraCaptureThreadOptions threadOptions = {});

    D3D12CameraCaptureThread(
        CameraId cameraId,
        D3D12CameraCapture&& capture,
        D3D12CameraCaptureThreadOptions threadOptions = {});

    ~D3D12CameraCaptureThread();

    D3D12CameraCaptureThread(const D3D12CameraCaptureThread&) = delete;
    D3D12CameraCaptureThread& operator=(const D3D12CameraCaptureThread&) = delete;
    D3D12CameraCaptureThread(D3D12CameraCaptureThread&&) = delete;
    D3D12CameraCaptureThread& operator=(D3D12CameraCaptureThread&&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    bool isRunning() const noexcept;
    bool isOpened() const noexcept;
    CameraId cameraId() const noexcept;

    // May be replaced while the worker is running. The new queue is used by the
    // next frame after the worker takes a fresh output snapshot.
    void setOutputQueue(std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> queue);
    std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> outputQueue() const;

    bool startAcquisition();
    bool stopAcquisition();
    bool softwareTrigger(const std::string& commandName = "TriggerSoftware");

    bool setIC4Property(const std::string& propertyName, bool value);
    bool setIC4Property(const std::string& propertyName, std::int64_t value);
    bool setIC4Property(const std::string& propertyName, double value);
    bool setIC4Property(const std::string& propertyName, const std::string& value);

    D3D12CameraCaptureThreadStats stats() const;
    CameraCaptureStats captureStats() const;
    D3D12FramePoolStats framePoolStats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace IC4Ext::V2
