# IC4Ext 2.0.0 D3D12 ReadOnly Frame Pipeline

この文書は、IC4Ext 2.0.0における正式なD3D12 camera pipelineの設計、所有権、同期、実行時output lifecycle、GPU lifetime、readback、検証方法を定義する。

## 1. Public API

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D12;
```

代表的なpublic type:

```text
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
Pipe::FrameSyncOutputState
Pipe::ReadOnlyFrameLifetimeTracker
Pipe::ReadOnlyFrameSource
```

`V2`は機能名ではないためpublic namespaceとして使用しない。一部の実装本体は物理移動の途中で`include/IC4Ext/V2`または`src/V2`に残るが、public APIとCMake build entryは`IC4Ext::D3D12`である。

## 2. Compatibility policy

IC4Ext 2.0.0では、旧D3D12 physical-copy fan-out APIとのsource compatibilityを保証しない。

旧構成:

```text
CameraCaptureThread
    -> output queueごとにD3D12FrameCopier
    -> consumer専用textureを毎出力生成
```

正式構成では、capture/sync層から公開されるframeはReadOnlyのみである。書き込みが必要なconsumerは、自分専用のdestination resourceを確保する。

## 3. Architecture

```text
IC4 QueueSink / ImageBuffer
        |
        v
IC4Ext::D3D12::CameraCapture
  - IC4 device / stream
  - D3D12FrameConverter core
  - UploadRing
  - reusable input buffers
  - producer fence
  - CameraCapture-owned FramePool
        |
        v
ReadOnlyFrame
        |
        v
CameraCaptureThread x N
        |
        v
one IndexedReadOnlyFrameQueue
        |
        v
one FrameSyncThread
  - timestamp-nearest matching
  - complete synchronized set
  - priority / FPS / required camera selection
  - runtime output lifecycle
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

完成frame用D3D12 texture poolは`CameraCapture`が所有する。

```text
CameraCapture
  +-- PooledFrameConverter
  |     +-- UploadRing
  |     +-- reusable input buffers
  +-- FramePool
        +-- Texture 0
        +-- Texture 1
        +-- ...
```

`CameraCaptureThread`、`FrameSyncThread`、consumerはpoolそのものを所有しない。

概念上のFramePool entry状態:

```text
Available
    -> Writing
    -> Published
    -> 最後のReadOnly参照とconsumer GPU work完了
    -> Available
```

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

許可する操作:

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

producerは概ね次の順序で公開する。

```text
initialState
    -> writeState
    -> GPU write
    -> UAV barrier if required
    -> publishedState
    -> queue signal
    -> publish()
```

camera captureのpublished stateは`D3D12_RESOURCE_STATE_GENERIC_READ`である。shader-readとcopy-source用途を含むため、複数ReadOnly consumerが元resourceをtransitionせずに読める。

## 7. Producer-ready tokenとconsumer lifetime

producer-ready tokenはTextureへのproducer書き込み完了を表す。consumer GPU処理完了は表さない。

```cpp
Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, frame);
auto consumerDone = SubmitConsumerWorkAndSignal();
lifetimeTracker.retainUntil(frame, consumerDone);
lifetimeTracker.collectCompleted();
```

同期set全体を保持する場合:

```cpp
lifetimeTracker.retainUntil(frameSet, consumerDone);
```

consumer GPU commandが読み終える前に入力handleを解放してはならない。

## 8. FramePool sizing

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

```text
IC4 CPU bytes
    -> UploadRing
    -> reusable default-heap input buffer
    -> compute shader
    -> FramePool output Texture2D
    -> producer fence
    -> ReadOnlyFrame
```

入力bufferはcommand slotごとにcacheし、既存容量以下なら再利用する。

## 10. CameraCapture

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
capture.open(selector, config, backend, options);
```

FramePoolは最初の実IC4 frameから得たnegotiated width、height、output formatに合わせて初期化する。frame shape変更時はfuture acquisition用に新しいpoolへ切り替え、既存frameは旧pool stateを共有保持する。

`read()`はGPU conversion完了をCPU waitせずに返す。consumerは`readyToken()`を尊重する。

## 11. CameraCaptureThread

`CameraCaptureThread`は1台の`CameraCapture`を連続実行し、中央sync ingress queueへ1つの共有ReadOnly handleを提出する。

```text
read(ReadMode::NextFrame)
    -> IndexedReadOnlyCameraFrame{cameraId, frame}
    -> IndexedReadOnlyFrameQueue
```

旧実装のようなper-output fan-outやGPU copyは行わない。

## 12. FrameSyncThread

1つの同期domainにつき、原則1つの`FrameSyncThread`を使う。

```text
CameraCaptureThread x N
    -> one ingress queue
    -> one FrameSyncThread
    -> output queues x N
```

frame-number matchingはサポートしない。cameraごとにframe counterのepochや開始値が異なり得るため、timestamp-nearestを使う。

```cpp
Pipe::FrameSyncConfig syncConfig;
syncConfig.cameraIds = {0, 1};
syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::HostReceived;
syncConfig.maxTimestampDiffNs = 4'000'000;
syncConfig.maxBufferedFramesPerCamera = 16;
syncConfig.groupTimeout = std::chrono::milliseconds(100);
```

160 fpsのframe periodは6.25 msである。toleranceをframe periodより大きくすると隣接frameを誤pairingする可能性がある。

## 13. Complete setとpartial output set

まず全cameraが揃った完全同期setを作り、その後`requiredCameras`に従って参照だけを選ぶ。

```text
Complete set: {0,1,2,3}
Output A:    {0,1}
Output B:    {0}
Output C:    {1,3}
```

部分set生成でGPU resourceをcopyしない。

## 14. Runtime output lifecycle

### 14.1 Add

```cpp
Pipe::FrameSyncOutputConfig outputConfig;
outputConfig.requiredCameras = {0, 1};
outputConfig.frameRate = Pipe::FrameRateLimit::Maximum();
outputConfig.priority = 100;
outputConfig.enabled = true;

const auto outputId = sync.registerOutput(queue, outputConfig);
```

consumerを初期化・開始し、queue待機可能になってから登録する。同じqueue instanceを複数output IDへ登録できない。

### 14.2 Update

```cpp
sync.updateOutput(outputId, newConfig);
```

updateは`Active`状態だけで許可する。queueは不変であり、queue replacement APIは提供しない。置換相当の処理は新outputの追加と旧outputの二段階退役へ分解する。

### 14.3 Stop supply

```cpp
sync.stopOutputSupply(outputId);
```

成功して戻った時点で、対象outputへのpushは実行中でも今後開始されることもない。dispatchとlifecycle命令を同じmutexで線形化するため、古いsnapshotによるlate pushも残らない。

queueはopenのままであり、既存frame setは保持される。状態は`SupplyStopped`となり、再開はできない。

### 14.4 Drain or discard

使い切る場合:

```cpp
sync.stopOutputSupply(outputId);
sync.closeOutputChannel(outputId);
while (auto set = queue->waitPop()) {
    Process(*set);
}
```

破棄する場合:

```cpp
sync.stopOutputSupply(outputId);
queue->clear();
sync.closeOutputChannel(outputId);
```

### 14.5 Close channel

```cpp
sync.closeOutputChannel(outputId);
```

`SupplyStopped`または`Faulted`でだけ成功する。queueをcloseし、待機consumerをwakeし、`FrameSyncThread`のqueue参照とregistry entryを解放する。queue内要素は自動clearしない。

詳細は`../OUTPUT_LIFECYCLE.md`を参照する。

## 15. Output fault isolation

供給中のqueueが外部からcloseされた場合や、1 outputのdispatchで例外が発生した場合、そのoutputだけを`Faulted`へ移す。中央workerと他outputは継続する。

```cpp
auto state = sync.outputState(outputId);
auto stats = sync.outputStats(outputId);
auto error = sync.outputLastError(outputId);
```

追加統計:

```text
dispatchErrors
closedQueuePushes
```

## 16. PriorityとFPS gate

`priority`が大きいoutputから先に処理し、同priorityでは登録順を維持する。

```cpp
Pipe::FrameRateLimit::Maximum();
Pipe::FrameRateLimit::Fixed(30.0);
```

FPS gateはpartial set生成、shared handle copy、queue push、後段処理を削減する。captureと完全同期set構築は継続する。

## 17. Queue policy

Latest display:

```text
capacity       1
policy         DropOldest
consumer pop   waitPopLatestFor
```

All-frame processing:

```text
capacity       bounded
policy         RejectNew
consumer pop   FIFO
```

queue full時はそのoutputだけdropとして記録し、他outputとcaptureを継続する。

## 18. Readback

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

複数CPU consumerは専用queue、command context、readback cacheを持つ。

## 19. Tests

```text
test_d3d12_readonly_pipeline
test_d3d12_pooled_converter_device
test_d3d12_dummy_capture_sync_integration
test_d3d12_synthetic_source_sync_integration
test_d3d12_dynamic_output_lifecycle
```

`test_d3d12_dynamic_output_lifecycle`は常設outputを継続させたまま、動的outputのadd、supply stop、drain/clear、channel closeを200回繰り返す。stop復帰後のlate pushがなく、1 outputのfaultが中央syncや他outputへ波及しないことを確認する。

実camera acceptance:

```text
test_d3d12_multi_camera_pipeline_e2e
test_d3d12_hardware_trigger_pipeline_smoke
test_d3d12_160fps_long_run_acceptance
```

2台・1536x1536・hardware trigger・160 fpsの30分試験では287,997同期set、159.998 fps、camera timeout、sync drop、output drop、FramePool exhaustionすべて0を確認した。

## 20. Samples

```text
SingleCameraReadOnlyReadbackD3D12
MultiCameraReadOnlySyncD3D12
MultiPipelineStressD3D12
```

詳細:

```text
samples/MultiPipelineStressD3D12/README.md
docs/d3d12/VALIDATION_AND_TUNING.md
docs/d3d12/MULTI_CAMERA_PIPELINE_ACCEPTANCE.md
```

## 21. Dependency policy

```text
D3D11Helper   v1.12.1
D3D12Helper   v1.12.1
ThreadKit     main
nlohmann/json v3.11.3
```

OpenCVは一部sampleだけの依存であり、IC4Ext library本体の依存ではない。

## 22. Remaining work

1. `include/IC4Ext/V2`と`src/V2`に残る実装本体を通常のD3D12 pathへ物理移動する。
2. dynamic output lifecycleを実camera 160 fps中にも反復するacceptance testを追加する。
3. pair timestamp deltaのp50/p95/p99/maxをlibrary統計へ追加する。
4. device removal、DRED、fence timeoutのfailure pathを試験する。
5. 10/12/16bit、packed Bayer、YUV/NV12等を必要に応じて追加する。
6. D3D12-D3D11 interopを必要に応じて実装する。
