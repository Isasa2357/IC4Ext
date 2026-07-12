#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/V2/D3D12/D3D12FramePool.hpp"
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrame.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace IC4Ext::V2 {

struct D3D12CameraCaptureOptions
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

struct D3D12ReadOnlyReadResult
{
    bool ok = false;
    D3D12ReadOnlyFrame frame;
    ErrorInfo error;

    explicit operator bool() const noexcept { return ok; }
};

// D3D12 capture path for IC4Ext v2.
//
// The class owns the producer-side frame pool. Every successful read publishes
// one immutable D3D12ReadOnlyFrame. The UploadRing and conversion command slots
// remain private implementation details; consumers only receive the completed
// output texture, its SRV and the producer-ready fence token.
class D3D12CameraCapture final
{
public:
    D3D12CameraCapture();
    ~D3D12CameraCapture();

    D3D12CameraCapture(const D3D12CameraCapture&) = delete;
    D3D12CameraCapture& operator=(const D3D12CameraCapture&) = delete;

    D3D12CameraCapture(D3D12CameraCapture&& other) noexcept;
    D3D12CameraCapture& operator=(D3D12CameraCapture&& other) noexcept;

    bool open(const IC4DeviceSelector& selector,
              const CameraCaptureConfig& config,
              D3D12BackendContext backend,
              D3D12CameraCaptureOptions options = {});

    void close() noexcept;
    bool isOpened() const noexcept { return opened_.load(); }

    bool startAcquisition();
    bool stopAcquisition();
    bool isStreaming() const noexcept;
    bool isAcquisitionActive() const noexcept;

    D3D12ReadOnlyReadResult read(ReadMode mode = ReadMode::LatestFrame);
    D3D12ReadOnlyReadResult read(const CameraReadOptions& options);

    bool applyIC4StateJson(const std::filesystem::path& jsonPath,
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
    D3D12FramePoolStats framePoolStats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> opened_{false};

    void moveFrom(D3D12CameraCapture&& other) noexcept;
};

} // namespace IC4Ext::V2
