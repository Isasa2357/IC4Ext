#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12Camera.hpp"
#include "IC4Ext/D3D12/D3D12CameraCapture.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"
#include "IC4Ext/D3D12/D3D12FrameCopier.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace IC4Ext {

class D3D12CameraCaptureThread
{
public:
    D3D12CameraCaptureThread(IC4DeviceSelector selector,
                             CameraCaptureConfig config,
                             D3D12BackendContext backend,
                             CameraThreadOptions options = {});

    explicit D3D12CameraCaptureThread(D3D12CameraCapture&& capture,
                                      D3D12BackendContext backend,
                                      CameraThreadOptions options = {});

    explicit D3D12CameraCaptureThread(std::shared_ptr<ID3D12Camera> source,
                                      D3D12BackendContext backend = {},
                                      CameraThreadOptions options = {});

    ~D3D12CameraCaptureThread();

    D3D12CameraCaptureThread(const D3D12CameraCaptureThread&) = delete;
    D3D12CameraCaptureThread& operator=(const D3D12CameraCaptureThread&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    void addOutputQueue(std::uint32_t cameraIndex,
                        std::shared_ptr<D3D12IndexedFrameQueue> queue);

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

    bool startAcquisition()
    {
        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        if (!source_ || !source_->isOpened()) {
            setError(ErrorCode::NotOpened,
                     "D3D12CameraCaptureThread::startAcquisition",
                     "Source is not opened");
            return false;
        }
        const bool ok = source_->startAcquisition();
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }

    bool stopAcquisition()
    {
        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        if (!source_ || !source_->isOpened()) {
            setError(ErrorCode::NotOpened,
                     "D3D12CameraCaptureThread::stopAcquisition",
                     "Source is not opened");
            return false;
        }
        const bool ok = source_->stopAcquisition();
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }

    bool isStreaming() const noexcept
    {
        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        return source_ && source_->isStreaming();
    }

    bool isAcquisitionActive() const noexcept
    {
        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        return source_ && source_->isAcquisitionActive();
    }

    bool setFrameRate(double fps);
    bool setExposureAuto(const std::string& mode);
    bool setExposureTime(double exposureTimeUs);
    bool setGainAuto(const std::string& mode);
    bool setGain(double gain);
    bool setGamma(double gamma);
    bool setOffset(int offsetX, int offsetY);
    bool setRoi(int width, int height, int offsetX, int offsetY);
    bool setPixelFormat(CameraPixelFormat fmt);
    bool softwareTrigger(const std::string& commandName = "TriggerSoftware")
    {
        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        if (!source_ || !source_->isOpened()) {
            setError(ErrorCode::NotOpened,
                     "D3D12CameraCaptureThread::softwareTrigger",
                     "Source is not opened");
            return false;
        }
        const bool ok = source_->softwareTrigger(commandName.empty() ? std::string("TriggerSoftware") : commandName);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }

    CameraThreadStats stats() const;
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    struct OutputBinding
    {
        std::uint32_t cameraIndex = 0;
        std::shared_ptr<D3D12IndexedFrameQueue> queue;
    };

    enum class SourceMode { InternalCapture, MovedCapture, ExternalSource };

    void workerLoop();
    void dispatchFrame(D3D12CameraFrame&& frame);
    void setError(ErrorCode code, const char* where, const std::string& message);

    IC4DeviceSelector selector_;
    CameraCaptureConfig config_;
    D3D12BackendContext backend_;
    CameraThreadOptions options_;
    SourceMode sourceMode_ = SourceMode::InternalCapture;

    std::shared_ptr<D3D12CameraCapture> ownedCapture_;
    std::shared_ptr<ID3D12Camera> source_;
    mutable std::mutex sourceMutex_;

    std::unique_ptr<D3D12FenceManager> copyFenceManager_;
    D3D12FrameCopier copier_;

    mutable std::mutex outputMutex_;
    std::vector<OutputBinding> outputs_;

    mutable std::mutex statsMutex_;
    CameraThreadStats stats_;
    ErrorInfo lastError_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace IC4Ext
