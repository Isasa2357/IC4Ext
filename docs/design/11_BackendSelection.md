# 11. Backend Selection

## Purpose

IC4Ext supports selecting which graphics backend is exposed and built. The selection must work both at CMake target level and at aggregate-header include level.

## Public macros

The aggregate header `IC4Ext/IC4Ext.hpp` includes `IC4Ext/BackendConfig.hpp` first. The following macros decide which backend headers are pulled in:

```cpp
IC4EXT_ENABLE_D3D11
IC4EXT_ENABLE_D3D12
```

At least one must be `1`.

CMake defines both macros publicly on target `IC4Ext` according to the options below. If a user includes headers manually without CMake, `BackendConfig.hpp` defaults to D3D11-only mode:

```cpp
IC4EXT_ENABLE_D3D11 = 1
IC4EXT_ENABLE_D3D12 = 0
```

## CMake options

```cmake
option(IC4EXT_ENABLE_D3D11 "Build and expose the D3D11 backend" ON)
option(IC4EXT_ENABLE_D3D12 "Build and expose the D3D12 backend" ON)
```

These options control:

- backend source file registration,
- helper dependency fetching,
- helper include directories,
- helper target linking,
- system library linking,
- sample registration,
- test registration,
- public preprocessor definitions.

## Common control definitions

`CameraControlCommand` and `ICameraControlSink` live under `IC4Ext/Core/CameraControl.hpp`. D3D11 and D3D12 expose backend-specific aliases:

```cpp
using D3D11CameraControlCommand = CameraControlCommand;
using D3D12CameraControlCommand = CameraControlCommand;
using ID3D11CameraControlSink = ICameraControlSink;
using ID3D12CameraControlSink = ICameraControlSink;
```

This avoids D3D12 headers depending on D3D11 headers when `IC4EXT_ENABLE_D3D11=OFF`.
