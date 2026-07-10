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
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace IC4Ext {

class D3D11CameraCaptureThread
{
public:
    D3D11CameraCaptureThread(IC4DeviceSelector selector,
                             CameraCaptureConfig config,
                             D3D11CoreLib::D3D11Core* core,
                             CameraThreadOptions options = {});

    explicit D3D11CameraCaptureThread(D3D11CameraCapture&& capture,
                                      CameraThreadOptions options = {});

    explicit D3D11CameraCaptureThread(std::shared_ptr<ID3D11Camera> source,
                                      CameraThreadOptions options = {});

    ~D3D11CameraCaptureThread();

    D3D11CameraCaptureThread(const D3D11CameraCaptureThread&) = delete;
    D3D11CameraCaptureThread& operator=(const D3D11CameraCaptureThread&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    void addOutputQueue(std::uint32_t cameraIndex,
                        std::shared_ptr<D3D11IndexedFrameQueue> queue);

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
    bool setPixelFormat(CameraPixelFormat fmt);
    bool softwareTrigger(const std::string& commandName = "TriggerSoftware")
    {
        std::lock_guard<std::mutex> sourceLock(sourceMutex_);
        if (!source_ || !source_->isOpened()) {
            setError(ErrorCode::NotOpened,
                     "D3D11CameraCaptureThread::softwareTrigger",
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
        std::shared_ptr<D3D11IndexedFrameQueue> queue;
    };

    void workerLoop();
    void dispatchFrame(D3D11CameraFrame&& frame);
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
