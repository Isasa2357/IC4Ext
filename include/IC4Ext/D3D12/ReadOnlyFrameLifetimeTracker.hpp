#pragma once

#include "IC4Ext/D3D12/D3D12ReadyToken.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameSet.hpp"

#include <D3D12Helper/D3D12Core/D3D12Queue.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace IC4Ext::D3D12 {

struct ReadOnlyFrameLifetimeTrackerStats
{
    std::size_t retainedFrames = 0;
    std::uint64_t retainedTotal = 0;
    std::uint64_t collectedTotal = 0;
};

// Enqueue a GPU-side wait so a consumer queue does not read a frame before the
// producer queue has finished writing it. Invalid ready tokens are treated as
// already satisfied; invalid frames return false.
bool WaitForReadOnlyFrameReadyOnQueue(
    D3D12CoreLib::D3D12Queue& queue,
    const ReadOnlyFrame& frame) noexcept;

bool WaitForReadyTokenOnQueue(
    D3D12CoreLib::D3D12Queue& queue,
    const ::IC4Ext::D3D12ReadyToken& token) noexcept;

// Holds read-only input frames until a consumer completion fence has passed.
// This prevents the producer-side frame pool from reusing a texture while a
// downstream GPU workload may still be reading it.
class ReadOnlyFrameLifetimeTracker final
{
public:
    ReadOnlyFrameLifetimeTracker() = default;

    ReadOnlyFrameLifetimeTracker(const ReadOnlyFrameLifetimeTracker&) = delete;
    ReadOnlyFrameLifetimeTracker& operator=(const ReadOnlyFrameLifetimeTracker&) = delete;

    bool retainUntil(ReadOnlyFrame frame,
                     ::IC4Ext::D3D12ReadyToken consumerCompletion);

    bool retainUntil(const ReadOnlyFrameSet& frameSet,
                     ::IC4Ext::D3D12ReadyToken consumerCompletion);

    std::size_t collectCompleted();
    void clear() noexcept;

    bool waitAllAndClear(std::uint32_t timeoutMs = INFINITE) noexcept;

    std::size_t retainedFrameCount() const;
    ReadOnlyFrameLifetimeTrackerStats stats() const;

private:
    struct Entry
    {
        ReadOnlyFrame frame;
        ::IC4Ext::D3D12ReadyToken completion;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t retainedTotal_ = 0;
    std::uint64_t collectedTotal_ = 0;
};

} // namespace IC4Ext::D3D12
