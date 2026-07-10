#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace IC4Ext {

class D3D12FrameSyncThread
{
public:
    D3D12FrameSyncThread(std::shared_ptr<D3D12IndexedFrameQueue> inputQueue,
                         std::shared_ptr<D3D12SyncedFrameQueue> outputQueue,
                         FrameSyncOptions options = {});
    ~D3D12FrameSyncThread();

    D3D12FrameSyncThread(const D3D12FrameSyncThread&) = delete;
    D3D12FrameSyncThread& operator=(const D3D12FrameSyncThread&) = delete;

    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    FrameSyncStats stats() const;
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    struct CameraBuffer
    {
        std::uint32_t cameraIndex = 0;
        std::deque<D3D12IndexedCameraFrame> frames;
    };

    void workerLoop();
    void handleInputFrame(D3D12IndexedCameraFrame&& frame);
    bool emitPassThrough(D3D12IndexedCameraFrame&& frame);
    void addBufferedFrame(D3D12IndexedCameraFrame&& frame);
    void tryEmitBufferedSets();
    void tryEmitFrameNumberExactSets();
    void tryEmitTimestampNearestSets();
    bool allRequiredBuffersHaveFrames() const;
    bool emitFrontSet();
    CameraBuffer* findBuffer(std::uint32_t cameraIndex);
    const CameraBuffer* findBuffer(std::uint32_t cameraIndex) const;
    bool isRequiredCamera(std::uint32_t cameraIndex) const;
    void trimBuffer(CameraBuffer& buffer);
    void dropFront(CameraBuffer& buffer);
    void incrementInputFrames();
    void incrementIgnoredFrames();
    void incrementDroppedFrames();
    void incrementEmittedOrPushFailure(ThreadKit::Queues::QueuePushResult result);
    void setError(ErrorCode code, const char* where, const std::string& message);

    std::uint64_t syncTimestampNs(const D3D12IndexedCameraFrame& frame) const noexcept;
    static std::uint64_t hostTimestampNs(const D3D12IndexedCameraFrame& frame) noexcept;

    std::shared_ptr<D3D12IndexedFrameQueue> inputQueue_;
    std::shared_ptr<D3D12SyncedFrameQueue> outputQueue_;
    FrameSyncOptions options_;

    std::vector<CameraBuffer> buffers_;

    mutable std::mutex statsMutex_;
    FrameSyncStats stats_;
    ErrorInfo lastError_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::uint64_t nextSyncGroupId_ = 1;
};

} // namespace IC4Ext
