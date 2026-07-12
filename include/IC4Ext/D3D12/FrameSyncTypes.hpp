#pragma once

#include "IC4Ext/V2/Core/FrameSyncTypes.hpp"

namespace IC4Ext::D3D12 {

using CameraId = ::IC4Ext::V2::CameraId;
using SyncGroupId = ::IC4Ext::V2::SyncGroupId;
using FrameSyncOutputId = ::IC4Ext::V2::FrameSyncOutputId;

inline constexpr FrameSyncOutputId InvalidFrameSyncOutputId =
    ::IC4Ext::V2::InvalidFrameSyncOutputId;

using FrameRateMode = ::IC4Ext::V2::FrameRateMode;
using FrameRateLimit = ::IC4Ext::V2::FrameRateLimit;
using FrameSyncPolicy = ::IC4Ext::V2::FrameSyncPolicy;
using FrameSyncTimestampSource = ::IC4Ext::V2::FrameSyncTimestampSource;
using FrameSyncConfig = ::IC4Ext::V2::FrameSyncConfig;
using FrameSyncStats = ::IC4Ext::V2::FrameSyncStats;

} // namespace IC4Ext::D3D12
