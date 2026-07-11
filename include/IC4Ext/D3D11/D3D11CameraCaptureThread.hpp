#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D11/D3D11Camera.hpp"
#include "IC4Ext/D3D11/D3D11CameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11FrameCopier.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace IC4Ext {

class D3D11FrameResizer;

class D3D11CameraCaptureThread
{
public:
    D3D11CameraCaptureThread(IC4DeviceSelector selector,
                             CameraCaptureConfig config,
                             D3D11CoreLib::D3D11Core* core,
                             CameraThreadOptions options = {});

    explicit D3D11CameraCaptureThread(D3D11CameraCapture&& capture,
                                      CameraThreadOptions options = {});

    D3D11CameraCaptureThread(D3D11CameraCapture&& capture,
                             D3D11CoreLib::D3D11Core* core,
                             CameraThreadOptions options = {});

    explicit D3D11CameraCaptureThread(std::shared_ptr<ID3D11Camera> source,
                                      CameraThreadOptions options = {});

    D3D11CameraCaptureThread(std::shared_ptr<ID3D11Camera> source,
                             D3D11CoreLib::D3D11Core* core,
                             CameraThreadOptions options = {});

    ~D3D11CameraCaptureThread();

    D3D11CameraCaptureThread(const D3D11CameraCaptureThread&) = delete;
    D3D11CameraCaptureThread& operator=(const D3D11CameraCaptureThread&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    // Existing behavior: register an output that receives the original-sized frame.
    void addOutputQueue(std::uint32_t cameraIndex,
                        std::shared_ptr<D3D11IndexedFrameQueue> queue);

    // Register an output with optional per-queue GPU resize. {0,0} is passthrough.
    void addOutputQueue(std::uint32_t cameraIndex,
                        std::shared_ptr<D3D11IndexedFrameQueue> queue,
                        CameraOutputResizeOptions resize);

    std::size_t removeOutputQueue(
        std::uint32_t cameraIndex,
        const std::shared_ptr<D3D11IndexedFrameQueue>& queue)
    {
        if (!queue) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(outputMutex_);
        std::size_t removedCount = 0;
        for (auto it = outputs_.begin(); it != outputs_.end();) {
            if (it->cameraIndex == cameraIndex && it->queue == queue) {
                it = outputs_.erase(it);
                ++removedCount;
            } else {
                ++it;
            }
        }
        return removedCount;
    }

    std::size_t clearOutputQueues()
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        const std::size_t removedCount = outputs_.size();
        outputs_.clear();
        return removedCount;
    }

    std::size_t outputQueueCount() const
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        return outputs_.size();
    }

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

    // IC4Ext-owned captures are internally synchronized, so lifecycle control must
    // not wait behind the worker's blocking read. External camera implementations
    // remain serialized through sourceMutex_ for compatibility.
    bool startAcquisition()
    {
        const auto invoke = [this](const std::shared_ptr<ID3D11Camera>& source) {
            if (!source || !source->isOpened()) {
                setError(ErrorCode::NotOpened,
                         "D3D11CameraCaptureThread::startAcquisition",
                         "Source is not opened");
                return false;
            }

            bool ok = false;
            if (auto* capture = dynamic_cast<D3D11CameraCapture*>(source.get())) {
                ok = capture->startAcquisition();
            } else {
                ok = source->setIC4Property("AcquisitionStart", std::string("execute"));
            }
            lastError_ = ok ? NoError() : source->lastError();
            return ok;
        };

        if (sourceMode_ == SourceMode::ExternalSource) {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            return invoke(source_);
        }
        return invoke(source_);
    }

    bool stopAcquisition()
    {
        const auto invoke = [this](const std::shared_ptr<ID3D11Camera>& source) {
            if (!source || !source->isOpened()) {
                setError(ErrorCode::NotOpened,
                         "D3D11CameraCaptureThread::stopAcquisition",
                         "Source is not opened");
                return false;
            }

            bool ok = false;
            if (auto* capture = dynamic_cast<D3D11CameraCapture*>(source.get())) {
                ok = capture->stopAcquisition();
            } else {
                ok = source->setIC4Property("AcquisitionStop", std::string("execute"));
            }
            lastError_ = ok ? NoError() : source->lastError();
            return ok;
        };

        if (sourceMode_ == SourceMode::ExternalSource) {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            return invoke(source_);
        }
        return invoke(source_);
    }

    bool isStreaming() const noexcept
    {
        const auto query = [](const std::shared_ptr<ID3D11Camera>& source) noexcept {
            if (!source) return false;
            if (const auto* capture = dynamic_cast<const D3D11CameraCapture*>(source.get())) {
                return capture->isStreaming();
            }
            return source->isOpened();
        };

        if (sourceMode_ == SourceMode::ExternalSource) {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            return query(source_);
        }
        return query(source_);
    }

    bool isAcquisitionActive() const noexcept
    {
        const auto query = [](const std::shared_ptr<ID3D11Camera>& source) noexcept {
            if (!source) return false;
            if (const auto* capture = dynamic_cast<const D3D11CameraCapture*>(source.get())) {
                return capture->isAcquisitionActive();
            }
            return source->isOpened();
        };

        if (sourceMode_ == SourceMode::ExternalSource) {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            return query(source_);
        }
        return query(source_);
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
        const auto invoke = [this, &commandName](const std::shared_ptr<ID3D11Camera>& source) {
            if (!source || !source->isOpened()) {
                setError(ErrorCode::NotOpened,
                         "D3D11CameraCaptureThread::softwareTrigger",
                         "Source is not opened");
                return false;
            }
            const bool ok = source->softwareTrigger(
                commandName.empty() ? std::string("TriggerSoftware") : commandName);
            lastError_ = ok ? NoError() : source->lastError();
            return ok;
        };

        if (sourceMode_ == SourceMode::ExternalSource) {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            return invoke(source_);
        }
        return invoke(source_);
    }

    CameraThreadStats stats() const;
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    struct OutputBinding
    {
        std::uint32_t cameraIndex = 0;
        std::shared_ptr<D3D11IndexedFrameQueue> queue;
        CameraOutputResizeOptions resize;
    };

    void workerLoop();
    void dispatchFrame(D3D11CameraFrame&& frame);
    bool ensureResizer();
    void setError(ErrorCode code, const std::string& where, const std::string& message);
    bool hasRuntimeSource() const noexcept { return static_cast<bool>(source_); }

    IC4DeviceSelector selector_;
    CameraCaptureConfig config_;
    D3D11CoreLib::D3D11Core* core_ = nullptr;
    CameraThreadOptions options_;

    enum class SourceMode
    {
        InternalCapture,
        MovedCapture,
        ExternalSource,
    };
    SourceMode sourceMode_ = SourceMode::InternalCapture;
    std::shared_ptr<D3D11CameraCapture> ownedCapture_;
    std::shared_ptr<ID3D11Camera> source_;
    mutable std::mutex sourceMutex_;

    std::unique_ptr<D3D11FenceManager> copyFenceManager_;
    D3D11FrameCopier copier_;
    std::unique_ptr<D3D11FrameResizer> resizer_;

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
