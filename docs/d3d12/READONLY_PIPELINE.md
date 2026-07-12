# IC4Ext 2.0.0 D3D12 ReadOnly Frame Pipeline

この文書は、IC4Ext 2.0.0における正式なD3D12 camera pipelineの設計、所有権、同期、実行時変更、GPU lifetime、readback、検証方法を定義する。

## 1. Public API

新しいD3D12コードは次をincludeし、`IC4Ext::D3D12`名前空間を使う。

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

namespace Pipe = IC4Ext::D3D12;
```

代表的なpublic type:

```cpp
Pipe::CameraCapture
Pipe::CameraCaptureOptions
Pipe::ReadResult
Pipe::CameraCaptureThread
Pipe::CameraCaptureThreadOptions
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
```

`V2`は機能名ではないためpublic namespaceとして使用しない。現状、一部の実装本体は物理移動の途中で`include/IC4Ext/V2`または`src/V2`に残っているが、public APIとCMake build entryは`IC4Ext::D3D12`である。

## 2. Compatibility policy

IC4Ext 2.0.0では、旧D3D12 physical-copy fan-out APIとのsource compatibilityを保証しない。

旧D3D12の次の構成は正式経路から外した。

```text
CameraCaptureThread
    -> output queueごとにD3D12FrameCopier
    -> consumer専用textureを毎出力生成
```

新構成では、capture/sync層から公開されるframeはReadOnlyのみである。書き込みが必要なconsumerは、自分専用のdestination resourceを確保する。

## 3. Architecture

```text
IC4 QueueSink / ImageBuffer
        |
        v
IC4Ext::D3D12::CameraCapture
  - IC4 device / stream
  - D3D12FrameConverter core
  - UploadRing
  - per-command-slot reusable input buffer
  - producer fence
  - CameraCapture-owned FramePool
        |
        v
ReadOnlyFrame
        |
        v
CameraCaptureThread
        |
        v
IndexedReadOnlyFrameQueue
        |
        v
FrameSyncThread
  - timestamp-nearest matching
  - complete synchronized set
  - runtime output snapshot
  - priority / FPS / required camera selection
        |
        +----> ReadOnlyFrameSetQueue A
        +----> ReadOnlyFrameSetQueue B
        +----> ReadOnlyFrameSetQueue C
        |
        v
GPU / CPU / recording consumers
```

`CameraCaptureThread`と`FrameSyncThread`はGPU textureを複製しない。fan-outは`ReadOnlyFrame`の共有参照を複製することで行う。

## 4. Ownership model

### 4.1 Producer resource owner

完成frame用D3D12 texture poolは`CameraCapture`が所有する。

```text
CameraCapture
  +-- PooledFrameConverter
  |     +-- UploadRing
  |     +-- command slots
  |     +-- reusable input buffers
  +-- FramePool
        +-- Texture 0
        +-- Texture 1
        +-- ...
```

`CameraCaptureThread`、`FrameSyncThread`、consumerはpoolそのものを所有しない。

### 4.2 UploadRingとFramePoolの違い

```text
UploadRing
  CPU camera bytesをGPUへ転送する一時upload memory。
  converter command slot側が管理する。

FramePool
  compute conversion後の完成Texture2Dを保持する。
  CameraCaptureが所有する。
```

UploadRing上の領域はproducer command完了後に再利用できる。公開Textureは最後のReadOnly参照とconsumer GPU workが完了するまで再利用できない。

### 4.3 FramePool entry state

概念上の状態遷移:

```text
Available
    |
    | acquire()
    v
Writing
    |
    | publish(readyToken, metadata)
    v
Published
    |
    | 最後のReadOnlyFrame参照が解放
    v
Available
```

`FrameWriter`はmove-onlyである。`publish()`後に同じwriterから再度書き込むことはできない。

## 5. ReadOnlyFrame contract

`ReadOnlyFrame`は次を共有保持する。

```text
ID3D12Resource
SRV descriptor heap / handles
DXGI format
published resource state
producer-ready fence token
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
pool return callback
```

consumerに許可する操作:

```text
SRVとして読む
COPY_SOURCEとして読む
metadataを読む
別resourceへの入力として使う
```

禁止する操作:

```text
元TextureへUAV書き込み
元TextureをCOPY_DESTとして上書き
元Textureをrender targetとして変更
他consumerと競合するresource state変更
```

書き込みが必要なconsumerは、専用output poolまたは専用resourceを作る。

## 6. Resource-state contract

producerは`FrameWriter`から次を取得する。

```cpp
writer.initialState();
writer.writeState();
writer.publishedState();
```

基本手順:

```text
initialState
    -> writeState
    -> GPU write
    -> UAV barrier if required
    -> publishedState
    -> queue signal
    -> publish()
```

camera captureのpublished stateは現在`D3D12_RESOURCE_STATE_GENERIC_READ`である。これはshader-readとcopy-source用途を含むため、複数のReadOnly consumerが元resourceをtransitionせずに読める。

## 7. Producer-ready tokenとconsumer lifetime

### 7.1 Producer-ready token

`ReadOnlyFrame::readyToken()`は、producer queueがTextureへの書き込みを完了する時点を表す。

GPU consumerはCPU waitより、consumer queue上のGPU waitを使う。

```cpp
Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, frame);
```

### 7.2 Consumer completion

producer-ready tokenは、consumerのGPU読み取り完了を表さない。

危険な例:

```text
consumer command submit
    -> CPU側ReadOnlyFrameをすぐ解放
    -> poolへ返却
    -> producerが次frameを書き込む
    -> consumer GPUはまだ旧frameを読んでいる
```

安全な方法:

```cpp
Pipe::ReadOnlyFrameLifetimeTracker lifetimeTracker;

Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, frame);
auto consumerDone = SubmitConsumerWorkAndSignal();
lifetimeTracker.retainUntil(frame, consumerDone);
lifetimeTracker.collectCompleted();
```

同期set全体を保持する場合:

```cpp
lifetimeTracker.retainUntil(frameSet, consumerDone);
```

## 8. FramePool sizing

FramePool容量は、同時に生存する共有frame数の上限である。

必要容量に影響するもの:

```text
FrameSyncThread internal buffer
各output queue capacity
latest consumerが処理中のframe
all-frame consumer backlog
GPU completion待ちslot
readback中のframe
recording中のframe
```

概算の安全側上限:

```text
required pool capacity
  >= sync buffer
   + sum(all-frame queue capacities)
   + latest pipeline count
   + active consumer/GPU slots
   + margin
```

ただし全outputが同じ完全同期setを共有するため、単純なqueue容量の総和より少ない場合もある。最終値は`FramePoolStats`で確認する。

重要な統計:

```text
capacity
available
writing
published
acquisitions
dynamicAllocations
exhaustionDrops
waitTimeouts
```

実機10-pipeline試験では、`initial=16, max=64`でpool exhaustionとcapture timeoutが発生し、`initial=128, max=256`で解消した。詳細は`VALIDATION_AND_TUNING.md`を参照する。

## 9. PooledFrameConverter

`PooledFrameConverter`は既存のD3D12 converter coreを使い、FramePoolから取得したoutput textureへ直接書き込む。

```text
IC4 CPU bytes
    -> UploadRing
    -> per-slot default-heap input buffer
    -> compute shader
    -> FramePool output Texture2D
    -> producer fence
    -> ReadOnlyFrame
```

command slotごとにdefault-heap input bufferをcacheする。入力が既存容量以下なら再利用し、大きくなった場合だけ再確保する。

```text
previous use end: NON_PIXEL_SHADER_RESOURCE
next use start:   COPY_DEST
shader input:     NON_PIXEL_SHADER_RESOURCE
```

統計:

```cpp
const auto stats = converter.stats();

stats.conversions;
stats.inputBufferAllocations;
stats.inputBufferReuses;
stats.cachedInputBufferCount;
stats.cachedInputBufferBytes;
```

## 10. CameraCapture

### 10.1 Open

```cpp
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
    const auto error = capture.lastError();
}
```

### 10.2 Lazy pool creation

FramePoolは最初の実IC4 frameから得たnegotiated width、height、output formatに合わせて初期化する。frame shapeが変更された場合は新しいpoolへ切り替える。

既に公開済みのframeは旧pool stateを共有保持するため、pool切替後も有効である。

### 10.3 Read

```cpp
auto result = capture.read(IC4Ext::CameraReadOptions{
    IC4Ext::ReadMode::NextFrame,
    1000});

if (result) {
    Pipe::ReadOnlyFrame frame = std::move(result.frame);
}
```

`read()`はGPU conversion完了をCPU waitせずに返す。consumerは`readyToken()`を尊重する。

## 11. CameraCaptureThread

`CameraCaptureThread`は1台の`CameraCapture`を連続実行し、中央sync ingress queueへ1つの共有ReadOnly handleを提出する。

```text
read(ReadMode::NextFrame)
    -> IndexedReadOnlyCameraFrame{cameraId, frame}
    -> IndexedReadOnlyFrameQueue
```

旧実装のようなper-output fan-outやGPU copyは行わない。

主なAPI:

```cpp
Pipe::CameraCaptureThread camera(
    cameraId,
    selector,
    config,
    backend,
    captureOptions,
    threadOptions);

camera.setOutputQueue(syncInput);
camera.start();
camera.requestStop();
camera.join();
```

camera-free testやcustom producer向けに`ReadOnlyFrameSource`を注入できる。

## 12. FrameSyncThread

### 12.1 One central synchronizer

1つの同期domainにつき、原則1つの`FrameSyncThread`を使う。

```text
CameraCaptureThread x N
    -> one ingress queue
    -> one FrameSyncThread
    -> runtime output table
```

旧方式のように処理ごとにSyncThreadを作らない。

### 12.2 Timestamp-nearest only

frame-number matchingはサポートしない。ハードウェア同期していても、cameraごとにframe counterのepochや開始値が異なるためである。

設定:

```cpp
Pipe::FrameSyncConfig syncConfig;
syncConfig.cameraIds = {0, 1};
syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::HostReceived;
syncConfig.maxTimestampDiffNs = 4'000'000;
syncConfig.maxBufferedFramesPerCamera = 16;
syncConfig.groupTimeout = std::chrono::milliseconds(100);
```

各camera buffer先頭のtimestampについて、最大値と最小値の差がtolerance以内なら完全同期setを作る。範囲外なら最古のfront frameをdropする。

### 12.3 Timestamp source

```text
HostReceived
  process-wide steady_clockで比較可能。
  USB転送、OS scheduling、pool stallの影響を受ける。

Device
  camera device timestampを直接比較する。
  camera間で同じepoch/clock domainの場合のみ有効。

Auto
  実装規則に従って利用可能なtimestampを選ぶ。
```

### 12.4 Tolerance warning

160 fpsのframe periodは6.25 msである。toleranceをframe periodより大きくすると、隣接frameを誤ってpairingする可能性がある。

大きいtoleranceは経路の動作確認には使えるが、pool exhaustionを解消した後に最小安定値へ下げる。

## 13. Complete setとpartial output set

初期実装では、まず全cameraが揃った完全同期setを作る。

```text
Complete set: {camera0, camera1, camera2, ...}
```

その後、各outputの`requiredCameras`に従って参照だけを選ぶ。

```text
Output A: {0,1}
Output B: {0}
Output C: {1,3}
```

部分set生成でGPU resourceをcopyしない。

全cameraが揃わないsetは、特定outputがそのcameraを必要としなくても配送されない。この制約は初期実装の単純化として受け入れる。

## 14. Runtime output registry

登録:

```cpp
Pipe::FrameSyncOutputConfig outputConfig;
outputConfig.requiredCameras = {0, 1};
outputConfig.frameRate = Pipe::FrameRateLimit::Maximum();
outputConfig.priority = 100;
outputConfig.enabled = true;

const auto outputId = sync.registerOutput(queue, outputConfig);
```

実行中に変更可能:

```cpp
sync.updateOutput(outputId, newConfig);
sync.replaceOutputQueue(outputId, newQueue);
sync.unregisterOutput(outputId);
```

設定tableはcopy-on-write snapshotとしてpublishする。現在配送中のsetは旧snapshotを使い、次の完全同期setから新設定を使う。

`unregisterOutput()`後でも、既に配送snapshotへ入った1 setが旧queueへ届く可能性がある。

## 15. Priority

`priority`が大きいoutputから先に処理する。同priorityでは登録順を維持する。

priorityは次を意味する。

```text
FrameSyncThread内のdispatch順
```

priorityが保証しないもの:

```text
OS thread scheduling
GPU queue scheduling
consumer処理完了順
低priority outputの自動省略
```

## 16. FPS gate

outputごとに次を設定できる。

```cpp
Pipe::FrameRateLimit::Maximum();
Pipe::FrameRateLimit::Fixed(30.0);
```

`FrameSyncThread`はsleepしない。完全同期setのtimestampに基づき、partial set生成前に対象setを選択する。

FPS gateで削減できるもの:

```text
partial FrameSet allocation
ReadOnly handle copy
queue push
後段consumer処理
```

captureと完全同期set構築は常に行う。

## 17. Queue policy

中央sync threadはoutput queue pushで長時間blockしてはならない。

### Latest display

```text
capacity       1
policy         DropOldest
consumer pop   waitPopLatestFor
```

古いframe dropは正常である。

### All-frame processing

```text
capacity       bounded
policy         RejectNew
consumer pop   FIFO
```

queue full時はそのoutputだけdropとして記録し、他outputとcaptureを継続する。

「すべてのframeを処理する」は、queue dropが0である構成が入力rateへ追従できたことを意味する。無限bufferを意味しない。

## 18. Readback

`D3D12FrameReadback`はReadOnly専用overloadを持つ。

```cpp
IC4Ext::D3D12FrameReadback readback;
readback.initialize(consumerBackend);

IC4Ext::CpuFrame cpu;
readback.readback(
    readOnlyFrame,
    IC4Ext::CpuFrameFormat::BGR8,
    cpu,
    5000);
```

ReadOnly経路の動作:

```text
published stateがCOPY_SOURCEを含むことを検証
consumer queueへproducer fenceのGpuWaitを登録
元TextureをtransitionせずCopyTextureRegion
consumer queue completionをCPU wait
readback bufferからCpuFrameへ変換
```

複数CPU consumerが同じ`D3D12FrameReadback`を共有してはならない。各consumerは専用queue、command context、readback cacheを持つ。

## 19. Tests

### no-camera / type tests

```text
test_d3d12_readonly_pipeline
```

### Real D3D12 device, no camera

```text
test_d3d12_pooled_converter_device
```

検証内容:

```text
FramePool acquire/publish/release
real compute conversion
producer fence
GPU readback pixel compare
per-slot input buffer allocation/reuse
```

### Dummy source integration

```text
test_d3d12_dummy_capture_sync_integration
```

検証経路:

```text
ReadOnlyFrameSource x2
    -> CameraCaptureThread x2
    -> FrameSyncThread
    -> output queue
```

## 20. Samples

```text
SingleCameraReadOnlyReadbackD3D12
MultiCameraReadOnlySyncD3D12
MultiPipelineStressD3D12
```

10-pipeline stress sampleの詳細:

```text
samples/MultiPipelineStressD3D12/README.md
docs/d3d12/VALIDATION_AND_TUNING.md
```

## 21. Preliminary validation summary

2026-07-12の予備実測では、10-pipeline構成で次が確認された。

```text
pool 16/64:
  pool exhaustionとcapture timeoutが発生
  synchronized rate 約25 fps

pool 128/256:
  pool exhaustion 0
  capture timeout 0
  sync drop 0
  synchronized rate 約53.36 fps
```

この結果は、FramePool sizingがcapture throughputへ直接影響することを示す。

同じ実行でHLSL Sobelは入力rateへ追従したが、OpenCV VideoWriterは約7-17 fpsであり、全フレーム保存には追従しなかった。

## 22. Dependency policy

IC4Ext本体の依存はv1.x系から維持する。

```text
D3D11Helper   v1.12.1
D3D12Helper   v1.12.1
ThreadKit     main
nlohmann/json v3.11.3
```

OpenCVは`MultiPipelineStressD3D12`など一部sampleだけの依存であり、IC4Ext library本体の依存ではない。

`D3DVideoEncoder`は固定D3D12Helper v1.12.1より新しいhelper headerを要求する場合があるため、現在は既定buildへ組み込んでいない。

## 23. Remaining work

1. `include/IC4Ext/V2`と`src/V2`に残る実装本体を、通常のD3D12 pathへ物理移動する。
2. 2台160 fpsを実際に供給できるcamera/trigger設定を確立する。
3. large pool状態でtimestamp toleranceを再調整する。
4. pair timestamp deltaのp50/p95/p99/maxを統計へ追加する。
5. OpenCV VideoWriterをD3D12 hardware encoderへ置き換える。
6. IC4 stream statisticsとcamera performance snapshotをstress CSVへ統合する。
7. runtime output updateを含む長時間stress testを追加する。
8. device removal、DRED、fence timeoutのfailure pathを試験する。
9. 10/12/16bit、packed Bayer、YUV/NV12等を必要に応じて追加する。
10. D3D12-D3D11 interopを必要に応じて実装する。
