#pragma once

#include "IC4Ext/D3D12/D3D12Camera.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace IC4Ext {

class D3D12DummyCameraCapture final : public ID3D12Camera
{
public:
    D3D12DummyCameraCapture(std::uint32_t cameraIndex,
                     std::shared_ptr<D3D12FrameQueue> frameQueue,
                     std::weak_ptr<ID3D12CameraControlSink> controlSink);

    ~D3D12DummyCameraCapture() override = default;

    D3D12DummyCameraCapture(const D3D12DummyCameraCapture&) = delete;
    D3D12DummyCameraCapture& operator=(const D3D12DummyCameraCapture&) = delete;

    std::uint32_t cameraIndex() const noexcept { return cameraIndex_; }
    std::shared_ptr<D3D12FrameQueue> frameQueue() const noexcept { return frameQueue_; }

    bool isOpened() const noexcept override { return opened_.load(); }
    void close() noexcept;

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

    const ErrorInfo& lastError() const noexcept override { return lastError_; }

private:
    bool sendControlCommand(const CameraControlCommand& command);
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    std::uint32_t cameraIndex_ = 0;
    std::shared_ptr<D3D12FrameQueue> frameQueue_;
    std::weak_ptr<ID3D12CameraControlSink> controlSink_;
    std::atomic<bool> opened_{true};
    ErrorInfo lastError_;
};

} // namespace IC4Ext
