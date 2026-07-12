#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/FrameQueues.hpp"
#include "IC4Ext/D3D11/FrameSyncOutputConfig.hpp"
#include "IC4Ext/D3D11/FrameSyncTypes.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace IC4Ext::D3D11 {

class D3D11FrameSyncThread final
{
public:
    D3D11FrameSyncThread(
        std::shared_ptr<D3D11IndexedReadOnlyFrameQueue> inputQueue,
        FrameSyncConfig config);
    ~D3D11FrameSyncThread();

    D3D11FrameSyncThread(const D3D11FrameSyncThread&) = delete;
    D3D11FrameSyncThread& operator=(const D3D11FrameSyncThread&) = delete;
    D3D11FrameSyncThread(D3D11FrameSyncThread&&) = delete;
    D3D11FrameSyncThread& operator=(D3D11FrameSyncThread&&) = delete;

    bool start();
    void requestStop();
    void join();
    void stopAndJoin();
    bool isRunning() const noexcept;

    FrameSyncOutputId registerOutput(
        std::shared_ptr<D3D11ReadOnlyFrameSetQueue> outputQueue,
        FrameSyncOutputConfig config);
    bool updateOutput(FrameSyncOutputId outputId, FrameSyncOutputConfig config);
    bool replaceOutputQueue(
        FrameSyncOutputId outputId,
        std::shared_ptr<D3D11ReadOnlyFrameSetQueue> outputQueue);
    bool unregisterOutput(FrameSyncOutputId outputId);

    std::optional<FrameSyncOutputConfig> outputConfig(
        FrameSyncOutputId outputId) const;
    std::vector<FrameSyncOutputInfo> outputs() const;
    std::optional<FrameSyncOutputStats> outputStats(
        FrameSyncOutputId outputId) const;

    const FrameSyncConfig& config() const noexcept;
    FrameSyncStats stats() const;
    ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

using FrameSyncThread = D3D11FrameSyncThread;

} // namespace IC4Ext::D3D11
