#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D11/D3D11Camera.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

namespace IC4Ext {

class D3D11FenceManager;
class D3D11FrameConverter;

class D3D11CameraCapture : public ID3D11Camera
{
public:
    D3D11CameraCapture();
    ~D3D11CameraCapture();

    D3D11CameraCapture(const D3D11CameraCapture&) = delete;
    D3D11CameraCapture& operator=(const D3D11CameraCapture&) = delete;

    D3D11CameraCapture(D3D11CameraCapture&& other) noexcept;
    D3D11CameraCapture& operator=(D3D11CameraCapture&& other) noexcept;

    bool open(const IC4DeviceSelector& selector,
              const CameraCaptureConfig& config,
              D3D11CoreLib::D3D11Core* core);

    void close() noexcept;
    bool isOpened() const noexcept override { return opened_.load(); }

    ReadResult read(ReadMode mode = ReadMode::LatestFrame) override;
    ReadResult read(const CameraReadOptions& options) override;

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
    void moveFrom(D3D11CameraCapture&& other) noexcept;
};

} // namespace IC4Ext
