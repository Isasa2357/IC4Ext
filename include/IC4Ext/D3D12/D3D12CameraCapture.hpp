#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12Camera.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

namespace IC4Ext {

class D3D12FenceManager;
class D3D12FrameConverter;

class D3D12CameraCapture : public ID3D12Camera
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
              const D3D12BackendContext& backend);

    bool open(const IC4DeviceSelector& selector,
              const CameraCaptureConfig& config,
              ID3D12Device* device,
              ID3D12CommandQueue* commandQueue)
    {
        (void)selector;
        (void)config;
        (void)device;
        (void)commandQueue;
        setError(ErrorCode::InvalidArgument,
                 "D3D12CameraCapture::open",
                 "Raw ID3D12Device/ID3D12CommandQueue initialization is intentionally unsupported in the helper-integrated backend. Use D3D12BackendContext::FromCore(...).");
        return false;
    }

    void close() noexcept;
    bool isOpened() const noexcept override { return opened_.load(); }

    // Typed lifecycle API intentionally lives on the concrete capture class so the
    // existing ID3D12Camera vtable remains ABI-compatible.
    bool startAcquisition();
    bool stopAcquisition();
    bool isStreaming() const noexcept;
    bool isAcquisitionActive() const noexcept;

    D3D12ReadResult read(ReadMode mode = ReadMode::LatestFrame) override;
    D3D12ReadResult read(const CameraReadOptions& options) override;

    bool applyIC4StateJson(const std::filesystem::path& jsonPath,
                           std::size_t deviceIndex = 0,
                           bool strict = false,
                           bool applyNestedSelectorStates = true) override;

    bool setIC4Property(const std::string& propertyName, bool value) override;
    bool setIC4Property(const std::string& propertyName, int value) override;
    bool setIC4Property(const std::string& propertyName, std::int64_t value) override;
    bool setIC4Property(const std::string& propertyName, double value) override;
    bool setIC4Property(const std::string& propertyName, const char* value) override;
    bool setIC4Property(const std::string& propertyName, const std::string& value) override;

    bool setFrameRate(double fps) override;
    bool setExposureAuto(const std::string& mode) override;
    bool setExposureTime(double exposureTimeUs) override;
    bool setGainAuto(const std::string& mode) override;
    bool setGain(double gain) override;
    bool setGamma(double gamma) override;
    bool setOffset(int offsetX, int offsetY) override;
    bool setRoi(int width, int height, int offsetX, int offsetY) override;
    bool setPixelFormat(CameraPixelFormat fmt) override;
    bool softwareTrigger(const std::string& commandName = "TriggerSoftware") override
    {
        return setIC4Property(commandName.empty() ? std::string("TriggerSoftware") : commandName,
                              std::string("execute"));
    }

    CameraCaptureStats stats() const;
    CameraPerformanceSnapshot performance();
    const ErrorInfo& lastError() const noexcept override { return lastError_; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> opened_{false};
    mutable std::mutex statsMutex_;
    CameraCaptureStats stats_;
    ErrorInfo lastError_;

    void setError(ErrorCode code, const std::string& where, const std::string& message);
    void moveFrom(D3D12CameraCapture&& other) noexcept;
};

} // namespace IC4Ext
