# IC4Ext

IC4Ext は、The Imaging Source **IC Imaging Control 4 SDK** で取得したカメラフレームを、Direct3D 11 / Direct3D 12 の GPU texture として扱うための C++17 ライブラリです。

IC4 の `QueueSink` から受け取った CPU 側 image buffer を D3D11 / D3D12 texture へ変換し、同期用 metadata、非同期 capture thread、frame sync、DummyCameraCapture、IC Capture 4 から export した JSON 設定、GPU frame の CPU readback を提供します。

このパッケージの現在位置は **D3DHelper v1.12.1 対応版**です。D3D12 backend は raw D3D12 object だけを渡す初期化ではなく、`D3D12CoreLib::D3D12Core` から作った `D3D12BackendContext` を使います。D3D11 backend も、可能な範囲で D3D11Helper の resource / view / compute pipeline / binding / fence / transfer helper に寄せています。

## 現在の実装状態

| 項目 | 状態 |
|---|---|
| D3D11 camera capture | 実装済み |
| D3D12 camera capture | 実装済み。D3D12Helper 統合済み |
| IC4 JSON state 読み込み | 実装済み。`devices[n].state` を適用 |
| Runtime setters | 実装済み。露光、gain、gamma、fps、offset、ROI、任意 IC4 property |
| DummyCameraCapture | D3D11 / D3D12 ともに実装済み |
| FrameSyncThread | D3D11 / D3D12 ともに実装済み |
| CpuFrame / readback | D3D11 / D3D12 ともに実装済み |
| D3D12-D3D11 interop | 未実装 |
| 10/12/16bit pixel format | 未実装。必要になったら追加予定 |
| chunk metadata | 未実装。現状は frame number / timestamp のみ保持 |

## Features

- IC4 camera frame を D3D11 / D3D12 GPU texture として取得
- device selection: `serial -> uniqueName -> deviceIndex -> first device`
- `CameraStreamRequest::requestedFormat` を IC4 device の `PixelFormat` property として設定
- `ReadMode::LatestFrame` / `ReadMode::NextFrame` を選択可能
- `FrameTiming` として `frameNumber` / `deviceTimestampNs` / `hostReceivedTime` を保持
- D3D11.4 fence (`ID3D11Fence`) / D3D12 fence による GPU ready token
- `.hlsl` runtime compile と `.cso` load の両対応
- IC Capture 4 公式ソフトから export した JSON (`devices[n].state`) を `nlohmann/json` で読み込み
- JSON に含まれない `OffsetX` / `OffsetY` を明示指定可能
- `ExposureTime` / `Gain` / `Gamma` / `AcquisitionFrameRate` / `ROI` などを open 後に setter で変更可能
- `D3D11DummyCameraCaptureGenerator` / `D3D12DummyCameraCaptureGenerator` による、1 台の実カメラからの擬似複数カメラ生成
- GPU frame を `CpuFrame` として readback し、`Gray8` / `RGBA8` / `RGB8` / `BGR8` に変換可能

## Supported formats

### Camera input formats

現在対応している IC4 入力 format は 8bit 系のみです。

```txt
Mono8
BayerRG8 / BayerGR8 / BayerGB8 / BayerBG8
BGR8
BGRa8
```

### GPU output formats

```txt
R8
RGBA8
```

対応変換は以下です。

```txt
Mono8   -> R8
Mono8   -> RGBA8
Bayer*8 -> RGBA8
BGR8    -> RGBA8
BGRa8   -> RGBA8
```

### CPU readback formats

`D3D11CameraFrame` / `D3D12CameraFrame` は readback により共通の `CpuFrame` に変換できます。

```txt
Gray8
RGBA8
RGB8
BGR8
```

`CpuFrame` は常に tight packed です。

### 未対応 format

必要になった時点で追加する方針です。

```txt
Mono10 / Mono12 / Mono16
Bayer10 / Bayer12 / Bayer16
Bayer10p / Bayer12p
YUV / YCbCr
Polarized formats
MJPG / NV12
```

## Requirements

- Windows 10/11
- Visual Studio 2019 以降、または CMake から使える MSVC 環境
- CMake 3.21 以降
- IC Imaging Control 4 SDK
- Direct3D 11.4 compatible environment
- Direct3D 12 compatible environment when `IC4EXT_ENABLE_D3D12=ON`

IC4 SDK は自動取得しません。IC4 installer が登録する `IC4PATH` 環境変数を利用します。`IC4_SDK_ROOT` を CMake で明示しない場合、IC4Ext は `$ENV{IC4PATH}` を既定値として使います。

Typical IC4 SDK layout:

```txt
%IC4PATH%/include
%IC4PATH%/lib/x64/ic4core.lib
%IC4PATH%/bin/x64/ic4core.dll
```

## Dependencies

CMake の `FetchContent` により、通常は以下を自動取得します。

```txt
D3D11Helper v1.12.1
D3D12Helper v1.12.1
ThreadKit
nlohmann/json single header
```

外部依存として許可しているのは、D3DHelper v1.12.1、ThreadKit、nlohmann/json、boost です。IC4Ext 本体は OpenCV などには依存しません。

外部依存をローカル checkout から使う場合は、以下を指定できます。

```bat
-DD3D11HELPER_ROOT="C:/Path/To/D3D11Helper" ^
-DD3D12HELPER_ROOT="C:/Path/To/D3D12Helper" ^
-DTHREADKIT_ROOT="C:/Path/To/ThreadKit"
```

## Build

### CMD: current project layout

以下は、このフォルダでビルドするための CMD 用コマンドです。`cd` は入れていません。現在の作業ディレクトリが IC4Ext の root、つまり `CMakeLists.txt` がある場所である前提です。

IC4 SDK は installer が登録する `%IC4PATH%` を使います。`IC4PATH` が設定されていれば CMake 側でも自動的に `IC4_SDK_ROOT` の既定値になりますが、コマンドでは明示的に `-DIC4_SDK_ROOT="%IC4PATH%"` を渡しています。

`IC4EXT_DXC_RUNTIME_DIR` は `dxcompiler.dll` と `dxil.dll` があるディレクトリを指してください。現在の作業フォルダに `packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64` がある場合は、このまま使えます。

```bat
set "IC4_SDK_ROOT=%IC4PATH%"
set "PATH=%IC4_SDK_ROOT%\bin\x64;%PATH%"

set "IC4EXT_DXC_RUNTIME_DIR=%CD%\packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\bin\x64"
set "PATH=%IC4EXT_DXC_RUNTIME_DIR%;%PATH%"

echo IC4PATH=%IC4PATH%
echo IC4_SDK_ROOT=%IC4_SDK_ROOT%
echo IC4EXT_DXC_RUNTIME_DIR=%IC4EXT_DXC_RUNTIME_DIR%

dir "%IC4_SDK_ROOT%\include\ic4\ic4.h"
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
  -DIC4EXT_D3D11HELPER_GIT_TAG=v1.12.1 ^
  -DIC4EXT_D3D12HELPER_GIT_TAG=v1.12.1 ^
  -DIC4EXT_FETCH_DEPENDENCIES=ON ^
  -DIC4EXT_FETCH_D3D11HELPER=ON ^
  -DIC4EXT_FETCH_D3D12HELPER=ON ^
  -DIC4EXT_FETCH_THREADKIT=ON ^
  -DIC4EXT_FETCH_NLOHMANN_JSON=ON

cmake --build out\build\default --config Debug

ctest --test-dir out\build\default -C Debug --output-on-failure
```

もし DXC のフォルダ名が `packages` ではなく `package` の場合は、この行だけ変えてください。

```bat
set "IC4EXT_DXC_RUNTIME_DIR=%CD%\package\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\bin\x64"
```

### Backend selection

両方有効:

```bat
-DIC4EXT_ENABLE_D3D11=ON ^
-DIC4EXT_ENABLE_D3D12=ON
```

D3D11 のみ:

```bat
-DIC4EXT_ENABLE_D3D11=ON ^
-DIC4EXT_ENABLE_D3D12=OFF
```

D3D12 のみ:

```bat
-DIC4EXT_ENABLE_D3D11=OFF ^
-DIC4EXT_ENABLE_D3D12=ON
```

## Tests

CTest target:

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

`test_single_camera_smoke` は IC4 compatible camera が接続されている場合に意味を持ちます。カメラや GPU/device が利用できない環境では、一部テストは skip return code `77` を返す設計です。

## Samples

### D3D11 camera log

```bat
out\build\default\samples\SingleCameraLog\Debug\SingleCameraLog.exe ^
  --device-index 0 --width 1920 --height 1080 --fps 60 ^
  --format BGR8 --output RGBA8 --frames 300
```

### D3D12 camera log

```bat
out\build\default\samples\SingleCameraLogD3D12\Debug\SingleCameraLogD3D12.exe ^
  --device-index 0 --width 1920 --height 1080 --fps 60 ^
  --format BGR8 --output RGBA8 --frames 300
```

### D3D11 readback

```bat
out\build\default\samples\SingleCameraReadback\Debug\SingleCameraReadback.exe ^
  --device-index 0 --output RGBA8 --cpu-format BGR8 --out frame.ppm
```

### D3D12 readback

```bat
out\build\default\samples\SingleCameraReadbackD3D12\Debug\SingleCameraReadbackD3D12.exe ^
  --device-index 0 --output RGBA8 --cpu-format BGR8 --out frame_d3d12.ppm
```

Gray output の場合は PGM、RGB/BGR/RGBA output の場合は PPM に保存します。

## Minimal usage

### D3D11 capture

```cpp
#include <IC4Ext/IC4Ext.hpp>
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

int main()
{
    auto core = D3D11CoreLib::D3D11Core::CreateShared();

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config;
    config.streamRequest.width = 1920;
    config.streamRequest.height = 1080;
    config.streamRequest.fps = 60.0;
    config.streamRequest.requestedFormat = IC4Ext::CameraPixelFormat::BGR8;
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.shaderConfig.shaderDirectory = "shaders/d3d11";

    IC4Ext::D3D11CameraCapture cap;
    if (!cap.open(selector, config, core.get())) {
        return 1;
    }

    auto result = cap.read(IC4Ext::ReadMode::LatestFrame);
    if (result) {
        result.frame.ready.wait();
        // result.frame.texture / result.frame.srv を利用する。
    }
}
```

### D3D12 capture

```cpp
#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

int main()
{
    auto core = D3D12CoreLib::D3D12Core::CreateShared();
    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config;
    config.streamRequest.width = 1920;
    config.streamRequest.height = 1080;
    config.streamRequest.fps = 60.0;
    config.streamRequest.requestedFormat = IC4Ext::CameraPixelFormat::BGR8;
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.shaderConfig.shaderDirectory = "shaders/d3d12";

    IC4Ext::D3D12CameraCapture cap;
    if (!cap.open(selector, config, backend)) {
        return 1;
    }

    auto result = cap.read(IC4Ext::ReadMode::LatestFrame);
    if (result) {
        result.frame.ready.wait();
        // result.frame.texture / result.frame.srv を利用する。
    }
}
```

### Readback to CpuFrame

```cpp
IC4Ext::CpuFrame cpu;
IC4Ext::D3D12FrameReadback readback;
readback.initialize(backend);

if (readback.readback(gpuFrame, IC4Ext::CpuFrameFormat::BGR8, cpu)) {
    // cpu.data は tight packed BGR8。
}
```

## IC Capture 4 JSON settings

IC Capture 4 exported JSON can be passed through `CameraCaptureConfig::ic4StateJson.path`.

IC4Ext reads:

```txt
devices[deviceIndex].state
```

Example:

```cpp
IC4Ext::CameraCaptureConfig config;
config.ic4StateJson.path = "camera_state.json";
config.ic4StateJson.deviceIndex = 0;
config.ic4StateJson.strict = false;
config.streamRequest.offsetX = 0;
config.streamRequest.offsetY = 0;
```

When `--ic4-json` is specified in samples, `PixelFormat`, `Width`, `Height`, `AcquisitionFrameRate`, and other scalar properties are loaded from `devices[0].state`. Use `--force-format 1` to override the JSON pixel format with `--format`.

## Runtime setters

After opening a camera, common IC4 properties can be changed through typed setters:

```cpp
cap.setExposureAuto("Off");
cap.setExposureTime(2000.0);
cap.setGainAuto("Off");
cap.setGain(12.0);
cap.setGamma(1.0);
cap.setFrameRate(160.0);
cap.setOffset(0, 0);
cap.setRoi(1536, 1536, 0, 0);
cap.setIC4Property("ReverseX", false);
```

Some properties, especially `Width`, `Height`, `OffsetX`, `OffsetY`, and `PixelFormat`, may be locked while acquisition is running depending on camera model and IC4 state. In that case, the setter returns `false` and `lastError()` stores the IC4 error information.

## Documentation

詳細は `docs/README.md` と `docs/design/*.md` を参照してください。現在の実装状態と今後の残タスクは `docs/design/14_CurrentStatusAndRoadmap.md` に整理しています。

## Known limitations / TODO

- D3D12-D3D11 shared texture interop は未実装です。
- 10/12/16bit packed / unpacked pixel format は未実装です。
- chunk metadata は未実装です。現状は `device_frame_number` / `device_timestamp_ns` を `FrameTiming` に保持します。
- 高 fps 長時間運用、実カメラ readback 統合テスト、readback resource reuse 最適化は今後の確認事項です。

## License

Add a license file before public distribution.
