# IC4Ext

IC4Extは、The Imaging Source **IC Imaging Control 4 SDK**で取得したcamera frameを、Direct3D 11 / Direct3D 12のGPU resourceとして扱うC++17 libraryである。

現在のproject versionは**2.0.0**である。D3D11/D3D12とも、1つの完成Texture2Dを複数consumerへ共有する**ReadOnly frame pipeline**を正式経路とする。

```cpp
#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>
namespace Pipe11 = IC4Ext::D3D11;

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe12 = IC4Ext::D3D12;
```

## 1. Architecture summary

### D3D11

```text
IC4 ImageBuffer
    -> reusable input-buffer slot
    -> D3D11 compute conversion
    -> CameraCapture-owned FramePool Texture2D
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> central FrameSyncThread
    -> ReadOnlyFrameSetQueue x N
    -> GPU / CPU consumers
```

IC4 bytesを最終FramePool Textureへ直接変換する。中間完成Textureと追加`CopyResource`は生成しない。複数producer/consumerが共有するimmediate contextは、context単位のtransaction mutexで保護する。

### D3D12

```text
IC4 ImageBuffer
    -> UploadRing + reusable input buffer
    -> D3D12 compute conversion
    -> CameraCapture-owned FramePool Texture2D
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> central FrameSyncThread
    -> ReadOnlyFrameSetQueue x N
    -> GPU / CPU consumers
```

両backendとも`CameraCaptureThread`と`FrameSyncThread`はoutputごとのGPU texture copyを行わない。fan-outでは同じReadOnly resourceへのshared handleを渡す。

## 2. Compatibility policy

v1 physical-copy fan-out APIとのsource compatibilityは保証しない。新規コードは`IC4Ext::D3D11`または`IC4Ext::D3D12`のReadOnly APIを使う。

D3D12実装の一部は物理移動途中で`include/IC4Ext/V2` / `src/V2`に残るが、public APIとCMake build entryではない。

## 3. Current implementation status

| 項目 | D3D11 | D3D12 |
|---|---:|---:|
| ReadOnly camera capture | 実装済み | 実装済み |
| IC4 bytesから最終FramePoolへのdirect conversion | 実装済み | 実装済み |
| CameraCapture-owned FramePool | 実装済み | 実装済み |
| reusable input buffer | 実装済み | 実装済み |
| central timestamp-nearest sync | 実装済み | 実装済み |
| runtime output register/update/remove | 実装済み | 実装済み |
| required cameras / FPS / priority | 実装済み | 実装済み |
| ReadOnly lifetime tracker | 実装済み | 実装済み |
| ReadOnly readback | 実装済み | 実装済み |
| camera-free SyntheticFrameSource | 実装済み | 実装済み |
| 10-pipeline stress sample | 実装済み | 実装済み |
| IC4 JSON / runtime property setters | 実装済み | 実装済み |
| Chunk metadata / performance snapshot | 実装済み | 実装済み |
| 10/12/16bit and packed formats | 未実装 | 未実装 |
| 2-camera 160 fps long-run acceptance | 未検証 | 検証中 |

動画encoderはcamera GPU resource提供libraryであるIC4Ext本体の責務に含めない。OpenCV `VideoWriter`はstress sampleのconsumer workloadである。

## 4. Features

- IC4 camera frameをD3D11/D3D12 GPU resourceへ変換。
- device selection: `serial -> uniqueName -> deviceIndex -> first device`。
- IC Capture 4からexportしたJSON `devices[n].state`を適用。
- `ReadMode::LatestFrame` / `ReadMode::NextFrame`。
- frame number、device timestamp、host received time。
- block id、exposure、gain、IMX174、MultiFrameSet chunk fields。
- hardware/software trigger設定helper。
- exposure、gain、gamma、fps、ROI、offset、PixelFormat、任意property setter。
- GPU producer-ready fence token。
- FramePoolとReadOnly共有fan-out。
- timestamp-nearest中央同期。
- outputごとのrequired cameras、FPS、priority、enabled。
- outputの実行中追加、更新、queue差替え、削除。
- consumer GPU completionまでのlifetime tracking。
- GPU frameからtight-packed `CpuFrame`へのreadback。
- D3D11 staging texture cache / D3D12 readback buffer cache。
- camera-free source injection。
- D3D11/D3D12 10-pipeline stress sample。

## 5. Supported formats

### Camera input

```text
Mono8
BayerRG8 / BayerGR8 / BayerGB8 / BayerBG8
BGR8
BGRa8
```

### GPU output

```text
R8
RGBA8
```

### CPU readback

```text
Gray8
RGBA8
RGB8
BGR8
```

### Supported conversion

```text
Mono8    -> R8
Mono8    -> RGBA8
Bayer*8  -> RGBA8
BGR8     -> RGBA8
BGRa8    -> RGBA8
```

## 6. Requirements

- Windows 10/11
- Visual Studio 2022または互換MSVC toolchain
- CMake 3.21+
- IC Imaging Control 4 SDK
- D3D11.4対応環境（D3D11を使う場合）
- D3D12対応環境（D3D12を使う場合）

IC4 SDKは自動取得しない。`IC4_SDK_ROOT`またはinstallerが設定する`IC4PATH`を使う。

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"
```

## 7. Dependencies

通常はCMake `FetchContent`で取得する。

```text
D3D11Helper v1.12.1
D3D12Helper v1.12.1
ThreadKit main
nlohmann/json v3.11.3
```

IC4Ext library本体はOpenCVに依存しない。`MultiPipelineStressD3D11`と`MultiPipelineStressD3D12`だけがOpenCVを要求する。

## 8. Build: D3D11 ReadOnly

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"
set "IC4EXT_OK=1"

git fetch origin
git switch agent/ic4ext-v2-d3d11-readonly-foundation
git pull --ff-only origin agent/ic4ext-v2-d3d11-readonly-foundation

cmake -S . -B out\build\v2_d3d11 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4_SDK_ROOT="%IC4_SDK_ROOT%" ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=OFF ^
  -DIC4EXT_BUILD_SAMPLES=OFF ^
  -DIC4EXT_BUILD_TESTS=ON

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] configure failed. CMD remains open.

if "%IC4EXT_OK%"=="1" cmake --build out\build\v2_d3d11 ^
  --config Debug ^
  --target ^
    test_d3d11_readonly_pipeline ^
    test_d3d11_pooled_converter_device ^
    test_d3d11_synthetic_source_sync_integration ^
  --parallel

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] build failed. CMD remains open.

if "%IC4EXT_OK%"=="1" ctest --test-dir out\build\v2_d3d11 ^
  -C Debug ^
  --output-on-failure ^
  -R "test_d3d11_(readonly_pipeline|pooled_converter_device|synthetic_source_sync_integration)"
```

## 9. Build: D3D12 ReadOnly

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"
set "IC4EXT_OK=1"

git fetch origin
git switch agent/ic4ext-v2-d3d12-foundation
git pull --ff-only origin agent/ic4ext-v2-d3d12-foundation

cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] configure failed. CMD remains open.
```

## 10. OpenCV stress samples

OpenCV root例:

```text
C:\personal\iwatake\library\opencv
```

```bat
set "OPENCV_ROOT=C:\personal\iwatake\library\opencv"
set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc17\lib"
if not exist "%OpenCV_DIR%\OpenCVConfig.cmake" set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc16\lib"
set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc17\bin"
if not exist "%OPENCV_BIN%" set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc16\bin"
set "PATH=%OPENCV_BIN%;%PATH%"
```

D3D11:

```bat
cmake -S . -B out\build\v2_d3d11 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=OFF ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DOpenCV_DIR="%OpenCV_DIR%"

cmake --build out\build\v2_d3d11 ^
  --config Release ^
  --target MultiPipelineStressD3D11 ^
  --parallel
```

D3D12:

```bat
cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON ^
  -DOpenCV_DIR="%OpenCV_DIR%"

cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target MultiPipelineStressD3D12 ^
  --parallel
```

詳細:

```text
docs/READONLY_FRAME_USAGE.md
docs/d3d11/READONLY_PIPELINE.md
samples/MultiPipelineStressD3D11/README.md
docs/d3d12/READONLY_PIPELINE.md
samples/MultiPipelineStressD3D12/README.md
```
