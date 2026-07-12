# IC4Ext 2.0.0 D3D11 ReadOnly Frame Pipeline

この文書は、D3D11 backendでD3D12 ReadOnly pipelineと同じconsumer-facing architectureを提供するための現行仕様を定義する。

## 1. Public API

```cpp
#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D11;
```

代表型:

```text
Pipe::D3D11BackendContext
Pipe::CameraCapture
Pipe::CameraCaptureThread
Pipe::ReadOnlyFrame
Pipe::ReadOnlyFrameSet
Pipe::FramePool
Pipe::FrameWriter
Pipe::PooledFrameConverter
Pipe::FrameSyncThread
Pipe::FrameSyncConfig
Pipe::FrameSyncOutputConfig
Pipe::ReadOnlyFrameLifetimeTracker
Pipe::ReadOnlyFrameSource
Pipe::SyntheticFrameSource
```

## 2. Architecture

```text
IC4 ImageBuffer
    -> D3D11 camera conversion
    -> producer-owned FramePool Texture2D
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> IndexedReadOnlyFrameQueue
    -> one central timestamp FrameSyncThread
    -> ReadOnlyFrameSetQueue x N
    -> GPU / CPU consumers
```

`CameraCaptureThread`と`FrameSyncThread`はoutputごとのGPU texture copyを行わない。fan-outでは同じimmutable storageへのshared handleだけを複製する。

## 3. D3D11とD3D12の共通契約

- 完成frameの所有者はproducer側FramePool。
- consumerへ公開するframeはReadOnly。
- 書き込みconsumerは自分専用のdestination resourceを用意する。
- producer-ready fenceをconsumerが尊重する。
- GPU consumerは処理完了まで入力frameを保持する。
- 1同期domainにつき中央`FrameSyncThread`を1つ使う。
- frame-number同期は使用せず、timestamp-nearestのみを使う。
- outputごとにrequired cameras、FPS、priority、enabledを設定できる。
- outputの登録、更新、queue差替え、削除を実行中に行える。

## 4. ReadOnlyFrame

```cpp
frame.texture();
frame.srv();
frame.dxgiFormat();
frame.readyToken();
frame.timing();
frame.format();
frame.chunkMetadata();
frame.waitReady(5000);
```

許可:

```text
SRV入力
CopyResource / CopySubresourceRegionのsource
metadata参照
別resourceへの入力
```

禁止:

```text
元TextureへのUAV書き込み
元Textureの上書き
他consumerと競合するbinding/state変更
```

D3D11にはD3D12の明示resource state transitionはないが、同じresourceを複数threadから書き換えないというReadOnly契約は同じである。

## 5. FramePool

概念状態:

```text
Available -> Writing -> Published -> Available
```

最後の`ReadOnlyFrame`参照が解放されるとentryがpoolへ戻る。pool stats:

```cpp
auto stats = pool.stats();
stats.capacity;
stats.available;
stats.writing;
stats.published;
stats.inFlight();
stats.availableRatio();
stats.inFlightRatio();
stats.exhaustionDrops;
```

## 6. PooledFrameConverter

D3D11 camera bytesを既存FramePool textureへ直接compute変換するproducer utilityである。

```text
CPU image bytes
    -> reusable typed input buffer slot
    -> compute shader
    -> FrameWriter UAV
    -> D3D11 fence signal
    -> ReadOnlyFrame publish
```

4 slotを持ち、input bufferとconstant bufferを再利用する。

```cpp
auto stats = converter.stats();
stats.conversions;
stats.inputBufferAllocations;
stats.inputBufferReuses;
stats.cachedInputBufferCount;
stats.cachedInputBufferBytes;
```

## 7. Immediate-context synchronization

`ID3D11Multithread`は個々のcontext callをthread-safeにするが、bind/update/dispatch/signalという複数callのtransactionをatomicにはしない。

`D3D11BackendContext`は同じimmediate contextに対するshared sequence mutexを解決する。producer/consumer implementationは、binding transactionが別threadとinterleaveしないようこのmutexを使う。

## 8. CameraCapture

新しいconsumer-facing APIは`IC4Ext::D3D11::CameraCapture`である。

現段階の実カメラ経路は、検証済みのlegacy IC4 capture/converterを内部producerとして利用し、完成frameをReadOnly FramePoolへ1回コピーして公開する。したがって、outputごとのcopy fan-outは廃止済みだが、D3D12のようなIC4 bytesからFramePoolへの完全direct conversionは次の最適化項目である。

この差はconsumer API、FramePool lifetime、central syncには影響しない。

## 9. CameraCaptureThread

```cpp
Pipe::CameraCaptureThread camera(
    cameraId,
    selector,
    config,
    backend,
    captureOptions,
    threadOptions);

camera.setOutputQueue(syncIngress);
camera.start();
```

外部source injection:

```cpp
auto source = std::make_shared<Pipe::SyntheticFrameSource>();
Pipe::CameraCaptureThread camera(cameraId, source, threadOptions);
```

## 10. Central timestamp synchronization

```cpp
Pipe::FrameSyncConfig config;
config.cameraIds = {0, 1};
config.timestampSource = Pipe::FrameSyncTimestampSource::Device;
config.maxTimestampDiffNs = 1'000'000;
config.maxBufferedFramesPerCamera = 16;
config.groupTimeout = std::chrono::milliseconds(100);
```

output registration:

```cpp
Pipe::FrameSyncOutputConfig outputConfig;
outputConfig.requiredCameras = {0, 1};
outputConfig.frameRate = Pipe::FrameRateLimit::Maximum();
outputConfig.priority = 100;
outputConfig.enabled = true;

auto outputId = sync.registerOutput(queue, outputConfig);
```

runtime operations:

```cpp
sync.updateOutput(outputId, updated);
sync.replaceOutputQueue(outputId, newQueue);
sync.unregisterOutput(outputId);
```

## 11. Queue policy

Latest display:

```text
capacity = 1
policy   = DropOldest
consumer = waitPopLatestFor
```

All-frame processing:

```text
capacity = bounded
policy   = RejectNew
consumer = FIFO
```

all-frameでqueue dropが0なら、そのconsumerが入力rateへ追従できたことを意味する。

## 12. Readback

```cpp
IC4Ext::D3D11FrameReadback readback;
readback.initialize(core.get());

IC4Ext::CpuFrame cpu;
readback.readback(
    readOnlyFrame,
    IC4Ext::CpuFrameFormat::BGR8,
    cpu,
    5000);
```

ReadOnly overloadはproducer fenceを待ってからstaging textureへcopyする。複数CPU pipelineを並列実行する場合は、pipelineごとにreadback instanceを分ける。

## 13. Synthetic source

`SyntheticFrameSource`は実カメラなしで任意size/fpsのRGBA8 GPU frameを生成する。詳細は`SYNTHETIC_FRAME_SOURCE.md`を参照する。

## 14. Tests

```text
test_d3d11_readonly_pipeline
test_d3d11_pooled_converter_device
test_d3d11_synthetic_source_sync_integration
```

最初はD3D11-onlyでbuildする。

```bat
cmake -S . -B out\build\v2_d3d11 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=OFF ^
  -DIC4EXT_BUILD_SAMPLES=OFF ^
  -DIC4EXT_BUILD_TESTS=ON
```

## 15. Remaining D3D11 work

1. 実カメラ経路をIC4 bytesからFramePoolへ完全direct conversion化する。
2. D3D11 ReadOnly実カメラsampleを追加する。
3. D3D11 multi-pipeline stress sampleを追加する。
4. synthetic長時間・runtime output update試験を追加する。
5. 実機camera/trigger/USB経路を検証する。
