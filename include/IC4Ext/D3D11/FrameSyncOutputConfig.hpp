#pragma once

#include "IC4Ext/D3D11/FrameSyncTypes.hpp"

#include <cstdint>
#include <vector>

namespace IC4Ext::D3D11 {

struct FrameSyncOutputConfig
{
    std::vector<CameraId> requiredCameras;
    FrameRateLimit frameRate = FrameRateLimit::Maximum();
    std::int32_t priority = 0;
    bool enabled = true;
};

struct FrameSyncOutputInfo
{
    FrameSyncOutputId id = InvalidFrameSyncOutputId;
    FrameSyncOutputConfig config;
    std::uint64_t registrationOrder = 0;
};

struct FrameSyncOutputStats
{
    std::uint64_t consideredSets = 0;
    std::uint64_t skippedByFrameRate = 0;
    std::uint64_t emittedSets = 0;
    std::uint64_t queueDrops = 0;
    std::uint64_t disabledSkips = 0;
};

} // namespace IC4Ext::D3D11
