#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace IC4Ext {

class D3D11FrameSyncThread
{
public:
    D3D11FrameSyncThread(std::shared_ptr<D3D11IndexedFrameQueue> inputQueue,
                         std::shared_ptr<D3D11SyncedFrameQueue> outputQueue,
                         FrameSyncOptions options = {});
    ~D3D11FrameSyncThread();

    D3D11FrameSyncThread(const D3D11FrameSyncThread&) = delete;
    D3D11FrameSyncThread& operator=(const D3D11FrameSyncThread&) = delete;

    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    FrameSyncStats stats() const;
    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    void workerLoop();
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    std::shared_ptr<D3D11IndexedFrameQueue> inputQueue_;
    std::shared_ptr<D3D11SyncedFrameQueue> outputQueue_;
    FrameSyncOptions options_;

    mutable std::mutex statsMutex_;
    FrameSyncStats stats_;
    ErrorInfo lastError_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::uint64_t nextSyncGroupId_ = 1;
};

} // namespace IC4Ext
