#pragma once

#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrameSet.hpp"

#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace IC4Ext::D3D11 {

struct ReadOnlyFrameLifetimeTrackerStats
{
    std::size_t retainedFrames = 0;
    std::uint64_t retainedTotal = 0;
    std::uint64_t collectedTotal = 0;
};

bool WaitForReadyTokenOnContext(
    ID3D11DeviceContext* context,
    const ::IC4Ext::D3D11ReadyToken& token) noexcept;

bool WaitForReadOnlyFrameReadyOnContext(
    ID3D11DeviceContext* context,
    const ReadOnlyFrame& frame) noexcept;

class ReadOnlyFrameLifetimeTracker final
{
public:
    ReadOnlyFrameLifetimeTracker() = default;
    ReadOnlyFrameLifetimeTracker(const ReadOnlyFrameLifetimeTracker&) = delete;
    ReadOnlyFrameLifetimeTracker& operator=(
        const ReadOnlyFrameLifetimeTracker&) = delete;

    bool retainUntil(
        ReadOnlyFrame frame,
        ::IC4Ext::D3D11ReadyToken consumerCompletion);
    bool retainUntil(
        const ReadOnlyFrameSet& frameSet,
        ::IC4Ext::D3D11ReadyToken consumerCompletion);

    std::size_t collectCompleted();
    void clear() noexcept;
    bool waitAllAndClear(std::uint32_t timeoutMs = INFINITE) noexcept;
    std::size_t retainedFrameCount() const;
    ReadOnlyFrameLifetimeTrackerStats stats() const;

private:
    struct Entry
    {
        ReadOnlyFrame frame;
        ::IC4Ext::D3D11ReadyToken completion;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t retainedTotal_ = 0;
    std::uint64_t collectedTotal_ = 0;
};

} // namespace IC4Ext::D3D11
