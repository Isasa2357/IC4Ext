#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace IC4Ext {

struct FrameTiming
{
    std::uint64_t frameNumber = 0;
    std::uint64_t deviceTimestampNs = 0;
    std::chrono::steady_clock::time_point hostReceivedTime{};
};

struct FrameFormatMetadata
{
    CameraPixelFormat requestedFormat = CameraPixelFormat::BayerRG8;
    CameraPixelFormat actualInputFormat = CameraPixelFormat::BayerRG8;
    GpuFrameFormat outputFormat = GpuFrameFormat::RGBA8;
    int width = 0;
    int height = 0;
    std::size_t inputRowPitchBytes = 0;
};

struct FrameChunkMetadata
{
    bool hasBlockId = false;
    std::uint64_t blockId = 0;

    bool hasExposureTime = false;
    double exposureTimeUs = 0.0;

    bool hasGain = false;
    double gain = 0.0;

    bool hasIMX174FrameId = false;
    std::int64_t imx174FrameId = 0;

    bool hasIMX174FrameSet = false;
    std::int64_t imx174FrameSet = 0;

    bool hasMultiFrameSetId = false;
    std::int64_t multiFrameSetId = 0;

    bool hasMultiFrameSetFrameId = false;
    std::int64_t multiFrameSetFrameId = 0;

    bool hasAny() const noexcept
    {
        return hasBlockId || hasExposureTime || hasGain ||
               hasIMX174FrameId || hasIMX174FrameSet ||
               hasMultiFrameSetId || hasMultiFrameSetFrameId;
    }
};

struct FrameReadbackCacheStats
{
    std::uint64_t readbacks = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t resourceRebuilds = 0;
    std::uint64_t bytesAllocated = 0;
};

struct CameraCaptureStats
{
    std::uint64_t receivedBuffers = 0;
    std::uint64_t droppedPendingBuffers = 0;
    std::uint64_t readFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t conversionFailures = 0;
};

struct CameraThreadStats
{
    std::uint64_t readFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t pushedFrames = 0;
    std::uint64_t pushFailures = 0;
    std::uint64_t copiedFrames = 0;
    std::uint64_t copyFailures = 0;
    std::uint64_t noOutputDrops = 0;
};

struct FrameSyncStats
{
    std::uint64_t inputFrames = 0;
    std::uint64_t emittedSets = 0;
    std::uint64_t ignoredFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t pushFailures = 0;
};

} // namespace IC4Ext
