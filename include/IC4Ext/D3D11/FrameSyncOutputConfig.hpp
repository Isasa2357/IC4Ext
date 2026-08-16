#pragma once

#include "IC4Ext/D3D11/FrameSyncTypes.hpp"

#include <cstdint>
#include <vector>

namespace IC4Ext::D3D11 {

enum class FrameSyncOutputState : std::uint32_t
{
    Active = 0,
    SupplyStopped = 1,
    Faulted = 2,
};

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
    FrameSyncOutputState state = FrameSyncOutputState::Active;
};

struct FrameSyncOutputStats
{
    std::uint64_t consideredSets = 0;
    std::uint64_t skippedByFrameRate = 0;
    std::uint64_t emittedSets = 0;
    std::uint64_t queueDrops = 0;
    std::uint64_t disabledSkips = 0;
    std::uint64_t dispatchErrors = 0;
    std::uint64_t closedQueuePushes = 0;
};

} // namespace IC4Ext::D3D11
