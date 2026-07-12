#pragma once

#include "IC4Ext/D3D12/FrameSyncTypes.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrame.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace IC4Ext::D3D12 {

struct D3D12IndexedReadOnlyFrame
{
    CameraId cameraId = 0;
    D3D12ReadOnlyFrame frame;
};

class D3D12ReadOnlyFrameSet final
{
public:
    using FrameList = std::vector<D3D12IndexedReadOnlyFrame>;
    using const_iterator = FrameList::const_iterator;

    D3D12ReadOnlyFrameSet() noexcept = default;

    static D3D12ReadOnlyFrameSet Create(
        SyncGroupId syncGroupId,
        std::uint64_t referenceTimestampNs,
        std::chrono::steady_clock::time_point completedTime,
        FrameList frames);

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    SyncGroupId syncGroupId() const noexcept;
    std::uint64_t referenceTimestampNs() const noexcept;
    std::chrono::steady_clock::time_point completedTime() const noexcept;

    std::size_t size() const noexcept;
    bool empty() const noexcept;
    const FrameList& frames() const noexcept;
    const D3D12IndexedReadOnlyFrame& operator[](std::size_t index) const noexcept;
    const D3D12IndexedReadOnlyFrame& at(std::size_t index) const;
    const D3D12ReadOnlyFrame* find(CameraId cameraId) const noexcept;
    bool contains(CameraId cameraId) const noexcept { return find(cameraId) != nullptr; }

    const_iterator begin() const noexcept;
    const_iterator end() const noexcept;

private:
    struct Storage;
    explicit D3D12ReadOnlyFrameSet(std::shared_ptr<const Storage> storage) noexcept;
    std::shared_ptr<const Storage> storage_;
};

using IndexedReadOnlyFrame = D3D12IndexedReadOnlyFrame;
using ReadOnlyFrameSet = D3D12ReadOnlyFrameSet;

} // namespace IC4Ext::D3D12
