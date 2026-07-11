#include "IC4Ext/D3D12/D3D12CameraCaptureThread.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace IC4Ext {

namespace {

void AddOrReplaceOverride(CameraCaptureConfig& config, std::string name, IC4PropertyValue value)
{
    for (auto& ov : config.propertyOverrides) {
        if (ov.propertyName == name) {
            ov.value = std::move(value);
            return;
        }
    }
    config.propertyOverrides.push_back(IC4PropertyOverride{std::move(name), std::move(value)});
}

bool IsQueuePushSucceeded(ThreadKit::Queues::QueuePushResult result)
{
    return result == ThreadKit::Queues::QueuePushResult::Pushed ||
           result == ThreadKit::Queues::QueuePushResult::DroppedOldestAndPushed;
}

bool ShouldEmitCopiedOutputs(
    std::uint64_t sourceFrameSequence,
    const CameraThreadOptions& options,
    const CameraCaptureConfig& config)
{
    const std::uint32_t copyStride =
        std::max<std::uint32_t>(1u, options.copiedOutputFrameStride);
    const std::uint32_t copyBurst =
        std::min(copyStride,
                 std::max<std::uint32_t>(1u, options.copiedOutputFrameBurst));

    if (copyBurst >= copyStride) {
        return true;
    }

    const double fps = config.streamRequest.fps;
    if (std::isfinite(fps) && fps > 0.0) {
        // A shared process-wide epoch makes copied-output bursts line up across
        // independent camera worker threads. This is important for stereo
        // consumers because per-camera frame counters can have arbitrary phase.
        static const auto epoch = std::chrono::steady_clock::now();
        const double periodSeconds = static_cast<double>(copyStride) / fps;
        const double burstSeconds = static_cast<double>(copyBurst) / fps;
        if (std::isfinite(periodSeconds) && std::isfinite(burstSeconds) &&
            periodSeconds > 0.0 && burstSeconds > 0.0) {
            const double elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - epoch).count();
            const double position = std::fmod(elapsedSeconds, periodSeconds);
            return position >= 0.0 && position < burstSeconds;
        }
    }

    return (sourceFrameSequence % copyStride) < copyBurst;
}

} // namespace

D3D12CameraCaptureThread::D3D12CameraCaptureThread(IC4DeviceSelector selector,
                                                   CameraCaptureConfig config,
                                                   D3D12BackendContext backend,
                                                   CameraThreadOptions options)
    : selector_(std::move(selector)), config_(std::move(config)), backend_(backend), options_(options), sourceMode_(SourceMode::InternalCapture)
{
    ownedCapture_ = std::make_shared<D3D12CameraCapture>();
    source_ = ownedCapture_;
}

D3D12CameraCaptureThread::D3D12CameraCaptureThread(D3D12CameraCapture&& capture,
                                                   D3D12BackendContext backend,
                                                   CameraThreadOptions options)
    : backend_(backend), options_(options), sourceMode_(SourceMode::MovedCapture)
{
    ownedCapture_ = std::make_shared<D3D12CameraCapture>(std::move(capture));
    source_ = ownedCapture_;
}

D3D12CameraCaptureThread::D3D12CameraCaptureThread(std::shared_ptr<ID3D12Camera> source,
                                                   D3D12BackendContext backend,
                                                   CameraThreadOptions options)
    : backend_(backend), options_(options), sourceMode_(SourceMode::ExternalSource), source_(std::move(source))
{
}

D3D12CameraCaptureThread::~D3D12CameraCaptureThread()
{
    stopAndJoin();
}

void D3D12CameraCaptureThread::setError(ErrorCode code, const char* where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12CameraCaptureThread::open()
{
    lastError_ = NoError();
    if (!source_) {
        setError(ErrorCode::InvalidArgument, "D3D12CameraCaptureThread::open", "camera source is null");
        return false;
    }

    if (sourceMode_ == SourceMode::InternalCapture) {
        if (source_->isOpened()) {
            return true;
        }
        if (!backend_.resolve() || !backend_.corePtr || !backend_.queue) {
            setError(ErrorCode::InvalidArgument,
                     "D3D12CameraCaptureThread::open",
                     "D3D12 backend must be created from D3D12Helper D3D12Core. Use D3D12BackendContext::FromCore(...).");
            return false;
        }

        CameraCaptureConfig threadConfig = config_;
        threadConfig.queuePolicy = FrameQueuePolicy::PreserveFrames;
        if (threadConfig.maxPendingBuffers == 1) {
            threadConfig.maxPendingBuffers = 0;
        }

        {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            if (!ownedCapture_->open(selector_, threadConfig, backend_)) {
                lastError_ = ownedCapture_->lastError();
                return false;
            }
        }

        copyFenceManager_ = std::make_unique<D3D12FenceManager>();
        if (!copyFenceManager_->initialize(backend_)) {
            lastError_ = copyFenceManager_->lastError();
            return false;
        }
        if (!copier_.initialize(backend_, copyFenceManager_.get())) {
            lastError_ = copier_.lastError();
            return false;
        }
    } else if (sourceMode_ == SourceMode::MovedCapture) {
        if (!source_->isOpened()) {
            setError(ErrorCode::InvalidArgument, "D3D12CameraCaptureThread::open", "Moved capture is not opened");
            return false;
        }
    } else {
        if (!source_->isOpened()) {
            setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::open", "External camera source is not opened");
            return false;
        }
    }

    if (!copyFenceManager_ && options_.copyPerOutputQueue) {
        auto resolved = backend_;
        if (resolved.resolve() && resolved.corePtr && resolved.queue) {
            backend_ = resolved;
            copyFenceManager_ = std::make_unique<D3D12FenceManager>();
            if (!copyFenceManager_->initialize(backend_)) {
                lastError_ = copyFenceManager_->lastError();
                return false;
            }
            if (!copier_.initialize(backend_, copyFenceManager_.get())) {
                lastError_ = copier_.lastError();
                return false;
            }
        }
    }
    return true;
}

bool D3D12CameraCaptureThread::start()
{
    lastError_ = NoError();
    if (running_.load()) return true;
    if (!open()) return false;
    dispatchedFrameCount_ = 0;
    stopRequested_.store(false);
    worker_ = std::thread(&D3D12CameraCaptureThread::workerLoop, this);
    running_.store(true);
    return true;
}

void D3D12CameraCaptureThread::requestStop()
{
    stopRequested_.store(true);
}

void D3D12CameraCaptureThread::join()
{
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

void D3D12CameraCaptureThread::stopAndJoin()
{
    requestStop();
    join();
}

void D3D12CameraCaptureThread::addOutputQueue(std::uint32_t cameraIndex,
                                              std::shared_ptr<D3D12IndexedFrameQueue> queue)
{
    if (!queue) {
        setError(ErrorCode::InvalidArgument, "D3D12CameraCaptureThread::addOutputQueue", "queue is null");
        return;
    }
    std::lock_guard<std::mutex> lock(outputMutex_);
    outputs_.push_back(OutputBinding{cameraIndex, std::move(queue)});
}

bool D3D12CameraCaptureThread::applyIC4StateJson(const std::filesystem::path& jsonPath,
                                                 std::size_t deviceIndex,
                                                 bool strict,
                                                 bool applyNestedSelectorStates)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->applyIC4StateJson(jsonPath, deviceIndex, strict, applyNestedSelectorStates);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }

    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::applyIC4StateJson", "Source is not opened");
        return false;
    }
    config_.ic4StateJson.path = jsonPath;
    config_.ic4StateJson.deviceIndex = deviceIndex;
    config_.ic4StateJson.strict = strict;
    config_.ic4StateJson.applyNestedSelectorStates = applyNestedSelectorStates;
    return true;
}

bool D3D12CameraCaptureThread::setIC4Property(const std::string& propertyName, bool value)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setIC4Property(propertyName, value);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setIC4Property", "Source is not opened");
        return false;
    }
    AddOrReplaceOverride(config_, propertyName, value);
    return true;
}

bool D3D12CameraCaptureThread::setIC4Property(const std::string& propertyName, int value)
{
    return setIC4Property(propertyName, static_cast<std::int64_t>(value));
}

bool D3D12CameraCaptureThread::setIC4Property(const std::string& propertyName, std::int64_t value)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setIC4Property(propertyName, value);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setIC4Property", "Source is not opened");
        return false;
    }
    AddOrReplaceOverride(config_, propertyName, value);
    return true;
}

bool D3D12CameraCaptureThread::setIC4Property(const std::string& propertyName, double value)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setIC4Property(propertyName, value);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setIC4Property", "Source is not opened");
        return false;
    }
    AddOrReplaceOverride(config_, propertyName, value);
    return true;
}

bool D3D12CameraCaptureThread::setIC4Property(const std::string& propertyName, const char* value)
{
    return setIC4Property(propertyName, std::string(value ? value : ""));
}

bool D3D12CameraCaptureThread::setIC4Property(const std::string& propertyName, const std::string& value)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setIC4Property(propertyName, value);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setIC4Property", "Source is not opened");
        return false;
    }
    AddOrReplaceOverride(config_, propertyName, value);
    return true;
}

bool D3D12CameraCaptureThread::setFrameRate(double fps)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setFrameRate(fps);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setFrameRate", "Source is not opened");
        return false;
    }
    config_.streamRequest.fps = fps;
    return true;
}

bool D3D12CameraCaptureThread::setExposureAuto(const std::string& mode)
{
    return setIC4Property("ExposureAuto", mode);
}

bool D3D12CameraCaptureThread::setExposureTime(double exposureTimeUs)
{
    return setIC4Property("ExposureTime", exposureTimeUs);
}

bool D3D12CameraCaptureThread::setGainAuto(const std::string& mode)
{
    return setIC4Property("GainAuto", mode);
}

bool D3D12CameraCaptureThread::setGain(double gain)
{
    return setIC4Property("Gain", gain);
}

bool D3D12CameraCaptureThread::setGamma(double gamma)
{
    return setIC4Property("Gamma", gamma);
}

bool D3D12CameraCaptureThread::setOffset(int offsetX, int offsetY)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setOffset(offsetX, offsetY);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setOffset", "Source is not opened");
        return false;
    }
    config_.streamRequest.offsetX = offsetX;
    config_.streamRequest.offsetY = offsetY;
    return true;
}

bool D3D12CameraCaptureThread::setRoi(int width, int height, int offsetX, int offsetY)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setRoi(width, height, offsetX, offsetY);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setRoi", "Source is not opened");
        return false;
    }
    config_.streamRequest.width = width;
    config_.streamRequest.height = height;
    config_.streamRequest.offsetX = offsetX;
    config_.streamRequest.offsetY = offsetY;
    return true;
}

bool D3D12CameraCaptureThread::setPixelFormat(CameraPixelFormat fmt)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (source_ && source_->isOpened()) {
        const bool ok = source_->setPixelFormat(fmt);
        lastError_ = ok ? NoError() : source_->lastError();
        return ok;
    }
    if (sourceMode_ != SourceMode::InternalCapture) {
        setError(ErrorCode::NotOpened, "D3D12CameraCaptureThread::setPixelFormat", "Source is not opened");
        return false;
    }
    config_.streamRequest.requestedFormat = fmt;
    config_.streamRequest.forceRequestedFormat = true;
    return true;
}

CameraThreadStats D3D12CameraCaptureThread::stats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void D3D12CameraCaptureThread::workerLoop()
{
    while (!stopRequested_.load()) {
        D3D12ReadResult result;
        {
            std::lock_guard<std::mutex> sourceLock(sourceMutex_);
            result = source_->read(CameraReadOptions{ReadMode::NextFrame, options_.readTimeoutMs});
        }
        if (!result) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            if (result.error.code == static_cast<int>(ErrorCode::Timeout)) {
                ++stats_.readTimeouts;
            } else {
                ++stats_.readErrors;
                lastError_ = result.error;
                if (options_.stopOnReadError) {
                    break;
                }
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.readFrames;
        }
        dispatchFrame(std::move(result.frame));
    }
    running_.store(false);
}

void D3D12CameraCaptureThread::dispatchFrame(D3D12CameraFrame&& frame)
{
    std::vector<OutputBinding> outputs;
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        outputs = outputs_;
    }

    if (outputs.empty()) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.noOutputDrops;
        return;
    }

    const std::uint64_t sourceFrameSequence = dispatchedFrameCount_++;

    if (outputs.size() == 1 || !options_.copyPerOutputQueue) {
        auto res = outputs.front().queue->push(
            D3D12IndexedCameraFrame{outputs.front().cameraIndex, std::move(frame)});
        std::lock_guard<std::mutex> lock(statsMutex_);
        if (IsQueuePushSucceeded(res)) ++stats_.pushedFrames;
        else ++stats_.pushFailures;
        return;
    }

    const bool emitCopiedOutputs =
        ShouldEmitCopiedOutputs(sourceFrameSequence, options_, config_);

    if (emitCopiedOutputs) {
        if (!copyFenceManager_) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.copyFailures;
            lastError_ = MakeError(
                ErrorCode::ThreadError,
                "D3D12CameraCaptureThread::dispatchFrame",
                "Multiple output queues require an internally opened real capture thread so frame copies can be fenced");
            return;
        }

        for (std::size_t i = 0; i + 1 < outputs.size(); ++i) {
            D3D12CameraFrame copied;
            if (!copier_.copyFrame(frame, copied)) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                ++stats_.copyFailures;
                lastError_ = copier_.lastError();
                continue;
            }
            auto res = outputs[i].queue->push(
                D3D12IndexedCameraFrame{outputs[i].cameraIndex, std::move(copied)});
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.copiedFrames;
            if (IsQueuePushSucceeded(res)) ++stats_.pushedFrames;
            else ++stats_.pushFailures;
        }
    }

    auto& last = outputs.back();
    auto res = last.queue->push(
        D3D12IndexedCameraFrame{last.cameraIndex, std::move(frame)});
    std::lock_guard<std::mutex> lock(statsMutex_);
    if (IsQueuePushSucceeded(res)) ++stats_.pushedFrames;
    else ++stats_.pushFailures;
}

} // namespace IC4Ext
