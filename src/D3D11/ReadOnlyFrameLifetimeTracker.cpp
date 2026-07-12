#include "IC4Ext/D3D11/ReadOnlyFrameLifetimeTracker.hpp"

#include <algorithm>
#include <utility>

namespace IC4Ext::D3D11 {

bool WaitForReadyTokenOnContext(
    ID3D11DeviceContext* context,
    const ::IC4Ext::D3D11ReadyToken& token) noexcept
{
    if (!token.isValid()) return true;
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))) || !context4) {
        return token.wait();
    }
    return SUCCEEDED(context4->Wait(token.fence.Get(), token.value));
}

bool WaitForReadOnlyFrameReadyOnContext(
    ID3D11DeviceContext* context,
    const ReadOnlyFrame& frame) noexcept
{
    if (!frame) return false;
    return WaitForReadyTokenOnContext(context, frame.readyToken());
}

bool ReadOnlyFrameLifetimeTracker::retainUntil(
    ReadOnlyFrame frame,
    ::IC4Ext::D3D11ReadyToken consumerCompletion)
{
    if (!frame || !consumerCompletion.isValid()) return false;
    if (consumerCompletion.isReady()) return true;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(Entry{
        std::move(frame),
        std::move(consumerCompletion)});
    ++retainedTotal_;
    return true;
}

bool ReadOnlyFrameLifetimeTracker::retainUntil(
    const ReadOnlyFrameSet& frameSet,
    ::IC4Ext::D3D11ReadyToken consumerCompletion)
{
    if (!frameSet || !consumerCompletion.isValid()) return false;
    if (consumerCompletion.isReady()) return true;

    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t added = 0;
    for (const auto& indexed : frameSet.frames()) {
        if (indexed.frame) {
            entries_.push_back(Entry{indexed.frame, consumerCompletion});
            ++added;
        }
    }
    retainedTotal_ += static_cast<std::uint64_t>(added);
    return added != 0;
}

std::size_t ReadOnlyFrameLifetimeTracker::collectCompleted()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = entries_.size();
    entries_.erase(
        std::remove_if(
            entries_.begin(),
            entries_.end(),
            [](const Entry& entry) { return entry.completion.isReady(); }),
        entries_.end());
    const auto collected = before - entries_.size();
    collectedTotal_ += static_cast<std::uint64_t>(collected);
    return collected;
}

void ReadOnlyFrameLifetimeTracker::clear() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    collectedTotal_ += static_cast<std::uint64_t>(entries_.size());
    entries_.clear();
}

bool ReadOnlyFrameLifetimeTracker::waitAllAndClear(
    std::uint32_t timeoutMs) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (!entry.completion.wait(timeoutMs)) return false;
    }
    collectedTotal_ += static_cast<std::uint64_t>(entries_.size());
    entries_.clear();
    return true;
}

std::size_t ReadOnlyFrameLifetimeTracker::retainedFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

ReadOnlyFrameLifetimeTrackerStats ReadOnlyFrameLifetimeTracker::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadOnlyFrameLifetimeTrackerStats{
        entries_.size(),
        retainedTotal_,
        collectedTotal_};
}

} // namespace IC4Ext::D3D11
