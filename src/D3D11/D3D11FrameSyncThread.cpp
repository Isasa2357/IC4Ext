#include "IC4Ext/D3D11/D3D11FrameSyncThread.hpp"

#include <chrono>
#include <utility>

namespace IC4Ext {

D3D11FrameSyncThread::D3D11FrameSyncThread(std::shared_ptr<D3D11IndexedFrameQueue> inputQueue,
                                           std::shared_ptr<D3D11SyncedFrameQueue> outputQueue,
                                           FrameSyncOptions options)
    : inputQueue_(std::move(inputQueue)), outputQueue_(std::move(outputQueue)), options_(std::move(options))
{
}

D3D11FrameSyncThread::~D3D11FrameSyncThread()
{
    stopAndJoin();
}

void D3D11FrameSyncThread::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameSyncThread::start()
{
    lastError_ = NoError();
    if (running_.load()) return true;
    if (!inputQueue_ || !outputQueue_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameSyncThread::start", "input/output queue is null");
        return false;
    }
    if (options_.policy != FrameSyncPolicy::PassThroughSingleCamera) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameSyncThread::start", "Initial implementation supports only PassThroughSingleCamera");
        return false;
    }
    if (options_.cameraIndices.empty()) {
        options_.cameraIndices.push_back(0);
    }
    stopRequested_.store(false);
    worker_ = std::thread(&D3D11FrameSyncThread::workerLoop, this);
    running_.store(true);
    return true;
}

void D3D11FrameSyncThread::requestStop()
{
    stopRequested_.store(true);
}

void D3D11FrameSyncThread::join()
{
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

void D3D11FrameSyncThread::stopAndJoin()
{
    requestStop();
    join();
}

FrameSyncStats D3D11FrameSyncThread::stats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void D3D11FrameSyncThread::workerLoop()
{
    const std::uint32_t targetCamera = options_.cameraIndices.empty() ? 0u : options_.cameraIndices.front();
    while (!stopRequested_.load()) {
        auto item = inputQueue_->waitPopFor(std::chrono::milliseconds(100));
        if (!item) continue;
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.inputFrames;
        }
        if (item->cameraIndex != targetCamera) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.ignoredFrames;
            continue;
        }

        D3D11SyncedFrameSet set;
        set.syncGroupId = nextSyncGroupId_++;
        set.frames.push_back(std::move(*item));
        // The timestamp represents the submission point: the synchronized set is
        // complete and is about to be pushed to the output queue.
        set.emittedTime = std::chrono::steady_clock::now();

        auto res = outputQueue_->push(std::move(set));
        std::lock_guard<std::mutex> lock(statsMutex_);
        if (res == ThreadKit::Queues::QueuePushResult::Pushed || res == ThreadKit::Queues::QueuePushResult::DroppedOldestAndPushed) ++stats_.emittedSets;
        else ++stats_.pushFailures;
    }
    running_.store(false);
}

} // namespace IC4Ext
