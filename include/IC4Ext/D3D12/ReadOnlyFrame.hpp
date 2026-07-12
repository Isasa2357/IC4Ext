#pragma once

// Internal implementation header still lives under IC4Ext/V2 while the files are
// being moved. Macro-remap the namespace at inclusion time so the public type is
// defined as IC4Ext::D3D12::D3D12ReadOnlyFrame, not IC4Ext::V2::...
#define V2 D3D12
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrame.hpp"
#undef V2

namespace IC4Ext::D3D12 {

using ReadOnlyFrame = D3D12ReadOnlyFrame;

} // namespace IC4Ext::D3D12
