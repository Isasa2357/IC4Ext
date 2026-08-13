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

    // Runtime configuration updates are allowed only while supply is active.
    // The queue itself is immutable: replacement must be expressed as adding a
    // new output and retiring the old output through the two-stage lifecycle.
    bool updateOutput(FrameSyncOutputId outputId, FrameSyncOutputConfig config);

    // Synchronous supply barrier. When this succeeds, no push to the output is
    // in flight and no future push to that output can start. Existing queued
    // frame sets remain available to the consumer. This transition is terminal.
    bool stopOutputSupply(FrameSyncOutputId outputId);

    // Second lifecycle stage. This is valid only after supply has stopped (or
    // the output faulted). It closes the queue, wakes waiters, and releases the
    // FrameSyncThread's ownership. Closing does not clear queued frame sets, so
    // consumers may drain them or explicitly clear them before/after closing.
    bool closeOutputChannel(FrameSyncOutputId outputId);

    std::optional<FrameSyncOutputConfig> outputConfig(
        FrameSyncOutputId outputId) const;
    std::optional<FrameSyncOutputState> outputState(
        FrameSyncOutputId outputId) const;
    std::vector<FrameSyncOutputInfo> outputs() const;
    std::optional<FrameSyncOutputStats> outputStats(
        FrameSyncOutputId outputId) const;
    std::optional<ErrorInfo> outputLastError(
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
