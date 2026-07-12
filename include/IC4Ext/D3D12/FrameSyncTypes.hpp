#pragma once

#define V2 D3D12
#include "IC4Ext/V2/Core/FrameSyncTypes.hpp"
#undef V2

namespace IC4Ext::D3D12 {

inline constexpr FrameSyncOutputId InvalidFrameSyncOutputIdValue =
    InvalidFrameSyncOutputId;

} // namespace IC4Ext::D3D12
