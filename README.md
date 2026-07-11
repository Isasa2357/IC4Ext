# IC4Ext

IC4Ext は、The Imaging Source **IC Imaging Control 4 SDK** で取得したカメラフレームを、Direct3D 11 / Direct3D 12 の GPU texture として扱うための C++17 ライブラリです。

IC4 の `QueueSink` から受け取った CPU 側 image buffer を D3D11 / D3D12 texture へ変換し、同期用 metadata、chunk metadata、非同期 capture thread、frame sync、DummyCameraCapture、IC Capture 4 から export した JSON 設定、GPU frame の CPU readback を提供します。

このパッケージの現在位置は **D3DHelper v1.13.0 対応版**です。D3D12 backend は raw D3D12 object だけを渡す初期化ではなく、`D3D12CoreLib::D3D12Core` から作った `D3D12BackendContext` を使います。D3D11 backend も、可能な範囲で D3D11Helper の resource / view / compute pipeline / binding / fence / transfer / processing helper に寄せています。

## 現在の実装状態

| 項目 | 状態 |
|---|---|
| D3D11 camera capture | 実装済み |
| D3D12 camera capture | 実装済み。D3D12Helper 統合済み |
| IC4 JSON state 読み込み | 実装済み。`devices[n].state` を適用 |
| Runtime setters | 実装済み。露光、gain、gamma、fps、offset、ROI、任意 IC4 property |
| DummyCameraCapture | D3D11 / D3D12 ともに実装済み |
| CameraCaptureThread output queue | 実行中の登録・削除、キューごとのGPUリサイズに対応 |
| FrameSyncThread | D3D11 / D3D12 ともに実装済み |
| CpuFrame / readback | D3D11 / D3D12 ともに実装済み。readback resource reuse 対応 |
| chunk metadata | 実装済み。取得できる chunk のみ `has*` flag 付きで保持 |
| D3D12-D3D11 interop | 未実装 |
| 10/12/16bit pixel format | 未実装。必要になったら追加予定 |

## Features

- IC4 camera frame を D3D11 / D3D12 GPU texture として取得
- device selection: `serial -> uniqueName -> deviceIndex -> first device`
- `CameraStreamRequest::requestedFormat` を IC4 device の `PixelFormat` property として設定
- `ReadMode::LatestFrame` / `ReadMode::NextFrame` を選択可能
- `FrameTiming` として `frameNumber` / `deviceTimestampNs` / `hostReceivedTime` を保持
- `FrameChunkMetadata` として `ChunkBlockId` / `ChunkExposureTime` / `ChunkGain` / IMX174 / MultiFrameSet 系 chunk を保持
- D3D11.4 fence (`ID3D11Fence`) / D3D12 fence による GPU ready token
- `.hlsl` runtime compile と `.cso` load の両対応
- IC Capture 4 公式ソフトから export した JSON (`devices[n].state`) を `nlohmann/json` で読み込み
- JSON に含まれない `OffsetX` / `OffsetY` を明示指定可能
- `ExposureTime` / `Gain` / `Gamma` / `AcquisitionFrameRate` / `ROI` などを open 後に setter で変更可能
- CameraCaptureThread 実行中の output queue 登録・削除
- output queue ごとの passthrough / Point resize / Linear resize
- `D3D11DummyCameraCaptureGenerator` / `D3D12DummyCameraCaptureGenerator` による、1 台の実カメラからの擬似複数カメラ生成
- GPU frame を `CpuFrame` として readback し、`Gray8` / `RGBA8` / `RGB8` / `BGR8` に変換可能
- `D3D11FrameReadback` は staging texture cache、`D3D12FrameReadback` は readback buffer cache を持つ

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

output queue ごとのGPUリサイズは、現在 `RGBA8` / `DXGI_FORMAT_R8G8B8A8_UNORM` を対象とします。リサイズを指定しない passthrough queue は従来どおり元のformatとサイズを受け取ります。

### CPU readback formats

`D3D11CameraFrame` / `D3D12CameraFrame` は readback により共通の `CpuFrame` に変換できます。`FrameChunkMetadata` も GPU frame から `CpuFrame` へ引き継がれます。

```txt
Gray8
RGBA8
RGB8
BGR8
```

`CpuFrame` は常に tight packed です。

`D3D11FrameReadback::cacheStats()` / `D3D12FrameReadback::cacheStats()` で readback cache の hit / miss / rebuild 数を確認できます。`resetCache()` で保持している readback resource を解放し、統計を初期化します。

### Chunk metadata

IC4 chunk data が有効な場合、`FrameChunkMetadata` に以下を保持します。取得できなかった項目は対応する `has*` flag が `false` のままです。

```txt
ChunkBlockId
ChunkExposureTime
ChunkGain
ChunkIMX174FrameId
ChunkIMX174FrameSet
ChunkMultiFrameSetId
ChunkMultiFrameSetFrameId
```

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
D3D11Helper v1.13.0
D3D12Helper v1.13.0
ThreadKit
nlohmann/json single header
```

IC4Ext 本体は OpenCV などには依存しません。output queue resize は D3DHelper v1.13.0 の非所有 resource-view Processing API を使用します。

外部依存をローカル checkout から使う場合は、以下を指定できます。

```bat
-DD3D11HELPER_ROOT="C:/Path/To/D3D11Helper" ^
-DD3D12HELPER_ROOT="C:/Path/To/D3D12Helper" ^
-DTHREADKIT_ROOT="C:/Path/To/ThreadKit"
```

## Build

### CMD: existing clone

以下は、既に clone 済みで、現在の作業ディレクトリが IC4Ext の root、つまり `CMakeLists.txt` がある場所である前提です。`exit /b` は入れていません。

```bat
git pull

set "IC4_SDK_ROOT="
set "IC4EXT_DXC_RUNTIME_DIR=%CD%\packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\bin\x64"
set "PATH=%IC4PATH%\bin\x64;%IC4EXT_DXC_RUNTIME_DIR%;%PATH%"

nuget install Microsoft.Direct3D.DXC -Version 1.9.2602.24 -OutputDirectory packages

rmdir /s /q out\build\default 2>nul

cmake -S . -B out\build\default -G "Visual Studio 17 2022" -A x64 -DIC4EXT_BUILD_SAMPLES:BOOL=ON -DIC4EXT_BUILD_TESTS:BOOL=ON -DIC4EXT_ENABLE_D3D11:BOOL=ON -DIC4EXT_ENABLE_D3D12:BOOL=ON "-DIC4EXT_DXC_RUNTIME_DIR:PATH=%IC4EXT_DXC_RUNTIME_DIR%" "-DD3D11HELPER_DXC_RUNTIME_DIR:PATH=%IC4EXT_DXC_RUNTIME_DIR%" "-DD3D12HELPER_DXC_RUNTIME_DIR:PATH=%IC4EXT_DXC_RUNTIME_DIR%" -DIC4EXT_D3D11HELPER_GIT_TAG:STRING=v1.13.0 -DIC4EXT_D3D12HELPER_GIT_TAG:STRING=v1.13.0 -DIC4EXT_FETCH_DEPENDENCIES:BOOL=ON -DIC4EXT_FETCH_D3D11HELPER:BOOL=ON -DIC4EXT_FETCH_D3D12HELPER:BOOL=ON -DIC4EXT_FETCH_THREADKIT:BOOL=ON -DIC4EXT_FETCH_NLOHMANN_JSON:BOOL=ON

cmake --build out\build\default --config Debug --parallel
```

### Test labels

テストは CTest label で3分類しています。

```txt
no_camera   : カメラなしで実行可能
camera1     : IC4 camera 1台が必要
camera2plus : IC4 camera 2台以上が必要。FrameSyncThread 向け
```

```bat
ctest --test-dir out\build\default -C Debug -L no_camera --output-on-failure

set "IC4EXT_TEST_CAMERA_COOLDOWN_MS=5000"
ctest --test-dir out\build\default -C Debug -L camera1 --output-on-failure
ctest --test-dir out\build\default -C Debug -L camera2plus --output-on-failure
```

カメラの既定フォーマットで open できない場合は、JSON または format 指定を使います。

```bat
set "IC4EXT_TEST_IC4_JSON=C:\path\to\camera_state.json"
set "IC4EXT_TEST_FORMAT=BGR8"
```

2台テストで個別 JSON を使う場合:

```bat
set "IC4EXT_TEST_IC4_JSON_0=C:\path\to\camera0.json"
set "IC4EXT_TEST_IC4_JSON_1=C:\path\to\camera1.json"
```

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
        if (result.frame.chunkMetadata.hasExposureTime) {
            const double exposureUs = result.frame.chunkMetadata.exposureTimeUs;
            (void)exposureUs;
        }
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
        if (result.frame.chunkMetadata.hasGain) {
            const double gain = result.frame.chunkMetadata.gain;
            (void)gain;
        }
    }
}
```

### CameraCaptureThread output queue resize

既存の2引数登録は元サイズを維持します。3引数登録では、キューごとの出力サイズとfilterを指定します。

```cpp
auto originalQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(queueOptions);
auto previewQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(queueOptions);

auto cameraThread = std::make_unique<IC4Ext::D3D12CameraCaptureThread>(
    std::move(capture), backend, threadOptions);

cameraThread->addOutputQueue(0, originalQueue);
cameraThread->addOutputQueue(
    0,
    previewQueue,
    IC4Ext::CameraOutputResizeOptions{
        640,
        360,
        IC4Ext::CameraOutputResizeFilter::Linear,
    });
```

`CameraOutputResizeOptions{}` または `{0, 0, filter}` は passthrough です。幅と高さの片方だけを0にした指定は無効です。詳細は `docs/design/17_OutputQueueResize.md` を参照してください。

### Readback to CpuFrame

```cpp
IC4Ext::CpuFrame cpu;
IC4Ext::D3D12FrameReadback readback;
readback.initialize(backend);

if (readback.readback(gpuFrame, IC4Ext::CpuFrameFormat::BGR8, cpu)) {
    // cpu.data は tight packed BGR8。
    // cpu.chunkMetadata は gpuFrame.chunkMetadata から引き継がれる。
}

const auto stats = readback.cacheStats();
// stats.cacheHits / cacheMisses / resourceRebuilds で readback resource reuse を確認できる。
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

詳細は `docs/README.md` と `docs/design/*.md` を参照してください。output queue resize は `docs/design/17_OutputQueueResize.md` に整理しています。

## Known limitations / TODO

- output queue GPU resize は現在 RGBA8 texture のみ対応しています。
- リサイズは指定幅・高さへの直接stretchで、aspect ratioの自動維持は行いません。
- D3D12-D3D11 shared texture interop は未実装です。
- 10/12/16bit packed / unpacked pixel format は未実装です。
- 高 fps 長時間運用と実カメラ readback 統合テストは今後の確認事項です。

## License

Add a license file before public distribution.
