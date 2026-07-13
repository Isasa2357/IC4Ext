# IC4Ext 2.0.0 D3D11 ReadOnly Frame Pipeline

この文書は、D3D11 backendでD3D12 ReadOnly pipelineと同じconsumer-facing architectureを提供する現行仕様を定義する。

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
    -> reusable D3D11 input-buffer slot
    -> compute conversion
    -> CameraCapture-owned FramePool Texture2D
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> IndexedReadOnlyFrameQueue
    -> one central timestamp FrameSyncThread
    -> ReadOnlyFrameSetQueue x N
    -> GPU / CPU consumers
```

実camera hot pathは、IC4 CPU bytesを最終FramePool Textureへ直接compute変換する。中間完成Textureと追加`CopyResource`は生成しない。

`CameraCaptureThread`と`FrameSyncThread`もoutputごとのGPU texture copyを行わない。fan-outでは同じimmutable storageへのshared handleだけを複製する。

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
他consumerと競合するmutable操作
```

D3D11にはD3D12の明示resource-state transitionはないが、同じresourceを複数threadから書き換えないReadOnly契約は同じである。

## 5. FramePool

概念状態:

```text
Available -> Writing -> Published -> Available
```

最後の`ReadOnlyFrame`参照が解放されるとentryがpoolへ戻る。

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

format/size変更時はfuture acquisition用に新しいpoolを作る。既存ReadOnlyFrameは古いpool stateをshared ownershipするため有効なままである。

## 6. PooledFrameConverter

D3D11 camera bytesを既存FramePool textureへ直接compute変換するproducer utilityである。

```text
CPU image bytes
    -> reusable input-buffer slot
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

converter.waitIdle(5000);
```

`waitIdle()`はcapture/source shutdown時にconverter-owned bufferを破棄する前に全slot完了を待つ。

## 7. Immediate-context synchronization

`ID3D11Multithread`は個々のcontext callをthread-safeにするが、次の複数call transactionをatomicにはしない。

```text
UpdateSubresource
bind SRV/UAV/CB
Dispatch or CopyResource
binding restore/unbind
Signal
```

`D3D11BackendContext`は同じ`ID3D11DeviceContext*`に対するshared recursive mutexを解決する。FrameWriterは`acquire()`から`publish()`/`cancel()`後のbinding復元までlockを保持する。converter、synthetic source、HLSL consumer、readbackは同じmutexを使用する。

この方式は安全性を優先する。複数consumerは独立buffer/cacheを持てるが、同じimmediate context上のmulti-call transactionは直列化される。

## 8. CameraCapture

新しいconsumer-facing APIは`IC4Ext::D3D11::CameraCapture`である。

```cpp
Pipe::CameraCapture capture;
capture.open(selector, config, backend, captureOptions);
auto result = capture.read({IC4Ext::ReadMode::NextFrame, 1000});
```

`CameraCapture`自身がIC4 QueueSink、PooledFrameConverter、FramePoolを所有する。旧D3D11 physical-copy fan-out APIは正式経路ではない。

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

Threadは1つの中央ingressへReadOnlyFrameをmoveするだけで、consumerごとのcopyは行わない。

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

ReadOnly overloadはproducer fenceを待ってからstaging textureへcopyする。複数CPU pipelineでは、pipelineごとにreadback instance、staging cache、CpuFrame、cv::Matを分ける。immediate contextは共有されるためcopy/map transactionは共通mutexで直列化する。

## 13. Synthetic source

`SyntheticFrameSource`は実cameraなしで任意size/fps/timestamp offsetのRGBA8 GPU frameを生成する。詳細は`SYNTHETIC_FRAME_SOURCE.md`を参照する。

## 14. Samples

```text
SingleCameraReadOnlyReadbackD3D11
MultiCameraReadOnlySyncD3D11
MultiPipelineStressD3D11
```

`MultiPipelineStressD3D11`は10 consumerを同時起動し、実cameraと`--synthetic`の両方に対応する。

```text
pair display / id0 display / id1 display
pair video / id0 video / id1 video
D3D11 HLSL Sobel
OpenCV Canny / OpenCV Sobel
OpenCV processed pair display
```

CPU/display/video consumerは各自readbackする。OpenCVはsampleだけの依存であり、IC4Ext library本体はOpenCVに依存しない。

## 15. Tests

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
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON
```
