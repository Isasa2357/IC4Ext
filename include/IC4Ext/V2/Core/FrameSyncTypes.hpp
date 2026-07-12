#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace IC4Ext::V2 {

using CameraId = std::uint32_t;
using SyncGroupId = std::uint64_t;
using FrameSyncOutputId = std::uint64_t;

inline constexpr FrameSyncOutputId InvalidFrameSyncOutputId = 0;

enum class FrameRateMode : std::uint32_t
{
    Maximum = 0,
    Fixed = 1,
};

struct FrameRateLimit
{
    FrameRateMode mode = FrameRateMode::Maximum;
    double fps = 0.0;

    static FrameRateLimit Maximum() noexcept
    {
        return {FrameRateMode::Maximum, 0.0};
    }

    static FrameRateLimit Fixed(double value) noexcept
    {
        return {FrameRateMode::Fixed, value};
    }

    bool isValid() const noexcept
    {
        switch (mode) {
        case FrameRateMode::Maximum:
            return true;
        case FrameRateMode::Fixed:
            return std::isfinite(fps) && fps > 0.0;
        default:
            return false;
        }
    }
};

enum class FrameSyncPolicy : std::uint32_t
{
    // Match camera-provided frame numbers by absolute value.
    // Use only when all cameras expose the same counter domain.
    FrameNumberExact = 0,

    // Match camera-provided frame numbers after subtracting each camera's first
    // observed frame number in this sync thread. This is useful for hardware
    // triggered cameras whose device counters start from different values.
    FrameNumberRelative = 1,

    // Match nearest timestamps within maxTimestampDiffNs.
    TimestampNearest = 2,
};

enum class FrameSyncTimestampSource : std::uint32_t
{
    Auto = 0,
    HostReceived = 1,
    Device = 2,
};

struct FrameSyncConfig
{
    std::vector<CameraId> cameraIds;
    FrameSyncPolicy policy = FrameSyncPolicy::FrameNumberExact;
    FrameSyncTimestampSource timestampSource = FrameSyncTimestampSource::Auto;
    std::uint64_t maxTimestampDiffNs = 1'000'000;
    std::size_t maxBufferedFramesPerCamera = 8;
    std::chrono::milliseconds groupTimeout{50};

    bool isValid() const noexcept
    {
        if (cameraIds.empty() || maxBufferedFramesPerCamera == 0 || groupTimeout.count() <= 0) {
            return false;
        }
        if (policy == FrameSyncPolicy::TimestampNearest && maxTimestampDiffNs == 0) {
            return false;
        }
        std::vector<CameraId> sorted = cameraIds;
        std::sort(sorted.begin(), sorted.end());
        return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    }
};

struct FrameSyncStats
{
    std::uint64_t inputFrames = 0;
    std::uint64_t completedSets = 0;
    std::uint64_t ignoredFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t incompleteSets = 0;
    std::uint64_t totalOutputSets = 0;
    std::uint64_t totalOutputQueueDrops = 0;
};

} // namespace IC4Ext::V2
