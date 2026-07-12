#include "IC4Ext/D3D11/ReadOnlyFrameSet.hpp"

#include <stdexcept>
#include <utility>

namespace IC4Ext::D3D11 {

struct D3D11ReadOnlyFrameSet::Storage
{
    SyncGroupId syncGroupId = 0;
    std::uint64_t referenceTimestampNs = 0;
    std::chrono::steady_clock::time_point completedTime{};
    FrameList frames;
};

D3D11ReadOnlyFrameSet::D3D11ReadOnlyFrameSet(
    std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage))
{
}

D3D11ReadOnlyFrameSet D3D11ReadOnlyFrameSet::Create(
    SyncGroupId syncGroupId,
    std::uint64_t referenceTimestampNs,
    std::chrono::steady_clock::time_point completedTime,
    FrameList frames)
{
    if (syncGroupId == 0 || frames.empty()) return {};
    auto storage = std::make_shared<Storage>();
    storage->syncGroupId = syncGroupId;
    storage->referenceTimestampNs = referenceTimestampNs;
    storage->completedTime = completedTime;
    storage->frames = std::move(frames);
    return D3D11ReadOnlyFrameSet(std::move(storage));
}

bool D3D11ReadOnlyFrameSet::valid() const noexcept
{
    return storage_ && storage_->syncGroupId != 0 && !storage_->frames.empty();
}

SyncGroupId D3D11ReadOnlyFrameSet::syncGroupId() const noexcept
{
    return storage_ ? storage_->syncGroupId : 0;
}

std::uint64_t D3D11ReadOnlyFrameSet::referenceTimestampNs() const noexcept
{
    return storage_ ? storage_->referenceTimestampNs : 0;
}

std::chrono::steady_clock::time_point
D3D11ReadOnlyFrameSet::completedTime() const noexcept
{
    return storage_
        ? storage_->completedTime
        : std::chrono::steady_clock::time_point{};
}

std::size_t D3D11ReadOnlyFrameSet::size() const noexcept
{
    return storage_ ? storage_->frames.size() : 0;
}

bool D3D11ReadOnlyFrameSet::empty() const noexcept { return size() == 0; }

const D3D11ReadOnlyFrameSet::FrameList&
D3D11ReadOnlyFrameSet::frames() const noexcept
{
    static const FrameList empty;
    return storage_ ? storage_->frames : empty;
}

const D3D11IndexedReadOnlyFrame& D3D11ReadOnlyFrameSet::operator[](
    std::size_t index) const noexcept
{
    return storage_->frames[index];
}

const D3D11IndexedReadOnlyFrame& D3D11ReadOnlyFrameSet::at(
    std::size_t index) const
{
    if (!storage_) throw std::out_of_range("D3D11ReadOnlyFrameSet is empty");
    return storage_->frames.at(index);
}

const D3D11ReadOnlyFrame* D3D11ReadOnlyFrameSet::find(
    CameraId cameraId) const noexcept
{
    if (!storage_) return nullptr;
    for (const auto& indexed : storage_->frames) {
        if (indexed.cameraId == cameraId) return &indexed.frame;
    }
    return nullptr;
}

D3D11ReadOnlyFrameSet::const_iterator
D3D11ReadOnlyFrameSet::begin() const noexcept
{
    return frames().begin();
}

D3D11ReadOnlyFrameSet::const_iterator
D3D11ReadOnlyFrameSet::end() const noexcept
{
    return frames().end();
}

} // namespace IC4Ext::D3D11
