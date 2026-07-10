#include "IC4Ext/D3D12/D3D12FrameSyncThread.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace IC4Ext {

namespace {

bool IsQueuePushSucceeded(ThreadKit::Queues::QueuePushResult result)
{
    return result == ThreadKit::Queues::QueuePushResult::Pushed ||
           result == ThreadKit::Queues::QueuePushResult::DroppedOldestAndPushed;
}

} // namespace

D3D12FrameSyncThread::D3D12FrameSyncThread(std::shared_ptr<D3D12IndexedFrameQueue> inputQueue,
                                           std::shared_ptr<D3D12SyncedFrameQueue> outputQueue,
                                           FrameSyncOptions options)
    : inputQueue_(std::move(inputQueue)), outputQueue_(std::move(outputQueue)), options_(std::move(options))
{
}

D3D12FrameSyncThread::~D3D12FrameSyncThread()
{
    stopAndJoin();
}

void D3D12FrameSyncThread::setError(ErrorCode code, const char* where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12FrameSyncThread::start()
{
    lastError_ = NoError();
    if (running_.load()) return true;
    if (!inputQueue_ || !outputQueue_) {
        setError(ErrorCode::InvalidArgument, "D3D12FrameSyncThread::start", "input/output queue is null");
        return false;
    }

    if (options_.cameraIndices.empty()) {
        options_.cameraIndices.push_back(0);
    }

    std::vector<std::uint32_t> unique;
    unique.reserve(options_.cameraIndices.size());
    for (std::uint32_t cameraIndex : options_.cameraIndices) {
        if (std::find(unique.begin(), unique.end(), cameraIndex) != unique.end()) {
            setError(ErrorCode::InvalidArgument, "D3D12FrameSyncThread::start", "cameraIndices contains duplicate camera index");
            return false;
        }
        unique.push_back(cameraIndex);
    }

    if (options_.policy != FrameSyncPolicy::PassThroughSingleCamera && options_.cameraIndices.size() < 2) {
        setError(ErrorCode::InvalidArgument,
                 "D3D12FrameSyncThread::start",
                 "TimestampNearest and FrameNumberExact require at least two camera indices");
        return false;
    }

    buffers_.clear();
    buffers_.reserve(options_.cameraIndices.size());
    for (std::uint32_t cameraIndex : options_.cameraIndices) {
        buffers_.push_back(CameraBuffer{cameraIndex, {}});
    }

    stopRequested_.store(false);
    worker_ = std::thread(&D3D12FrameSyncThread::workerLoop, this);
    running_.store(true);
    return true;
}

void D3D12FrameSyncThread::requestStop()
{
    stopRequested_.store(true);
}

void D3D12FrameSyncThread::join()
{
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

void D3D12FrameSyncThread::stopAndJoin()
{
    requestStop();
    join();
}

FrameSyncStats D3D12FrameSyncThread::stats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void D3D12FrameSyncThread::incrementInputFrames()
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.inputFrames;
}

void D3D12FrameSyncThread::incrementIgnoredFrames()
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.ignoredFrames;
}

void D3D12FrameSyncThread::incrementDroppedFrames()
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.droppedFrames;
}

void D3D12FrameSyncThread::incrementEmittedOrPushFailure(ThreadKit::Queues::QueuePushResult result)
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    if (IsQueuePushSucceeded(result)) {
        ++stats_.emittedSets;
    } else {
        ++stats_.pushFailures;
    }
}

void D3D12FrameSyncThread::workerLoop()
{
    while (!stopRequested_.load()) {
        auto item = inputQueue_->waitPopFor(std::chrono::milliseconds(100));
        if (!item) {
            continue;
        }
        incrementInputFrames();
        handleInputFrame(std::move(*item));
    }
    running_.store(false);
}

void D3D12FrameSyncThread::handleInputFrame(D3D12IndexedCameraFrame&& frame)
{
    if (!isRequiredCamera(frame.cameraIndex)) {
        incrementIgnoredFrames();
        return;
    }

    if (options_.policy == FrameSyncPolicy::PassThroughSingleCamera) {
        const std::uint32_t targetCamera = options_.cameraIndices.empty() ? 0u : options_.cameraIndices.front();
        if (frame.cameraIndex != targetCamera) {
            incrementIgnoredFrames();
            return;
        }
        emitPassThrough(std::move(frame));
        return;
    }

    addBufferedFrame(std::move(frame));
    tryEmitBufferedSets();
}

bool D3D12FrameSyncThread::emitPassThrough(D3D12IndexedCameraFrame&& frame)
{
    D3D12SyncedFrameSet set;
    set.syncGroupId = nextSyncGroupId_++;
    set.frames.push_back(std::move(frame));
    // The timestamp represents the submission point: the synchronized set is
    // complete and is about to be pushed to the output queue.
    set.emittedTime = std::chrono::steady_clock::now();

    auto result = outputQueue_->push(std::move(set));
    incrementEmittedOrPushFailure(result);
    return IsQueuePushSucceeded(result);
}

D3D12FrameSyncThread::CameraBuffer* D3D12FrameSyncThread::findBuffer(std::uint32_t cameraIndex)
{
    for (auto& buffer : buffers_) {
        if (buffer.cameraIndex == cameraIndex) return &buffer;
    }
    return nullptr;
}

const D3D12FrameSyncThread::CameraBuffer* D3D12FrameSyncThread::findBuffer(std::uint32_t cameraIndex) const
{
    for (const auto& buffer : buffers_) {
        if (buffer.cameraIndex == cameraIndex) return &buffer;
    }
    return nullptr;
}

bool D3D12FrameSyncThread::isRequiredCamera(std::uint32_t cameraIndex) const
{
    return std::find(options_.cameraIndices.begin(), options_.cameraIndices.end(), cameraIndex) != options_.cameraIndices.end();
}

void D3D12FrameSyncThread::addBufferedFrame(D3D12IndexedCameraFrame&& frame)
{
    CameraBuffer* buffer = findBuffer(frame.cameraIndex);
    if (!buffer) {
        incrementIgnoredFrames();
        return;
    }
    buffer->frames.push_back(std::move(frame));
    trimBuffer(*buffer);
}

void D3D12FrameSyncThread::trimBuffer(CameraBuffer& buffer)
{
    if (options_.maxBufferedFramesPerCamera == 0) {
        return;
    }
    while (buffer.frames.size() > options_.maxBufferedFramesPerCamera) {
        buffer.frames.pop_front();
        incrementDroppedFrames();
    }
}

void D3D12FrameSyncThread::dropFront(CameraBuffer& buffer)
{
    if (!buffer.frames.empty()) {
        buffer.frames.pop_front();
        incrementDroppedFrames();
    }
}

bool D3D12FrameSyncThread::allRequiredBuffersHaveFrames() const
{
    for (const auto& buffer : buffers_) {
        if (buffer.frames.empty()) return false;
    }
    return !buffers_.empty();
}

void D3D12FrameSyncThread::tryEmitBufferedSets()
{
    switch (options_.policy) {
    case FrameSyncPolicy::FrameNumberExact:
        tryEmitFrameNumberExactSets();
        break;
    case FrameSyncPolicy::TimestampNearest:
        tryEmitTimestampNearestSets();
        break;
    case FrameSyncPolicy::PassThroughSingleCamera:
    default:
        break;
    }
}

void D3D12FrameSyncThread::tryEmitFrameNumberExactSets()
{
    while (allRequiredBuffersHaveFrames()) {
        std::uint64_t targetFrameNumber = 0;
        for (const auto& buffer : buffers_) {
            targetFrameNumber = std::max(targetFrameNumber, buffer.frames.front().frame.timing.frameNumber);
        }

        bool droppedAny = false;
        for (auto& buffer : buffers_) {
            while (!buffer.frames.empty() && buffer.frames.front().frame.timing.frameNumber < targetFrameNumber) {
                dropFront(buffer);
                droppedAny = true;
            }
        }
        if (!allRequiredBuffersHaveFrames()) {
            return;
        }
        if (droppedAny) {
            continue;
        }

        bool allMatch = true;
        for (const auto& buffer : buffers_) {
            if (buffer.frames.front().frame.timing.frameNumber != targetFrameNumber) {
                allMatch = false;
                break;
            }
        }
        if (!allMatch) {
            continue;
        }
        emitFrontSet();
    }
}

void D3D12FrameSyncThread::tryEmitTimestampNearestSets()
{
    const std::uint64_t maxDiffNs = options_.maxTimestampDiffNs;

    while (allRequiredBuffersHaveFrames()) {
        std::uint64_t minTs = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t maxTs = 0;
        CameraBuffer* minBuffer = nullptr;

        for (auto& buffer : buffers_) {
            const std::uint64_t ts = syncTimestampNs(buffer.frames.front());
            if (ts < minTs) {
                minTs = ts;
                minBuffer = &buffer;
            }
            maxTs = std::max(maxTs, ts);
        }

        if (maxTs >= minTs && maxTs - minTs <= maxDiffNs) {
            emitFrontSet();
            continue;
        }

        if (!minBuffer) {
            return;
        }
        dropFront(*minBuffer);
    }
}

bool D3D12FrameSyncThread::emitFrontSet()
{
    if (!allRequiredBuffersHaveFrames()) return false;

    D3D12SyncedFrameSet set;
    set.syncGroupId = nextSyncGroupId_++;
    set.frames.reserve(buffers_.size());

    for (auto& buffer : buffers_) {
        set.frames.push_back(std::move(buffer.frames.front()));
        buffer.frames.pop_front();
    }

    // Timestamp immediately before the completed set is submitted to the output queue.
    set.emittedTime = std::chrono::steady_clock::now();
    auto result = outputQueue_->push(std::move(set));
    incrementEmittedOrPushFailure(result);
    return IsQueuePushSucceeded(result);
}

std::uint64_t D3D12FrameSyncThread::hostTimestampNs(const D3D12IndexedCameraFrame& frame) noexcept
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        frame.frame.timing.hostReceivedTime.time_since_epoch()).count();
    if (ns <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ns);
}

std::uint64_t D3D12FrameSyncThread::syncTimestampNs(const D3D12IndexedCameraFrame& frame) const noexcept
{
    const std::uint64_t hostNs = hostTimestampNs(frame);
    const std::uint64_t deviceNs = frame.frame.timing.deviceTimestampNs;

    switch (options_.timestampSource) {
    case FrameSyncTimestampSource::HostReceived:
        return hostNs;
    case FrameSyncTimestampSource::Device:
        return deviceNs;
    case FrameSyncTimestampSource::Auto:
    default:
        // Independent cameras often expose free-running timestamp counters with
        // unrelated epochs. hostReceivedTime is recorded in one process-wide
        // steady-clock domain and is therefore the safe automatic comparison basis.
        return hostNs != 0 ? hostNs : deviceNs;
    }
}

} // namespace IC4Ext
