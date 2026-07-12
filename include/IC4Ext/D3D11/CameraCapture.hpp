#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D11/D3D11BackendContext.hpp"
#include "IC4Ext/D3D11/FramePool.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace IC4Ext::D3D11 {

struct D3D11CameraCaptureOptions
{
    std::size_t initialFramePoolCapacity = 8;
    std::size_t maxFramePoolCapacity = 32;
    FramePoolExhaustionPolicy framePoolExhaustionPolicy =
        FramePoolExhaustionPolicy::DropNewest;
    std::chrono::milliseconds framePoolWaitTimeout{5};

    bool isValid() const noexcept
    {
        return initialFramePoolCapacity > 0 &&
               maxFramePoolCapacity >= initialFramePoolCapacity &&
               framePoolWaitTimeout.count() >= 0;
    }
};

struct D3D11ReadOnlyReadResult
{
    bool ok = false;
    D3D11ReadOnlyFrame frame;
    ErrorInfo error;
    explicit operator bool() const noexcept { return ok; }
};

class D3D11ReadOnlyCameraCapture final
{
public:
    D3D11ReadOnlyCameraCapture();
    ~D3D11ReadOnlyCameraCapture();

    D3D11ReadOnlyCameraCapture(const D3D11ReadOnlyCameraCapture&) = delete;
    D3D11ReadOnlyCameraCapture& operator=(const D3D11ReadOnlyCameraCapture&) = delete;
    D3D11ReadOnlyCameraCapture(D3D11ReadOnlyCameraCapture&& other) noexcept;
    D3D11ReadOnlyCameraCapture& operator=(D3D11ReadOnlyCameraCapture&& other) noexcept;

    bool open(
        const IC4DeviceSelector& selector,
        const CameraCaptureConfig& config,
        D3D11BackendContext backend,
        D3D11CameraCaptureOptions options = {});

    void close() noexcept;
    bool isOpened() const noexcept { return opened_.load(); }

    bool startAcquisition();
    bool stopAcquisition();
    bool isStreaming() const noexcept;
    bool isAcquisitionActive() const noexcept;

    D3D11ReadOnlyReadResult read(ReadMode mode = ReadMode::LatestFrame);
    D3D11ReadOnlyReadResult read(const CameraReadOptions& options);

    bool applyIC4StateJson(
        const std::filesystem::path& jsonPath,
        std::size_t deviceIndex = 0,
        bool strict = false,
        bool applyNestedSelectorStates = true);

    bool setIC4Property(const std::string& propertyName, bool value);
    bool setIC4Property(const std::string& propertyName, int value);
    bool setIC4Property(const std::string& propertyName, std::int64_t value);
    bool setIC4Property(const std::string& propertyName, double value);
    bool setIC4Property(const std::string& propertyName, const char* value);
    bool setIC4Property(const std::string& propertyName, const std::string& value);

    bool setFrameRate(double fps);
    bool setExposureAuto(const std::string& mode);
    bool setExposureTime(double exposureTimeUs);
    bool setGainAuto(const std::string& mode);
    bool setGain(double gain);
    bool setGamma(double gamma);
    bool setOffset(int offsetX, int offsetY);
    bool setRoi(int width, int height, int offsetX, int offsetY);
    bool setPixelFormat(CameraPixelFormat format);
    bool softwareTrigger(const std::string& commandName = "TriggerSoftware");

    CameraCaptureStats stats() const;
    CameraPerformanceSnapshot performance();
    D3D11FramePoolStats framePoolStats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> opened_{false};

    void moveFrom(D3D11ReadOnlyCameraCapture&& other) noexcept;
};

using CameraCaptureOptions = D3D11CameraCaptureOptions;
using ReadResult = D3D11ReadOnlyReadResult;
using CameraCapture = D3D11ReadOnlyCameraCapture;

} // namespace IC4Ext::D3D11
