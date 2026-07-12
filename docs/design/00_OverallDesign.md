# 00. Overall design

IC4Extは、IC Imaging Control 4 SDKから取得したcamera frameを、Direct3D 11 / Direct3D 12のGPU resourceとして扱うC++17 libraryである。

project version 2.0.0では、D3D12の正式経路をReadOnly frame pipelineへ変更した。D3D11は既存APIを維持するが、D3D12と同じpublic shapeであることは保証しない。

## 1. 目的

- IC4 camera frameをGPU resourceとして取得する。
- 通常pipelineではGPU上に維持し、必要なconsumerだけがreadbackする。
- D3D12では1つの完成textureを複数consumerへReadOnly共有する。
- 複数cameraをtimestampで同期し、完全同期setを複数outputへ配送する。
- outputごとに必要camera、FPS、priority、enabled状態を実行中に変更できる。
- metadata、chunk metadata、GPU ready token、consumer lifetimeを明示する。
- camera-free test用にsynthetic ReadOnly sourceを注入できる。

## 2. Layer structure

```text
IC4 camera / QueueSink
    |
    v
Core
  CameraCaptureConfig
  FrameTiming
  FrameFormatMetadata
  FrameChunkMetadata
  CpuFrame
  ErrorInfo
    |
    +-------------------------------+
    |                               |
    v                               v
D3D11 backend                  D3D12 ReadOnly backend
legacy frame API               IC4Ext::D3D12 namespace
D3D11CameraFrame               ReadOnlyFrame
D3D11 copy fan-out             shared immutable fan-out
    |                               |
    +---------------+---------------+
                    v
Application / processing
  renderer
  compute
  encoder
  OpenCV / inspection
  logging / tests
```

## 3. D3D12 end-to-end path

```text
IC4 ImageBuffer
    -> CameraCapture
    -> UploadRing
    -> per-slot reusable default-heap input buffer
    -> compute conversion
    -> CameraCapture-owned FramePool Texture2D
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> IndexedReadOnlyFrameQueue
    -> central FrameSyncThread
    -> complete synchronized set
    -> priority/FPS/required-camera selection
    -> ReadOnlyFrameSetQueue x N
    -> consumers
```

`CameraCaptureThread`と`FrameSyncThread`はoutputごとのGPU copyを行わない。

## 4. Normal path and readback path

### 4.1 GPU normal path

```text
ReadOnlyFrame
    -> wait producer fence on consumer queue
    -> read as SRV/COPY_SOURCE
    -> write to consumer-owned output resource if needed
    -> retain input until consumer completion fence
```

### 4.2 CPU readback path

```text
ReadOnlyFrame
    -> consumer-owned D3D12 queue
    -> consumer-owned D3D12FrameReadback
    -> consumer-owned readback buffer cache
    -> CpuFrame
    -> OpenCV / file / checksum / diagnostics
```

複数CPU pipelineはreadback contextやCpuFrameを共有しない。

## 5. Backend status

| Backend | 状態 |
|---|---|
| D3D11 | 既存capture/thread/sync/copy/readback APIを実装済み |
| D3D12 | `IC4Ext::D3D12` ReadOnly pipelineを正式経路として実装済み |
| D3D12-D3D11 interop | 未実装 |

D3D12初期化は`D3D12CoreLib::D3D12Core`から作った`D3D12BackendContext`を使う。

```cpp
auto core = D3D12CoreLib::D3D12Core::CreateShared();
auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
```

raw `ID3D12Device*` / `ID3D12CommandQueue*`だけの初期化は正式経路ではない。

## 6. Frame metadata

`FrameTiming`:

```text
frameNumber
  cameraが報告したframe counter。D3D12 frame synchronizationには使わない。

deviceTimestampNs
  camera device timestamp。

hostReceivedTime
  process-wide steady_clock上のhost受信時刻。
```

`FrameChunkMetadata`:

```text
ChunkBlockId
ChunkExposureTime
ChunkGain
ChunkIMX174FrameId
ChunkIMX174FrameSet
ChunkMultiFrameSetId
ChunkMultiFrameSetFrameId
```

取得できない項目は対応する`has*` flagがfalseになる。

## 7. Format scope

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

10/12/16bit、packed Bayer、YUV/YCbCr、polarized、MJPG/NV12は未実装である。

## 8. Main public classes

### Core

```text
CameraCaptureConfig
CameraStreamRequest
CameraReadOptions
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
CameraPerformanceSnapshot
CpuFrame
ErrorInfo
```

### D3D11

```text
D3D11CameraCapture
D3D11CameraCaptureThread
D3D11FrameSyncThread
D3D11FrameReadback
D3D11DummyCameraCapture
D3D11DummyCameraCaptureGenerator
```

### D3D12

```text
D3D12BackendContext
IC4Ext::D3D12::CameraCapture
IC4Ext::D3D12::CameraCaptureThread
IC4Ext::D3D12::ReadOnlyFrame
IC4Ext::D3D12::ReadOnlyFrameSet
IC4Ext::D3D12::FramePool
IC4Ext::D3D12::FrameWriter
IC4Ext::D3D12::PooledFrameConverter
IC4Ext::D3D12::FrameSyncThread
IC4Ext::D3D12::ReadOnlyFrameLifetimeTracker
D3D12FrameReadback
```

## 9. Synchronization model

D3D12はtimestamp-nearestのみを使う。frame-number同期は、camera間でcounter epochが一致しないため非対応とする。

初期実装では、全cameraが揃ってから完全同期setを確定する。その後、各outputの`requiredCameras`に従い部分setを作る。

```text
complete set {0,1,2,3}
    -> output A {0,1}
    -> output B {2}
    -> output C {1,3}
```

GPU textureはcopyしない。

## 10. Runtime output model

output registrationは実行中に追加、更新、queue差替え、削除できる。

```text
required cameras
Maximum / Fixed FPS
priority
enabled
output queue
```

変更はimmutable snapshotとしてpublishし、次の完全同期setから反映する。

## 11. Backpressure model

中央sync threadは1つの遅いconsumerでblockしない。

```text
latest display:
  capacity 1
  DropOldest
  waitPopLatest

all-frame processing:
  bounded capacity
  RejectNew
  FIFO
```

all-frame queueでdropが発生した場合、そのconsumerは入力rateへ追従できていない。

## 12. FramePool model

FramePoolはCameraCaptureが所有し、公開frameの最後の共有参照が消えるまでentryを再利用しない。

pool容量は次のin-flight frameを吸収する必要がある。

```text
sync internal buffers
output queue backlog
active CPU readbacks
active GPU slots
recording backlog
```

10-pipeline予備試験では、16/64のpoolで枯渇し、128/256で枯渇が解消した。pool sizingは実測で決める。

## 13. GPU lifetime rule

producer-ready fenceとconsumer completion fenceは別である。

```text
producer ready:
  camera textureへの書き込み完了

consumer completion:
  downstream GPUがtextureを読み終えた時点
```

consumerはGPU完了まで入力ReadOnly handleを保持する。`ReadOnlyFrameLifetimeTracker`を利用できる。

## 14. Important design rules

- D3D12 public APIは`IC4Ext::D3D12`を使う。
- D3D12 capture/sync層はReadOnly frameだけを公開する。
- `CameraCapture`が完成texture poolを所有する。
- `CameraCaptureThread`は中央sync ingressへ1回だけ提出する。
- 1同期domainにつき中央`FrameSyncThread`を1つ使う。
- frame-number同期を使わない。
- output queue pushで中央sync threadを長時間blockしない。
- GPU consumerはproducer fenceを待ち、consumer completionまで入力を保持する。
- CPU consumerは各自でreadbackする。
- OpenCVはsample/application依存であり、IC4Ext library本体の依存にしない。

## 15. Authoritative documents

```text
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
samples/MultiPipelineStressD3D12/README.md
```
