#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D11/CameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11BackendContext.hpp"
#include "IC4Ext/D3D11/FrameQueues.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrameSource.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace IC4Ext::D3D11 {

struct D3D11CameraCaptureThreadOptions
{
    std::uint32_t readTimeoutMs = 1000;
    bool stopOnReadError = false;
    bool isValid() const noexcept { return readTimeoutMs > 0; }
};

struct D3D11CameraCaptureThreadStats
{
    std::uint64_t readFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t pushedFrames = 0;
    std::uint64_t droppedOldestAndPushed = 0;
    std::uint64_t pushFailures = 0;
    std::uint64_t noOutputDrops = 0;
};

class D3D11ReadOnlyCameraCaptureThread final
{
public:
    D3D11ReadOnlyCameraCaptureThread(
        CameraId cameraId,
        IC4DeviceSelector selector,
        CameraCaptureConfig config,
        D3D11BackendContext backend,
        D3D11CameraCaptureOptions captureOptions = {},
        D3D11CameraCaptureThreadOptions threadOptions = {});

    D3D11ReadOnlyCameraCaptureThread(
        CameraId cameraId,
        D3D11ReadOnlyCameraCapture&& capture,
        D3D11CameraCaptureThreadOptions threadOptions = {});

    D3D11ReadOnlyCameraCaptureThread(
        CameraId cameraId,
        std::shared_ptr<ReadOnlyFrameSource> source,
        D3D11CameraCaptureThreadOptions threadOptions = {});

    ~D3D11ReadOnlyCameraCaptureThread();

    D3D11ReadOnlyCameraCaptureThread(
        const D3D11ReadOnlyCameraCaptureThread&) = delete;
    D3D11ReadOnlyCameraCaptureThread& operator=(
        const D3D11ReadOnlyCameraCaptureThread&) = delete;
    D3D11ReadOnlyCameraCaptureThread(
        D3D11ReadOnlyCameraCaptureThread&&) = delete;
    D3D11ReadOnlyCameraCaptureThread& operator=(
        D3D11ReadOnlyCameraCaptureThread&&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    bool isRunning() const noexcept;
    bool isOpened() const noexcept;
    CameraId cameraId() const noexcept;

    void setOutputQueue(std::shared_ptr<D3D11IndexedReadOnlyFrameQueue> queue);
    std::shared_ptr<D3D11IndexedReadOnlyFrameQueue> outputQueue() const;

    bool startAcquisition();
    bool stopAcquisition();
    bool softwareTrigger(const std::string& commandName = "TriggerSoftware");
    bool setIC4Property(const std::string& propertyName, bool value);
    bool setIC4Property(const std::string& propertyName, std::int64_t value);
    bool setIC4Property(const std::string& propertyName, double value);
    bool setIC4Property(const std::string& propertyName, const std::string& value);

    D3D11CameraCaptureThreadStats stats() const;
    CameraCaptureStats captureStats() const;
    D3D11FramePoolStats framePoolStats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

using CameraCaptureThreadOptions = D3D11CameraCaptureThreadOptions;
using CameraCaptureThreadStats = D3D11CameraCaptureThreadStats;
using CameraCaptureThread = D3D11ReadOnlyCameraCaptureThread;

} // namespace IC4Ext::D3D11
