#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D12/D3D12Camera.hpp"
#include "IC4Ext/D3D12/D3D12CameraCapture.hpp"
#include "IC4Ext/D3D12/D3D12DummyCameraCapture.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"
#include "IC4Ext/D3D12/D3D12FrameCopier.hpp"

#include "IC4Ext/D3D12/D3D12BackendContext.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace IC4Ext {

struct D3D12DummyCameraCaptureGeneratorOptions
{
    std::uint32_t readTimeoutMs = 1000;
    std::size_t dummyQueueCapacity = 1;
    FrameQueuePolicy dummyQueuePolicy = FrameQueuePolicy::LatestOnly;
    bool stopOnReadError = false;
};

struct D3D12DummyCameraCaptureGeneratorStats
{
    std::uint64_t sourceFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t copiedFrames = 0;
    std::uint64_t copyFailures = 0;
    std::uint64_t pushedFrames = 0;
    std::uint64_t pushFailures = 0;
    std::uint64_t noOutputDrops = 0;
    std::uint64_t expiredOutputs = 0;
    std::uint64_t controlCommands = 0;
    std::uint64_t controlFailures = 0;
};

class D3D12DummyCameraCaptureGenerator
{
public:
    D3D12DummyCameraCaptureGenerator(IC4DeviceSelector selector,
                              CameraCaptureConfig config,
                              D3D12BackendContext backend,
                              D3D12DummyCameraCaptureGeneratorOptions options = {});

    explicit D3D12DummyCameraCaptureGenerator(D3D12CameraCapture&& capture,
                                       D3D12BackendContext backend,
                                       D3D12DummyCameraCaptureGeneratorOptions options = {});

    ~D3D12DummyCameraCaptureGenerator();

    D3D12DummyCameraCaptureGenerator(const D3D12DummyCameraCaptureGenerator&) = delete;
    D3D12DummyCameraCaptureGenerator& operator=(const D3D12DummyCameraCaptureGenerator&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    std::shared_ptr<D3D12DummyCameraCapture> createDummyCameraCapture(std::uint32_t cameraIndex);

    bool submitControlCommand(const CameraControlCommand& command);

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

    D3D12DummyCameraCaptureGeneratorStats stats() const;
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    class ControlSink;

    struct OutputEndpoint
    {
        std::weak_ptr<D3D12FrameQueue> queue;
    };

    void workerLoop();
    bool fanOutFrame(const D3D12CameraFrame& sourceFrame);
    bool applyControlCommandToConfig(const CameraControlCommand& command);
    bool applyControlCommandToCapture(const CameraControlCommand& command);
    void setError(ErrorCode code, const std::string& where, const std::string& message);
    void incrementExpiredOutputs(std::uint64_t n = 1);

    IC4DeviceSelector selector_;
    CameraCaptureConfig config_;
    D3D12BackendContext backend_;
    D3D12DummyCameraCaptureGeneratorOptions options_;

    bool useExternalMovedCapture_ = false;
    D3D12CameraCapture capture_;
    mutable std::mutex captureMutex_;

    std::unique_ptr<D3D12FenceManager> copyFenceManager_;
    D3D12FrameCopier copier_;

    std::shared_ptr<ID3D12CameraControlSink> controlSink_;

    mutable std::mutex outputMutex_;
    std::vector<OutputEndpoint> outputs_;

    mutable std::mutex statsMutex_;
    D3D12DummyCameraCaptureGeneratorStats stats_;
    ErrorInfo lastError_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace IC4Ext
