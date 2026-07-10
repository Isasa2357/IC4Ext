#pragma once

// Backend selection macros.
//
// CMake defines these values on target IC4Ext. If a user includes headers
// without CMake, the default is the original lightweight D3D11-only mode.
#ifndef IC4EXT_ENABLE_D3D11
#define IC4EXT_ENABLE_D3D11 1
#endif

#ifndef IC4EXT_ENABLE_D3D12
#define IC4EXT_ENABLE_D3D12 0
#endif

#if !IC4EXT_ENABLE_D3D11 && !IC4EXT_ENABLE_D3D12
#error "IC4Ext requires at least one backend: define IC4EXT_ENABLE_D3D11 or IC4EXT_ENABLE_D3D12 to 1."
#endif

#if IC4EXT_ENABLE_D3D11 && IC4EXT_ENABLE_D3D12
#define IC4EXT_ENABLE_MULTI_BACKEND 1
#else
#define IC4EXT_ENABLE_MULTI_BACKEND 0
#endif
