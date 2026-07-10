# 08. Build and sample/test guide

このファイルは v1.3 readback 版の build / sample / test 手順です。

## Requirements

- Windows 10/11
- Visual Studio Developer Command Prompt or x64 Native Tools Command Prompt
- CMake 3.21+
- IC Imaging Control 4 SDK
- `dxcompiler.dll` / `dxil.dll` for D3D shader runtime support

## CMD build command

以下は現在の作業ディレクトリが IC4Ext root、つまり `CMakeLists.txt` がある場所である前提です。`cd` は含めていません。

```bat
set "IC4_SDK_ROOT=C:\Users\user\AppData\Local\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "PATH=%IC4_SDK_ROOT%\bin\x64;%PATH%"

set "IC4EXT_DXC_RUNTIME_DIR=%CD%\package\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\bin\x64"
set "PATH=%IC4EXT_DXC_RUNTIME_DIR%;%PATH%"

dir "%IC4EXT_DXC_RUNTIME_DIR%\dxcompiler.dll"
dir "%IC4EXT_DXC_RUNTIME_DIR%\dxil.dll"

where cmake
where ctest
where cl

rmdir /s /q out\build\default 2>nul

cmake -S . -B out\build\default ^
  -DIC4_SDK_ROOT="%IC4_SDK_ROOT%" ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%" ^
  -DD3D11HELPER_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%" ^
  -DD3D12HELPER_DXC_RUNTIME_DIR="%IC4EXT_DXC_RUNTIME_DIR%" ^
  -DIC4EXT_FETCH_DEPENDENCIES=ON ^
  -DIC4EXT_FETCH_D3D11HELPER=ON ^
  -DIC4EXT_FETCH_D3D12HELPER=ON ^
  -DIC4EXT_FETCH_THREADKIT=ON ^
  -DIC4EXT_FETCH_NLOHMANN_JSON=ON

cmake --build out\build\default --config Debug

ctest --test-dir out\build\default -C Debug --output-on-failure
```

DXC が `packages` 以下にある場合は `IC4EXT_DXC_RUNTIME_DIR` の `package` を `packages` に変えてください。

## CMake options

```txt
IC4EXT_BUILD_SAMPLES
IC4EXT_BUILD_TESTS
IC4EXT_ENABLE_D3D11
IC4EXT_ENABLE_D3D12
IC4EXT_FETCH_DEPENDENCIES
IC4EXT_FETCH_D3D11HELPER
IC4EXT_FETCH_D3D12HELPER
IC4EXT_FETCH_THREADKIT
IC4EXT_FETCH_NLOHMANN_JSON
IC4EXT_DXC_RUNTIME_DIR
D3D11HELPER_ROOT
D3D12HELPER_ROOT
THREADKIT_ROOT
```

## Samples

### SingleCameraLog

D3D11 GPU texture として frame を取得し、metadata を表示します。

```bat
out\build\default\samples\SingleCameraLog\Debug\SingleCameraLog.exe ^
  --device-index 0 --format BGR8 --output RGBA8 --frames 300
```

### SingleCameraLogD3D12

D3D12 GPU texture として frame を取得し、metadata を表示します。

```bat
out\build\default\samples\SingleCameraLogD3D12\Debug\SingleCameraLogD3D12.exe ^
  --device-index 0 --format BGR8 --output RGBA8 --frames 300
```

### SingleCameraReadback

D3D11 GPU frame を `CpuFrame` に readback し、PGM/PPM に保存します。

```bat
out\build\default\samples\SingleCameraReadback\Debug\SingleCameraReadback.exe ^
  --device-index 0 --output RGBA8 --cpu-format BGR8 --out frame.ppm
```

### SingleCameraReadbackD3D12

D3D12 GPU frame を `CpuFrame` に readback し、PGM/PPM に保存します。

```bat
out\build\default\samples\SingleCameraReadbackD3D12\Debug\SingleCameraReadbackD3D12.exe ^
  --device-index 0 --output RGBA8 --cpu-format BGR8 --out frame_d3d12.ppm
```

## JSON state

IC Capture 4 から export した JSON を使う場合:

```bat
out\build\default\samples\SingleCameraLogD3D12\Debug\SingleCameraLogD3D12.exe ^
  --device-index 0 --ic4-json C:\path\to\camera_state.json ^
  --offset-x 0 --offset-y 0 --output RGBA8 --frames 300
```

`--force-format 1` を指定すると、JSON 内の `PixelFormat` より `--format` を優先します。

## Tests

```bat
ctest --test-dir out\build\default -C Debug --output-on-failure
```

現在の test target:

```txt
test_core
test_cpu_frame
test_backend_config
test_d3d11_frame_readback
test_single_camera_smoke
test_d3d12_core
test_d3d12_shader_reference
test_d3d12_dummy_camera_capture
test_d3d12_frame_readback
test_d3d12_frame_sync_thread
test_d3d12_shader_compile
```

`test_single_camera_smoke` は実カメラ依存です。GPU/device 作成や camera open ができない場合、一部のテストは skip return code `77` で終了します。
