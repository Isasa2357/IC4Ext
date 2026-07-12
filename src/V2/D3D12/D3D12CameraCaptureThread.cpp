#include "IC4Ext/V2/D3D12/D3D12CameraCaptureThread.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace IC4Ext::V2 {

class D3D12CameraCaptureThread::Impl
{
public:
    enum class SourceMode
    {
        Internal,
        MovedCapture,
    };

    CameraId cameraId = 0;
    IC4DeviceSelector selector;
    CameraCaptureConfig captureConfig;
    D3D12BackendContext backend;
    D3D12CameraCaptureOptions captureOptions;
    D3D12CameraCaptureThreadOptions threadOptions;
    SourceMode sourceMode = SourceMode::Internal;

    std::unique_ptr<D3D12CameraCapture> capture;

    mutable std::mutex outputMutex;
    std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> output;

    mutable std::mutex statsMutex;
    D3D12CameraCaptureThreadStats threadStats;

    mutable std::mutex errorMutex;
    ErrorInfo error;

    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    std::thread worker;

    void setError(ErrorInfo value)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = std::move(value);
    }

    void setError(ErrorCode code, const char* where, const std::string& message)
    {
        setError(MakeError(code, where, message));
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = NoError();
    }

    ErrorInfo getError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return error;
    }

    D3D12CameraCaptureThreadStats getStats() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        return threadStats;
    }

    void incrementReadFrame()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.readFrames;
    }

    void incrementReadTimeout()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.readTimeouts;
    }

    void incrementReadError()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.readErrors;
    }

    void incrementNoOutputDrop()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.noOutputDrops;
    }

    void recordPush(ThreadKit::Queues::QueuePushResult result)
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        switch (result) {
        case ThreadKit::Queues::QueuePushResult::Pushed:
            ++threadStats.pushedFrames;
            break;
        case ThreadKit::Queues::QueuePushResult::DroppedOldestAndPushed:
            ++threadStats.pushedFrames;
            ++threadStats.droppedOldestAndPushed;
            break;
        case ThreadKit::Queues::QueuePushResult::Full:
        case ThreadKit::Queues::QueuePushResult::Closed:
        case ThreadKit::Queues::QueuePushResult::Stopped:
        default:
            ++threadStats.pushFailures;
            break;
        }
    }

    std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> outputSnapshot() const
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        return output;
    }

    bool openCapture()
    {
        if (!threadOptions.isValid()) {
            setError(
                ErrorCode::InvalidArgument,
                "V2::D3D12CameraCaptureThread::open",
                "Invalid thread options");
            return false;
        }

        if (capture && capture->isOpened()) {
            clearError();
            return true;
        }

        if (sourceMode == SourceMode::MovedCapture) {
            setError(
                ErrorCode::NotOpened,
                "V2::D3D12CameraCaptureThread::open",
                "Moved capture is not opened and cannot be reopened without selector/configuration");
            return false;
        }

        capture = std::make_unique<D3D12CameraCapture>();
        if (!capture->open(
                selector,
                captureConfig,
                backend,
                captureOptions)) {
            setError(capture->lastError());
            return false;
        }

        clearError();
        return true;
    }

    void workerLoop()
    {
        while (!stopRequested.load()) {
            auto result = capture->read(CameraReadOptions{
                ReadMode::NextFrame,
                threadOptions.readTimeoutMs});

            if (!result) {
                if (result.error.code == static_cast<int>(ErrorCode::Timeout)) {
                    incrementReadTimeout();
                    continue;
                }

                incrementReadError();
                setError(result.error);
                if (threadOptions.stopOnReadError) break;
                continue;
            }

            incrementReadFrame();

            auto queue = outputSnapshot();
            if (!queue) {
                incrementNoOutputDrop();
                continue;
            }

            auto pushResult = queue->push(D3D12IndexedReadOnlyCameraFrame{
                cameraId,
                std::move(result.frame)});
            recordPush(pushResult);
        }

        running.store(false);
    }
};

D3D12CameraCaptureThread::D3D12CameraCaptureThread(
    CameraId cameraId,
    IC4DeviceSelector selector,
    CameraCaptureConfig config,
    D3D12BackendContext backend,
    D3D12CameraCaptureOptions captureOptions,
    D3D12CameraCaptureThreadOptions threadOptions)
    : impl_(std::make_unique<Impl>())
{
    impl_->cameraId = cameraId;
    impl_->selector = std::move(selector);
    impl_->captureConfig = std::move(config);
    impl_->backend = std::move(backend);
    impl_->captureOptions = captureOptions;
    impl_->threadOptions = threadOptions;
    impl_->sourceMode = Impl::SourceMode::Internal;
}

D3D12CameraCaptureThread::D3D12CameraCaptureThread(
    CameraId cameraId,
    D3D12CameraCapture&& capture,
    D3D12CameraCaptureThreadOptions threadOptions)
    : impl_(std::make_unique<Impl>())
{
    impl_->cameraId = cameraId;
    impl_->capture = std::make_unique<D3D12CameraCapture>(std::move(capture));
    impl_->threadOptions = threadOptions;
    impl_->sourceMode = Impl::SourceMode::MovedCapture;
}

D3D12CameraCaptureThread::~D3D12CameraCaptureThread()
{
    stopAndJoin();
}

bool D3D12CameraCaptureThread::open()
{
    return impl_ && impl_->openCapture();
}

bool D3D12CameraCaptureThread::start()
{
    if (!impl_) return false;
    if (impl_->running.load()) return true;

    // A finished std::thread remains joinable. Join it before assigning a new
    // worker so start-stop-start is supported without std::terminate().
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    if (!impl_->openCapture()) return false;

    impl_->stopRequested.store(false);
    impl_->running.store(true);
    try {
        impl_->worker = std::thread(&Impl::workerLoop, impl_.get());
    } catch (const std::exception& exception) {
        impl_->running.store(false);
        impl_->setError(
            ErrorCode::ThreadError,
            "V2::D3D12CameraCaptureThread::start",
            exception.what());
        return false;
    }

    impl_->clearError();
    return true;
}

void D3D12CameraCaptureThread::requestStop()
{
    if (impl_) impl_->stopRequested.store(true);
}

void D3D12CameraCaptureThread::join()
{
    if (!impl_) return;
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->running.store(false);
}

void D3D12CameraCaptureThread::stopAndJoin()
{
    requestStop();
    join();
}

bool D3D12CameraCaptureThread::isRunning() const noexcept
{
    return impl_ && impl_->running.load();
}

bool D3D12CameraCaptureThread::isOpened() const noexcept
{
    return impl_ && impl_->capture && impl_->capture->isOpened();
}

CameraId D3D12CameraCaptureThread::cameraId() const noexcept
{
    return impl_ ? impl_->cameraId : CameraId{};
}

void D3D12CameraCaptureThread::setOutputQueue(
    std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> queue)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->outputMutex);
    impl_->output = std::move(queue);
}

std::shared_ptr<D3D12IndexedReadOnlyFrameQueue>
D3D12CameraCaptureThread::outputQueue() const
{
    return impl_ ? impl_->outputSnapshot() : nullptr;
}

bool D3D12CameraCaptureThread::startAcquisition()
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->startAcquisition();
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

bool D3D12CameraCaptureThread::stopAcquisition()
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->stopAcquisition();
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

bool D3D12CameraCaptureThread::softwareTrigger(const std::string& commandName)
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->softwareTrigger(commandName);
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

bool D3D12CameraCaptureThread::setIC4Property(
    const std::string& propertyName,
    bool value)
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->setIC4Property(propertyName, value);
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

bool D3D12CameraCaptureThread::setIC4Property(
    const std::string& propertyName,
    std::int64_t value)
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->setIC4Property(propertyName, value);
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

bool D3D12CameraCaptureThread::setIC4Property(
    const std::string& propertyName,
    double value)
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->setIC4Property(propertyName, value);
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

bool D3D12CameraCaptureThread::setIC4Property(
    const std::string& propertyName,
    const std::string& value)
{
    if (!impl_ || !impl_->capture) return false;
    const bool ok = impl_->capture->setIC4Property(propertyName, value);
    if (!ok) impl_->setError(impl_->capture->lastError());
    else impl_->clearError();
    return ok;
}

D3D12CameraCaptureThreadStats D3D12CameraCaptureThread::stats() const
{
    return impl_ ? impl_->getStats() : D3D12CameraCaptureThreadStats{};
}

CameraCaptureStats D3D12CameraCaptureThread::captureStats() const
{
    return impl_ && impl_->capture
        ? impl_->capture->stats()
        : CameraCaptureStats{};
}

D3D12FramePoolStats D3D12CameraCaptureThread::framePoolStats() const
{
    return impl_ && impl_->capture
        ? impl_->capture->framePoolStats()
        : D3D12FramePoolStats{};
}

ErrorInfo D3D12CameraCaptureThread::lastError() const
{
    if (!impl_) return NoError();
    const auto threadError = impl_->getError();
    if (threadError) return threadError;
    return impl_->capture ? impl_->capture->lastError() : NoError();
}

} // namespace IC4Ext::V2
