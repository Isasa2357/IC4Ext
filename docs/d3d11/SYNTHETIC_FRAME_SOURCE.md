# D3D11 SyntheticFrameSource

`IC4Ext::D3D11::SyntheticFrameSource`は、物理cameraへ接続せず、任意size・任意fpsのRGBA8 D3D11 Texture2Dを生成する`ReadOnlyFrameSource`である。

## 1. Purpose

再現する経路:

```text
GPU RGBA pattern generation
    -> D3D11 FramePool
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> FrameSyncThread
    -> ReadOnlyFrameSetQueue
    -> readback / HLSL / consumer
```

再現しないもの:

```text
IC4 QueueSink
USB transfer
camera exposure/readout
hardware trigger input
camera pixel-format negotiation
Bayer/BGR camera conversion
real device/chunk timestamp behavior
```

したがって、camera I/O以外のReadOnly pipeline、lifetime、synchronization、queue、consumer処理を実機なしで検証するために使う。

## 2. Configuration

```cpp
namespace Pipe = IC4Ext::D3D11;

Pipe::SyntheticFrameSourceConfig config;
config.width = 1536;
config.height = 1536;
config.fps = 160.0;
config.pattern = Pipe::SyntheticFramePattern::HashNoise;
config.seed = 100;
config.firstFrameNumber = 1;
config.deviceTimestampOriginNs = 1'000'000'000ull;
config.deviceTimestampOffsetNs = 0;
config.frameLimit = 0; // unlimited
config.initialFramePoolCapacity = 128;
config.maxFramePoolCapacity = 256;
```

pattern:

```text
HashNoise
Gradient
Checkerboard
FrameCounterBars
```

`HashNoise`はx、y、frame index、seedから決定的な値を生成する。同じ設定は再現可能であり、frame indexが進むと画像が変化する。

## 3. Initialization

```cpp
auto core = D3D11CoreLib::D3D11Core::CreateShared(coreConfig);
auto backend = IC4Ext::D3D11BackendContext::FromCore(core, true);

auto source = std::make_shared<Pipe::SyntheticFrameSource>();
if (!source->initialize(backend, config)) {
    auto error = source->lastError();
}
```

内部で次を作る。

```text
D3D11 compute pipeline
constant-buffer slots x 4
D3D11 fence timeline
RGBA8 FramePool
```

## 4. FPS pacing

最初のframeは直ちに生成し、以降は次でrate limitする。

```text
periodNs = round(1,000,000,000 / fps)
```

GPU/OS schedulingが遅れた場合、catch-up burstは行わない。次のdeadlineを実際の完了時刻以降へ移動する。

## 5. Timing metadata

```text
frameNumber = firstFrameNumber + frameIndex

deviceTimestampNs =
    deviceTimestampOriginNs
    + frameIndex * periodNs
    + deviceTimestampOffsetNs

hostReceivedTime = frame生成開始時のsteady_clock
```

2 sourceのoffset例:

```cpp
auto config0 = config;
config0.deviceTimestampOffsetNs = 0;

auto config1 = config;
config1.deviceTimestampOffsetNs = 500'000; // 500 us
```

これによりtimestamp toleranceを決定的に検証できる。

## 6. CameraCaptureThread connection

```cpp
auto source0 = std::make_shared<Pipe::SyntheticFrameSource>();
auto source1 = std::make_shared<Pipe::SyntheticFrameSource>();

source0->initialize(backend, config0);
source1->initialize(backend, config1);

Pipe::CameraCaptureThread camera0(0, source0);
Pipe::CameraCaptureThread camera1(1, source1);

camera0.setOutputQueue(syncIngress);
camera1.setOutputQueue(syncIngress);
```

以降のpipelineは実camera sourceの場合と同じである。

## 7. FramePool and lifetime

生成Textureはsource-owned FramePoolに属する。最後の`ReadOnlyFrame`参照が解放されるまでpool entryへ戻らない。

```cpp
auto stats = source->framePoolStats();
stats.capacity;
stats.available;
stats.published;
stats.exhaustionDrops;
```

遅いconsumer、深いqueue、多数のfan-out outputを再現するにはpool上限を小さくし、`exhaustionDrops`を観察できる。

## 8. Source statistics

```cpp
auto stats = source->stats();
stats.generatedFrames;
stats.readTimeouts;
stats.poolAcquireFailures;
stats.gpuGenerationFailures;
stats.lateFrames;
stats.lastFrameNumber;
stats.lastDeviceTimestampNs;
```

## 9. Integration test

```text
test_d3d11_synthetic_source_sync_integration
```

検証内容:

- 2 sourceを120 fpsで生成
- source間device timestamp offset 500 us
- CameraCaptureThread x 2
- central FrameSyncThread
- 24 synchronized sets
- RGBA8 Texture2DとSRV
- timestamp周期とoffset
- ReadOnlyFrame readback
- alpha 255
- queue/sync/pool drop 0
- 最終的にpool entryが全て返却される

## 10. Build and run

```bat
set "IC4EXT_OK=1"

cmake -S . -B out\build\v2_d3d11 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=OFF ^
  -DIC4EXT_BUILD_SAMPLES=OFF ^
  -DIC4EXT_BUILD_TESTS=ON

if errorlevel 1 set "IC4EXT_OK=0"

if "%IC4EXT_OK%"=="1" cmake --build out\build\v2_d3d11 ^
  --config Debug ^
  --target test_d3d11_synthetic_source_sync_integration ^
  --parallel

if errorlevel 1 set "IC4EXT_OK=0"

if "%IC4EXT_OK%"=="1" ctest --test-dir out\build\v2_d3d11 ^
  -C Debug ^
  --output-on-failure ^
  -R "test_d3d11_synthetic_source_sync_integration"

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] D3D11 synthetic test failed. CMD remains open.
if "%IC4EXT_OK%"=="1" echo [OK] D3D11 synthetic test passed.
```

物理cameraは不要だが、D3D11 device、Windows SDK、IC4 SDK headers/librariesは現在のlibrary buildに必要である。
