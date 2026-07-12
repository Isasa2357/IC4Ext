// Transitional implementation wrapper.
// The implementation body is being moved from src/V2. Compile it directly into
// IC4Ext::D3D12 so public symbols no longer live in IC4Ext::V2.
#define V2 D3D12
#include "../V2/D3D12/D3D12ReadOnlyFrame.cpp"
#undef V2
