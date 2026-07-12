# D3D12 Synthetic Frame Source

`IC4Ext::D3D12::SyntheticFrameSource`は、物理カメラやIC4 QueueSinkを使わずに、通常の`ReadOnlyFrame`を任意サイズ・任意fpsで生成するcamera-free sourceである。

目的は、次のD3D12 ReadOnly pipelineを実機なしで再現することにある。

```text
SyntheticFrameSource x N
    -> CameraCaptureThread x N
    -> IndexedReadOnlyFrameQueue
    -> FrameSyncThread
    -> ReadOnlyFrameSetQueue x M
    -> GPU / readback / CPU consumers
```

## 1. Scope

再現できるもの:

```text
FramePool acquire / publish / release
D3D12 command slot reuse
producer-ready fence
ReadOnlyFrame lifetime
CameraCaptureThread
central timestamp-nearest FrameSyncThread
requiredCameras / FPS / priority / runtime output
latest / all-frame queue behavior
GPU consumer
ReadOnly readback
長時間resource保持とpool exhaustion
```

再現しないもの:

```text
IC4 SDK / QueueSink callback
USB3Vision transfer
実カメラ露光
Line1外部trigger
camera property negotiation
Bayer/BGR camera input conversion
実camera chunk metadata
実camera device timestamp clock domain
```

したがって、このsourceは実機試験の代替ではなく、IC4入力以降のlibrary pipelineを検証するためのproducerである。

## 2. Public API

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

namespace Pipe = IC4Ext::D3D12;

Pipe::SyntheticFrameSourceConfig config;
config.width = 1536;
config.height = 1536;
config.fps = 160.0;
config.pattern = Pipe::SyntheticFramePattern::HashNoise;
config.seed = 1234;
config.deviceTimestampOffsetNs = 500'000;
config.initialFramePoolCapacity = 128;
config.maxFramePoolCapacity = 256;

auto source = std::make_shared<Pipe::SyntheticFrameSource>();
if (!source->initialize(backend, config)) {
    const auto error = source->lastError();
}

Pipe::CameraCaptureThread cameraThread(0, source);
cameraThread.setOutputQueue(syncIngress);
cameraThread.start();
```

`SyntheticFrameSource`は`ReadOnlyFrameSource`を実装するため、既存の`CameraCaptureThread`外部source constructorへそのまま渡せる。

## 3. GPU generation path

```text
FramePool::acquire()
    -> RGBA8 Texture2D UAV
    -> dedicated Direct Queue
    -> hash / gradient / checker / frame-counter HLSL
    -> UAV barrier
    -> GENERIC_READ
    -> queue signal
    -> FrameWriter::publish()
    -> ReadOnlyFrame
```

CPU側でRGBA画像を生成してuploadするのではなく、GPU compute shaderが直接FramePool textureを埋める。

各sourceは以下を専有する。

```text
D3D12 Direct Queue
producer fence
4 command contexts
4 shader-visible descriptor allocators
compute pipeline
FramePool
```

## 4. Patterns

```cpp
SyntheticFramePattern::HashNoise
SyntheticFramePattern::Gradient
SyntheticFramePattern::Checkerboard
SyntheticFramePattern::FrameCounterBars
```

`HashNoise`は`x`、`y`、frame index、64-bit seedから決定的な32-bit hashを生成する。実行ごとに再現でき、frame間およびsource間で異なるRGBA画像になる。

## 5. Pacing

`read()`は`fps`から計算したperiodに従って次frame時刻まで待つ。

```text
periodNs = round(1,000,000,000 / fps)
```

frame生成が予定時刻より遅れた場合、catch-up burstは発生させない。次のdeadlineを実際の完了時刻以降へ進めるため、sourceは設定fpsを上限として動作する。

`CameraReadOptions::timeoutMs`より次frame予定時刻が遠い場合、`ErrorCode::Timeout`を返す。`CameraCaptureThread`では通常のread timeoutとして扱われる。

## 6. Timing metadata

各frameは次を持つ。

```text
frameNumber       = firstFrameNumber + frameIndex
deviceTimestampNs = deviceTimestampOriginNs
                    + frameIndex * periodNs
                    + deviceTimestampOffsetNs
hostReceivedTime  = synthetic frameを生成開始したsteady_clock時刻
```

`deviceTimestampOffsetNs`により、複数source間の既知のtimestamp差を作れる。

例:

```text
source0 offset =       0 ns
source1 offset = 500,000 ns
sync tolerance = 500,001 ns
```

これは`FrameSyncTimestampSource::Device`の決定的な同期testに使う。

## 7. Frame metadata

生成resourceは常に次である。

```text
DXGI format       R8G8B8A8_UNORM
GpuFrameFormat    RGBA8
published state   D3D12_RESOURCE_STATE_GENERIC_READ
SRV               present
UAV               producer側のみ
```

synthetic sourceにはcamera input pixel formatが存在しないため、`FrameFormatMetadata::requestedFormat`と`actualInputFormat`は4-byte inputに最も近い`BGRa8`として記録する。GPU outputは`RGBA8`である。

## 8. Pool and lifetime

通常のcamera producerと同じ`FramePool`を使う。最後の`ReadOnlyFrame`参照が解放されるまでentryは再利用されない。

設定:

```cpp
config.initialFramePoolCapacity = 16;
config.maxFramePoolCapacity = 64;
config.framePoolExhaustionPolicy =
    Pipe::FramePoolExhaustionPolicy::DropNewest;
config.framePoolWaitTimeout = std::chrono::milliseconds(5);
```

統計:

```cpp
const auto stats = source->stats();

stats.generatedFrames;
stats.readTimeouts;
stats.poolAcquireFailures;
stats.gpuGenerationFailures;
stats.lateFrames;
stats.lastFrameNumber;
stats.lastDeviceTimestampNs;
stats.framePool;
```

## 9. Finite source

```cpp
config.frameLimit = 1000;
```

`frameLimit == 0`は無制限である。有限値を指定すると、指定数をpublishした後の`read()`はTimeoutを返す。決定的なintegration testやresource-release確認に使う。

## 10. Integration test

```text
test_d3d12_synthetic_source_sync_integration
```

構成:

```text
SyntheticFrameSource 0: 96x64, 120 fps, seed A, offset 0
SyntheticFrameSource 1: 96x64, 120 fps, seed B, offset 500 us
    -> CameraCaptureThread x2
    -> FrameSyncThread(Device timestamp, tolerance 500001 ns)
    -> 24 synchronized sets
```

検証内容:

```text
任意width/height/fps設定
GPU RGBA8 resource生成
producer fence
SRVとresource description
frame number連続性
device timestamp period
timestamp offset
2 source同期set数
RGBA readback
alpha=255
source seed差によるimage checksum差
frame index差によるimage checksum差
queue drop 0
pool exhaustion 0
全ReadOnly参照解放後のpool返却
```

D3D12 deviceを作成できない環境ではreturn code 77でCTest skipになる。物理cameraは不要である。

## 11. Build and run

```bat
cmake --build out\build\v2_d3d12 ^
  --config Debug ^
  --target test_d3d12_readonly_pipeline test_d3d12_synthetic_source_sync_integration ^
  --parallel

ctest --test-dir out\build\v2_d3d12 ^
  -C Debug ^
  --output-on-failure ^
  -R "test_d3d12_(readonly_pipeline|synthetic_source_sync_integration)"
```
