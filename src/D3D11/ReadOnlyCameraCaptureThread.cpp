#include "IC4Ext/D3D11/CameraCaptureThread.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace IC4Ext::D3D11 {

class D3D11ReadOnlyCameraCaptureThread::Impl
{
public:
    enum class SourceMode
    {
        Internal,
        MovedCapture,
        ExternalSource,
    };

    CameraId cameraId = 0;
    IC4DeviceSelector selector;
    CameraCaptureConfig captureConfig;
    D3D11BackendContext backend;
    D3D11CameraCaptureOptions captureOptions;
    D3D11CameraCaptureThreadOptions threadOptions;
    SourceMode sourceMode = SourceMode::Internal;

    std::unique_ptr<D3D11ReadOnlyCameraCapture> capture;
    std::shared_ptr<ReadOnlyFrameSource> externalSource;

    mutable std::mutex outputMutex;
    std::shared_ptr<D3D11IndexedReadOnlyFrameQueue> output;

    mutable std::mutex statsMutex;
    D3D11CameraCaptureThreadStats threadStats;
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
    void setError(ErrorCode code, const char* where, std::string message)
    {
        setError(MakeError(code, where, std::move(message)));
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

    D3D11CameraCaptureThreadStats getStats() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        return threadStats;
    }

    void recordReadFrame()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.readFrames;
    }
    void recordTimeout()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.readTimeouts;
    }
    void recordError()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++threadStats.readErrors;
    }
    void recordNoOutput()
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
        default:
            ++threadStats.pushFailures;
            break;
        }
    }

    std::shared_ptr<D3D11IndexedReadOnlyFrameQueue> outputSnapshot() const
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        return output;
    }

    bool sourceIsOpened() const noexcept
    {
        if (sourceMode == SourceMode::ExternalSource) {
            return externalSource && externalSource->isOpened();
        }
        return capture && capture->isOpened();
    }

    bool openCapture()
    {
        if (!threadOptions.isValid()) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D11ReadOnlyCameraCaptureThread::open",
                "Invalid thread options");
            return false;
        }

        if (sourceMode == SourceMode::ExternalSource) {
            if (!externalSource || !externalSource->isOpened()) {
                auto sourceError = externalSource
                    ? externalSource->lastError()
                    : NoError();
                if (!sourceError) {
                    sourceError = MakeError(
                        ErrorCode::NotOpened,
                        "D3D11ReadOnlyCameraCaptureThread::open",
                        "External ReadOnlyFrameSource is not opened");
                }
                setError(std::move(sourceError));
                return false;
            }
            clearError();
            return true;
        }

        if (capture && capture->isOpened()) {
            clearError();
            return true;
        }
        if (sourceMode == SourceMode::MovedCapture) {
            setError(
                ErrorCode::NotOpened,
                "D3D11ReadOnlyCameraCaptureThread::open",
                "Moved capture is not opened and cannot be reopened");
            return false;
        }

        capture = std::make_unique<D3D11ReadOnlyCameraCapture>();
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

    D3D11ReadOnlyReadResult readNext()
    {
        const CameraReadOptions options{
            ReadMode::NextFrame,
            threadOptions.readTimeoutMs};
        if (sourceMode != SourceMode::ExternalSource) {
            return capture->read(options);
        }

        D3D11ReadOnlyReadResult result;
        result.ok = externalSource->read(options, result.frame, result.error);
        if (!result.ok && !result.error) result.error = externalSource->lastError();
        if (result.ok && !result.frame) {
            result.ok = false;
            result.error = MakeError(
                ErrorCode::InternalError,
                "D3D11ReadOnlyCameraCaptureThread::readNext",
                "External source returned success with an invalid frame");
        }
        return result;
    }

    void workerLoop()
    {
        while (!stopRequested.load(std::memory_order_acquire)) {
            auto result = readNext();
            if (!result) {
                if (result.error.code == static_cast<int>(ErrorCode::Timeout)) {
                    recordTimeout();
                    continue;
                }
                recordError();
                setError(result.error);
                if (threadOptions.stopOnReadError) break;
                continue;
            }

            recordReadFrame();
            auto queue = outputSnapshot();
            if (!queue) {
                recordNoOutput();
                continue;
            }
            recordPush(queue->push(D3D11IndexedReadOnlyCameraFrame{
                cameraId,
                std::move(result.frame)}));
        }
        running.store(false, std::memory_order_release);
    }

    D3D11ReadOnlyCameraCapture* requireCapture(const char* where)
    {
        if (capture) return capture.get();
        setError(
            ErrorCode::InvalidArgument,
            where,
            "Operation is unavailable for an injected ReadOnlyFrameSource");
        return nullptr;
    }
};

D3D11ReadOnlyCameraCaptureThread::D3D11ReadOnlyCameraCaptureThread(
    CameraId cameraId,
    IC4DeviceSelector selector,
    CameraCaptureConfig config,
    D3D11BackendContext backend,
    D3D11CameraCaptureOptions captureOptions,
    D3D11CameraCaptureThreadOptions threadOptions)
    : impl_(std::make_unique<Impl>())
{
    impl_->cameraId = cameraId;
    impl_->selector = std::move(selector);
    impl_->captureConfig = std::move(config);
    impl_->backend = std::move(backend);
    impl_->captureOptions = captureOptions;
    impl_->threadOptions = threadOptions;
}

D3D11ReadOnlyCameraCaptureThread::D3D11ReadOnlyCameraCaptureThread(
    CameraId cameraId,
    D3D11ReadOnlyCameraCapture&& capture,
    D3D11CameraCaptureThreadOptions threadOptions)
    : impl_(std::make_unique<Impl>())
{
    impl_->cameraId = cameraId;
    impl_->capture = std::make_unique<D3D11ReadOnlyCameraCapture>(
        std::move(capture));
    impl_->threadOptions = threadOptions;
    impl_->sourceMode = Impl::SourceMode::MovedCapture;
}

D3D11ReadOnlyCameraCaptureThread::D3D11ReadOnlyCameraCaptureThread(
    CameraId cameraId,
    std::shared_ptr<ReadOnlyFrameSource> source,
    D3D11CameraCaptureThreadOptions threadOptions)
    : impl_(std::make_unique<Impl>())
{
    impl_->cameraId = cameraId;
    impl_->externalSource = std::move(source);
    impl_->threadOptions = threadOptions;
    impl_->sourceMode = Impl::SourceMode::ExternalSource;
}

D3D11ReadOnlyCameraCaptureThread::~D3D11ReadOnlyCameraCaptureThread()
{
    stopAndJoin();
}

bool D3D11ReadOnlyCameraCaptureThread::open()
{
    return impl_ && impl_->openCapture();
}

bool D3D11ReadOnlyCameraCaptureThread::start()
{
    if (!impl_) return false;
    if (impl_->running.load(std::memory_order_acquire)) return true;
    if (impl_->worker.joinable()) impl_->worker.join();
    if (!impl_->openCapture()) return false;

    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread(&Impl::workerLoop, impl_.get());
    } catch (const std::exception& exception) {
        impl_->running.store(false, std::memory_order_release);
        impl_->setError(
            ErrorCode::ThreadError,
            "D3D11ReadOnlyCameraCaptureThread::start",
            exception.what());
        return false;
    }
    impl_->clearError();
    return true;
}

void D3D11ReadOnlyCameraCaptureThread::requestStop()
{
    if (impl_) impl_->stopRequested.store(true, std::memory_order_release);
}

void D3D11ReadOnlyCameraCaptureThread::join()
{
    if (!impl_) return;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->running.store(false, std::memory_order_release);
}

void D3D11ReadOnlyCameraCaptureThread::stopAndJoin()
{
    requestStop();
    join();
}

bool D3D11ReadOnlyCameraCaptureThread::isRunning() const noexcept
{
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

bool D3D11ReadOnlyCameraCaptureThread::isOpened() const noexcept
{
    return impl_ && impl_->sourceIsOpened();
}

CameraId D3D11ReadOnlyCameraCaptureThread::cameraId() const noexcept
{
    return impl_ ? impl_->cameraId : CameraId{};
}

void D3D11ReadOnlyCameraCaptureThread::setOutputQueue(
    std::shared_ptr<D3D11IndexedReadOnlyFrameQueue> queue)
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->outputMutex);
    impl_->output = std::move(queue);
}

std::shared_ptr<D3D11IndexedReadOnlyFrameQueue>
D3D11ReadOnlyCameraCaptureThread::outputQueue() const
{
    return impl_ ? impl_->outputSnapshot() : nullptr;
}

bool D3D11ReadOnlyCameraCaptureThread::startAcquisition()
{
    auto* capture = impl_->requireCapture(
        "D3D11ReadOnlyCameraCaptureThread::startAcquisition");
    if (!capture) return false;
    const bool ok = capture->startAcquisition();
    impl_->setError(ok ? NoError() : capture->lastError());
    return ok;
}

bool D3D11ReadOnlyCameraCaptureThread::stopAcquisition()
{
    auto* capture = impl_->requireCapture(
        "D3D11ReadOnlyCameraCaptureThread::stopAcquisition");
    if (!capture) return false;
    const bool ok = capture->stopAcquisition();
    impl_->setError(ok ? NoError() : capture->lastError());
    return ok;
}

bool D3D11ReadOnlyCameraCaptureThread::softwareTrigger(
    const std::string& commandName)
{
    auto* capture = impl_->requireCapture(
        "D3D11ReadOnlyCameraCaptureThread::softwareTrigger");
    if (!capture) return false;
    const bool ok = capture->softwareTrigger(commandName);
    impl_->setError(ok ? NoError() : capture->lastError());
    return ok;
}

#define IC4EXT_D3D11_THREAD_PROPERTY(TYPE) \
bool D3D11ReadOnlyCameraCaptureThread::setIC4Property( \
    const std::string& name, TYPE value) \
{ \
    auto* capture = impl_->requireCapture( \
        "D3D11ReadOnlyCameraCaptureThread::setIC4Property"); \
    if (!capture) return false; \
    const bool ok = capture->setIC4Property(name, value); \
    impl_->setError(ok ? NoError() : capture->lastError()); \
    return ok; \
}

IC4EXT_D3D11_THREAD_PROPERTY(bool)
IC4EXT_D3D11_THREAD_PROPERTY(std::int64_t)
IC4EXT_D3D11_THREAD_PROPERTY(double)
IC4EXT_D3D11_THREAD_PROPERTY(const std::string&)
#undef IC4EXT_D3D11_THREAD_PROPERTY

D3D11CameraCaptureThreadStats D3D11ReadOnlyCameraCaptureThread::stats() const
{
    return impl_ ? impl_->getStats() : D3D11CameraCaptureThreadStats{};
}

CameraCaptureStats D3D11ReadOnlyCameraCaptureThread::captureStats() const
{
    return impl_ && impl_->capture
        ? impl_->capture->stats()
        : CameraCaptureStats{};
}

D3D11FramePoolStats D3D11ReadOnlyCameraCaptureThread::framePoolStats() const
{
    return impl_ && impl_->capture
        ? impl_->capture->framePoolStats()
        : D3D11FramePoolStats{};
}

ErrorInfo D3D11ReadOnlyCameraCaptureThread::lastError() const
{
    if (!impl_) return NoError();
    const auto threadError = impl_->getError();
    if (threadError) return threadError;
    if (impl_->capture) return impl_->capture->lastError();
    if (impl_->externalSource) return impl_->externalSource->lastError();
    return NoError();
}

} // namespace IC4Ext::D3D11
