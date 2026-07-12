# IC4Ext

IC4Extは、The Imaging Source **IC Imaging Control 4 SDK**で取得したcamera frameを、Direct3D 11 / Direct3D 12のGPU resourceとして扱うC++17 libraryである。

現在のproject versionは**2.0.0**である。

D3D12側の正式経路は、1つの完成Texture2Dを複数consumerへ共有する**ReadOnly frame pipeline**である。

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D12;
```

## 1. Architecture summary

### D3D12

```text
IC4 ImageBuffer
    -> CameraCapture
    -> UploadRing + reusable input buffer
    -> compute conversion
    -> CameraCapture-owned FramePool
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> central FrameSyncThread
    -> ReadOnlyFrameSetQueue x N
    -> GPU / CPU / recording consumers
```

`CameraCaptureThread`と`FrameSyncThread`はoutputごとのGPU texture copyを行わない。fan-outでは同じReadOnly resourceへの共有handleを渡す。

### D3D11

D3D11側は既存のcapture/thread/sync/readback APIを維持している。D3D12 2.0.0と完全に同じpublic architectureではない。

## 2. Compatibility policy

D3D12 v1 physical-copy fan-out APIとのsource compatibilityは保証しない。

旧D3D12のcamera capture/thread/frame sync/frame copier public APIは正式経路から外した。新しいD3D12コードは`IC4Ext::D3D12`を使う。

一部の実装本体は物理移動途中で`include/IC4Ext/V2` / `src/V2`に残っているが、public APIとCMake build entryではない。

## 3. Current implementation status

| 項目 | 状態 |
|---|---|
| D3D11 camera capture | 実装済み |
| D3D12 ReadOnly camera capture | 実装済み |
| D3D12 CameraCapture-owned FramePool | 実装済み |
| D3D12 pooled input buffer reuse | 実装済み |
| D3D12 central timestamp sync | 実装済み |
| Runtime output register/update/remove | 実装済み |
| Per-output required cameras / FPS / priority | 実装済み |
| D3D12 consumer lifetime tracker | 実装済み |
| D3D12 ReadOnly readback | 実装済み |
| IC4 JSON state | 実装済み |
| Runtime camera property setters | 実装済み |
| Chunk metadata | 実装済み |
| Camera performance snapshot | 実装済み |
| D3D12 hardware video encoder integration | 未実装 |
| D3D12-D3D11 interop | 未実装 |
| 10/12/16bit and packed formats | 未実装 |
| 2-camera 160 fps long-run acceptance | 検証中 |

## 4. Features

- IC4 camera frameをD3D11/D3D12 GPU resourceへ変換。
- device selection: `serial -> uniqueName -> deviceIndex -> first device`。
- IC Capture 4からexportしたJSON `devices[n].state`を適用。
- `ReadMode::LatestFrame` / `ReadMode::NextFrame`。
- `FrameTiming`: frame number、device timestamp、host received time。
- `FrameChunkMetadata`: block id、exposure、gain、IMX174、MultiFrameSet fields。
- hardware/software trigger設定helper。
- exposure、gain、gamma、fps、ROI、offset、PixelFormat、任意property setter。
- GPU ready fence token。
- D3D12 FramePoolとReadOnly共有fan-out。
- timestamp-nearest中央同期。
- outputごとのrequired cameras、FPS、priority、enabled。
- outputの実行中追加、更新、queue差替え、削除。
- consumer GPU completionまでのlifetime tracking。
- GPU frameからtight-packed `CpuFrame`へのreadback。
- D3D11 staging texture cache / D3D12 readback buffer cache。
- camera-free D3D12 `ReadOnlyFrameSource` injection。
- D3D12 10-pipeline stress sample。

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
set "IC4_SDK_ROOT=C:\Users\MiyafujiLab2\AppData\Local\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
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

IC4Ext library本体はOpenCVに依存しない。`MultiPipelineStressD3D12`など一部sampleだけがOpenCVを要求する。

## 8. DXC runtime

`dxcompiler.dll`と`dxil.dll`はCMakeが既存package/global NuGet cacheから探し、必要なら`Microsoft.Direct3D.DXC`をNuGetから取得する。

```text
-DIC4EXT_FETCH_DXC_RUNTIME=ON
```

sample/test targetでは両DLLをexeと同じdirectoryへcopyする。

version固定:

```text
-DIC4EXT_DXC_NUGET_VERSION=1.9.2602.24
```

## 9. Build: D3D12

失敗してもCMDを閉じない例:

```bat
set "IC4_SDK_ROOT=C:\Users\MiyafujiLab2\AppData\Local\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
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

if "%IC4EXT_OK%"=="1" cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --parallel

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] build failed. CMD remains open.
```

## 10. Build: OpenCV stress sample

OpenCV root例:

```text
C:\personal\iwatake\library\opencv
```

```bat
set "OPENCV_ROOT=C:\personal\iwatake\library\opencv"
set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc17\lib"
set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc17\bin"
set "PATH=%OPENCV_BIN%;%PATH%"

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

## 11. Minimal D3D12 capture

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <IC4Ext/D3D12/D3D12BackendContext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

int main()
{
    namespace Pipe = IC4Ext::D3D12;

    auto core = D3D12CoreLib::D3D12Core::CreateShared();
    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = 0;

    IC4Ext::CameraCaptureConfig config;
    config.streamRequest.requestedFormat = IC4Ext::CameraPixelFormat::BGR8;
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;

    Pipe::CameraCaptureOptions options;
    options.initialFramePoolCapacity = 16;
    options.maxFramePoolCapacity = 64;

    Pipe::CameraCapture capture;
    if (!capture.open(selector, config, backend, options)) {
        return 1;
    }

    auto result = capture.read(IC4Ext::CameraReadOptions{
        IC4Ext::ReadMode::NextFrame,
        1000});

    if (!result) {
        return 2;
    }

    const Pipe::ReadOnlyFrame& frame = result.frame;
    frame.readyToken().wait(5000);

    ID3D12Resource* texture = frame.resource();
    const auto timing = frame.timing();
    (void)texture;
    (void)timing;
}
```

## 12. Minimal central synchronization

```cpp
namespace Pipe = IC4Ext::D3D12;

ThreadKit::Queues::QueueOptions ingressOptions;
ingressOptions.maxSize = 256;
ingressOptions.overflowPolicy =
    ThreadKit::Queues::QueueOverflowPolicy::DropOldest;

auto ingress =
    std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(ingressOptions);

Pipe::FrameSyncConfig syncConfig;
syncConfig.cameraIds = {0, 1};
syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::HostReceived;
syncConfig.maxTimestampDiffNs = 4'000'000;
syncConfig.maxBufferedFramesPerCamera = 16;
syncConfig.groupTimeout = std::chrono::milliseconds(100);

Pipe::FrameSyncThread sync(ingress, syncConfig);

ThreadKit::Queues::QueueOptions outputOptions;
outputOptions.maxSize = 1;
outputOptions.overflowPolicy =
    ThreadKit::Queues::QueueOverflowPolicy::DropOldest;

auto displayQueue =
    std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);

Pipe::FrameSyncOutputConfig displayConfig;
displayConfig.requiredCameras = {0, 1};
displayConfig.frameRate = Pipe::FrameRateLimit::Maximum();
displayConfig.priority = 100;

const auto outputId = sync.registerOutput(displayQueue, displayConfig);

Pipe::CameraCaptureThread camera0(
    0, selector0, cameraConfig0, backend, captureOptions, threadOptions);
Pipe::CameraCaptureThread camera1(
    1, selector1, cameraConfig1, backend, captureOptions, threadOptions);

camera0.setOutputQueue(ingress);
camera1.setOutputQueue(ingress);

sync.start();
camera0.start();
camera1.start();
```

D3D12 synchronizationはtimestamp-nearestのみである。frame-number matchingは使わない。

## 13. Runtime output update

```cpp
auto config = sync.outputConfig(outputId);
if (config) {
    config->requiredCameras = {0};
    config->frameRate = Pipe::FrameRateLimit::Fixed(30.0);
    config->priority = 200;
    sync.updateOutput(outputId, *config);
}

sync.replaceOutputQueue(outputId, newQueue);
sync.unregisterOutput(outputId);
```

変更は次の完全同期setから反映する。既にdispatch snapshotへ入ったsetが旧queueへ1回届く可能性がある。

## 14. GPU consumer lifetime

```cpp
Pipe::ReadOnlyFrameLifetimeTracker tracker;

const Pipe::ReadOnlyFrame* input = frameSet.find(0);
Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, *input);

auto consumerDone = SubmitProcessingAndSignal();
tracker.retainUntil(*input, consumerDone);
tracker.collectCompleted();
```

producer-ready tokenだけではconsumer GPU完了までresourceを保持できない。

## 15. Readback

```cpp
IC4Ext::D3D12FrameReadback readback;
readback.initialize(consumerBackend);

IC4Ext::CpuFrame cpu;
if (readback.readback(
        readOnlyFrame,
        IC4Ext::CpuFrameFormat::BGR8,
        cpu,
        5000)) {
    // cpu.data is tight-packed BGR8.
}
```

並列CPU consumerごとに専用D3D12 queueと専用readback instanceを持つことを推奨する。

## 16. Samples

```text
IC4DeviceDiagnostics
SingleCameraReadOnlyReadbackD3D12
MultiCameraReadOnlySyncD3D12
MultiPipelineStressD3D12
```

10-pipeline stress sampleは、各CPU/display/video consumerが独立readbackを行う。

## 17. Tests

```text
test_core
test_cpu_frame
test_backend_config
test_chunk_metadata
test_d3d12_core
test_d3d12_shader_reference
test_d3d12_readonly_pipeline
test_d3d12_pooled_converter_device
test_d3d12_dummy_capture_sync_integration
test_d3d12_shader_compile
```

```bat
ctest --test-dir out\build\v2_d3d12 ^
  -C Release ^
  -L no_camera ^
  --output-on-failure
```

## 18. Preliminary validation

10-pipeline実機試験では、capture poolを16/64から128/256へ増やすことで、pool exhaustion、capture timeout、sync dropが解消した。

large pool実行の予備値:

```text
sync rate          約53.36 sets/s
sync drop          0
camera read        3201 / 3202 in 60 s
pool exhaustion    0 / 0
HLSL Sobel         input rateへ追従
OpenCV recording   約7-17 fps
```

この結果は160 fps acceptanceではない。外部trigger周波数、camera setting、ROI、exposure、USB帯域を切り分ける必要がある。

## 19. Documentation

推奨入口:

```text
docs/README.md
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
samples/MultiPipelineStressD3D12/README.md
docs/design/14_CurrentStatusAndRoadmap.md
```

## 20. Known limitations

- 一部D3D12実装本体の物理移動が未完了。
- D3D12 hardware encoder未統合。
- D3D12-D3D11 interop未実装。
- 10/12/16bit、packed Bayer、YUV/NV12等は未実装。
- pair timestamp delta percentile統計は未実装。
- device removal / DRED failure injection testは未実装。
- 2台160 fps 10分以上のacceptanceは未完了。

## License

Public distribution前にlicense fileを追加すること。
