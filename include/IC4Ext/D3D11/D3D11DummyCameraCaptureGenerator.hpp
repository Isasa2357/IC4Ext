#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/Core/IC4DeviceSelector.hpp"
#include "IC4Ext/D3D11/D3D11Camera.hpp"
#include "IC4Ext/D3D11/D3D11CameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11DummyCameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace IC4Ext {

struct D3D11DummyCameraCaptureGeneratorOptions
{
    std::uint32_t readTimeoutMs = 1000;
    std::size_t dummyQueueCapacity = 1;
    FrameQueuePolicy dummyQueuePolicy = FrameQueuePolicy::LatestOnly;
    bool stopOnReadError = false;
};

struct D3D11DummyCameraCaptureGeneratorStats
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

class D3D11DummyCameraCaptureGenerator
{
public:
    D3D11DummyCameraCaptureGenerator(IC4DeviceSelector selector,
                              CameraCaptureConfig config,
                              D3D11CoreLib::D3D11Core* core,
                              D3D11DummyCameraCaptureGeneratorOptions options = {});

    explicit D3D11DummyCameraCaptureGenerator(D3D11CameraCapture&& capture,
                                       D3D11CoreLib::D3D11Core* core,
                                       D3D11DummyCameraCaptureGeneratorOptions options = {});

    ~D3D11DummyCameraCaptureGenerator();

    D3D11DummyCameraCaptureGenerator(const D3D11DummyCameraCaptureGenerator&) = delete;
    D3D11DummyCameraCaptureGenerator& operator=(const D3D11DummyCameraCaptureGenerator&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    std::shared_ptr<D3D11DummyCameraCapture> createDummyCameraCapture(std::uint32_t cameraIndex);

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

    D3D11DummyCameraCaptureGeneratorStats stats() const;
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    class ControlSink;

    struct OutputEndpoint
    {
        std::weak_ptr<D3D11FrameQueue> queue;
    };

    void workerLoop();
    bool fanOutFrame(const D3D11CameraFrame& sourceFrame);
    bool copyFrameNoSignal(const D3D11CameraFrame& src, D3D11CameraFrame& dst);
    bool applyControlCommandToConfig(const CameraControlCommand& command);
    bool applyControlCommandToCapture(const CameraControlCommand& command);
    void setError(ErrorCode code, const std::string& where, const std::string& message);
    void incrementExpiredOutputs(std::uint64_t n = 1);

    IC4DeviceSelector selector_;
    CameraCaptureConfig config_;
    D3D11CoreLib::D3D11Core* core_ = nullptr;
    D3D11DummyCameraCaptureGeneratorOptions options_;

    bool useExternalMovedCapture_ = false;
    D3D11CameraCapture capture_;
    mutable std::mutex captureMutex_;

    std::unique_ptr<D3D11FenceManager> copyFenceManager_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    std::shared_ptr<ID3D11CameraControlSink> controlSink_;

    mutable std::mutex outputMutex_;
    std::vector<OutputEndpoint> outputs_;

    mutable std::mutex statsMutex_;
    D3D11DummyCameraCaptureGeneratorStats stats_;
    ErrorInfo lastError_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace IC4Ext
