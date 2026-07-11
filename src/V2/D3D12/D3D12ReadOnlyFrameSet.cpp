#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrameSet.hpp"

#include <stdexcept>
#include <utility>

namespace IC4Ext::V2 {

struct D3D12ReadOnlyFrameSet::Storage
{
    SyncGroupId syncGroupId = 0;
    std::uint64_t referenceTimestampNs = 0;
    std::chrono::steady_clock::time_point completedTime{};
    FrameList frames;
};

D3D12ReadOnlyFrameSet::D3D12ReadOnlyFrameSet(std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage))
{
}

D3D12ReadOnlyFrameSet D3D12ReadOnlyFrameSet::Create(
    SyncGroupId syncGroupId,
    std::uint64_t referenceTimestampNs,
    std::chrono::steady_clock::time_point completedTime,
    FrameList frames)
{
    if (syncGroupId == 0 || frames.empty()) {
        return {};
    }
    auto storage = std::make_shared<Storage>();
    storage->syncGroupId = syncGroupId;
    storage->referenceTimestampNs = referenceTimestampNs;
    storage->completedTime = completedTime;
    storage->frames = std::move(frames);
    return D3D12ReadOnlyFrameSet(std::move(storage));
}

bool D3D12ReadOnlyFrameSet::valid() const noexcept
{
    return storage_ && storage_->syncGroupId != 0 && !storage_->frames.empty();
}

SyncGroupId D3D12ReadOnlyFrameSet::syncGroupId() const noexcept
{
    return storage_ ? storage_->syncGroupId : 0;
}

std::uint64_t D3D12ReadOnlyFrameSet::referenceTimestampNs() const noexcept
{
    return storage_ ? storage_->referenceTimestampNs : 0;
}

std::chrono::steady_clock::time_point D3D12ReadOnlyFrameSet::completedTime() const noexcept
{
    return storage_ ? storage_->completedTime : std::chrono::steady_clock::time_point{};
}

std::size_t D3D12ReadOnlyFrameSet::size() const noexcept
{
    return storage_ ? storage_->frames.size() : 0;
}

bool D3D12ReadOnlyFrameSet::empty() const noexcept
{
    return size() == 0;
}

const D3D12ReadOnlyFrameSet::FrameList& D3D12ReadOnlyFrameSet::frames() const noexcept
{
    static const FrameList empty;
    return storage_ ? storage_->frames : empty;
}

const D3D12IndexedReadOnlyFrame& D3D12ReadOnlyFrameSet::operator[](std::size_t index) const noexcept
{
    return storage_->frames[index];
}

const D3D12IndexedReadOnlyFrame& D3D12ReadOnlyFrameSet::at(std::size_t index) const
{
    if (!storage_) {
        throw std::out_of_range("D3D12ReadOnlyFrameSet is empty");
    }
    return storage_->frames.at(index);
}

const D3D12ReadOnlyFrame* D3D12ReadOnlyFrameSet::find(CameraId cameraId) const noexcept
{
    if (!storage_) return nullptr;
    for (const auto& indexed : storage_->frames) {
        if (indexed.cameraId == cameraId) {
            return &indexed.frame;
        }
    }
    return nullptr;
}

D3D12ReadOnlyFrameSet::const_iterator D3D12ReadOnlyFrameSet::begin() const noexcept
{
    return frames().begin();
}

D3D12ReadOnlyFrameSet::const_iterator D3D12ReadOnlyFrameSet::end() const noexcept
{
    return frames().end();
}

} // namespace IC4Ext::V2
