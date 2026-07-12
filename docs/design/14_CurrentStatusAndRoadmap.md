# 14. Current status and roadmap

この文書はIC4Ext 2.0.0 branchの実装状態、実機検証結果、未実装項目、優先順位をまとめる。

## 1. Version and policy

```text
VERSION              2.0.0
D3D12 public namespace IC4Ext::D3D12
D3D12 public include   IC4Ext/D3D12/ReadOnlyPipeline.hpp
D3D12 compatibility    v1 physical-copy fan-outとの互換性は保証しない
D3D11 status           既存APIを維持
```

D3D12では、capture/sync層が公開するframeをReadOnlyへ統一した。

## 2. Implemented Core

```text
CameraCaptureConfig
CameraStreamRequest
CameraReadOptions
CameraSyncMode
CameraSyncConfig
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
FrameReadbackCacheStats
CameraCaptureStats
CameraPerformanceSnapshot
IC4StreamStatistics
CameraTimingPerformance
CameraTemperatureReading
CpuFrame
ErrorInfo
backend selection macros
```

## 3. Implemented D3D11

```text
D3D11CameraCapture
D3D11CameraCaptureThread
D3D11FrameConverter
D3D11FrameCopier
D3D11FrameSyncThread
D3D11DummyCameraCapture
D3D11DummyCameraCaptureGenerator
D3D11FrameReadback
```

D3D11は既存のcopy fan-out modelを維持している。D3D12 2.0.0と完全に同じpublic architectureではない。

## 4. Implemented D3D12 ReadOnly pipeline

### 4.1 Backend and producer

```text
D3D12BackendContext
D3D12ReadyToken
D3D12FenceManager
D3D12FrameConverter core
IC4Ext::D3D12::PooledFrameConverter
IC4Ext::D3D12::FramePool
IC4Ext::D3D12::FrameWriter
IC4Ext::D3D12::ReadOnlyFrame
IC4Ext::D3D12::CameraCapture
```

実装内容:

- `D3D12Core` / `D3D12Queue`統合。
- UploadRingを使ったCPU-to-GPU転送。
- command slotごとのdefault-heap input buffer再利用。
- CameraCapture所有の完成Texture2D pool。
- bounded dynamic growth。
- `DropNewest` / `WaitWithTimeout` exhaustion policy。
- move-only writerと一方向`publish()`。
- producer-ready fence token。
- lazy pool creationとframe shape変更時のpool切替。
- runtime IC4 property setter、trigger control、performance snapshot。

### 4.2 Capture thread

```text
IC4Ext::D3D12::CameraCaptureThread
IC4Ext::D3D12::ReadOnlyFrameSource
```

実装内容:

- `ReadMode::NextFrame`連続取得。
- 中央`IndexedReadOnlyFrameQueue`へ共有handleを1回push。
- physical GPU copy fan-outなし。
- start/stop/start安全化。
- output queueの実行中差替え。
- camera-free/custom producer向けsource injection。

### 4.3 Central synchronization and fan-out

```text
IC4Ext::D3D12::FrameSyncThread
IC4Ext::D3D12::ReadOnlyFrameSet
FrameSyncConfig
FrameSyncOutputConfig
FrameSyncOutputStats
```

実装内容:

- timestamp-nearest matchingのみ。
- frame-number matching非対応。
- 全cameraが揃ったcomplete set生成。
- outputごとのrequired cameras選択。
- `Maximum` / fixed FPS gate。
- priority順dispatch。
- runtime register/update/queue replacement/unregister。
- immutable output snapshot。
- per-output/global statistics。
- non-blocking output push。

### 4.4 Consumer lifetime and readback

```text
IC4Ext::D3D12::WaitForReadOnlyFrameReadyOnQueue
IC4Ext::D3D12::ReadOnlyFrameLifetimeTracker
D3D12FrameReadback(ReadOnlyFrame overload)
```

実装内容:

- producer fenceをconsumer queue上でGPU wait。
- consumer completion fenceまで入力handle保持。
- ReadOnly sourceをstate transitionせずCOPY_SOURCEとしてreadback。
- D3D12 readback buffer cache。
- metadata/chunk metadata継承。

## 5. Settings and control

```text
IC Capture 4 JSON state loading
NoSync / HardwareTrigger / SoftwareTrigger helper
softwareTrigger() command execution
OffsetX / OffsetY
ExposureTime / Gain / Gamma / AcquisitionFrameRate
ROI / PixelFormat
arbitrary IC4 property setter
```

hardware synchronizationでは、外部TTL/trigger line/master output等で露光開始を揃える。IC4Extはproperty適用、frame取得、timestamp記録、取得後のset化を担当する。

## 6. Metadata and diagnostics

### Chunk metadata

```text
ChunkBlockId
ChunkExposureTime
ChunkGain
ChunkIMX174FrameId
ChunkIMX174FrameSet
ChunkMultiFrameSetId
ChunkMultiFrameSetFrameId
```

### Performance snapshot

```text
CameraCaptureStats
IC4StreamStatistics
CameraTimingPerformance
CameraTemperatureReading[]
```

`CameraTimingPerformance`にはdevice/host fps、jitter、frame-number gap、estimated dropを含む。

## 7. Automated tests

### Common / D3D11

```text
test_core
test_cpu_frame
test_backend_config
test_chunk_metadata
test_no_camera_pipeline_stress
test_d3d11_frame_readback
test_d3d11_dummy_camera_capture
test_d3d11_frame_sync_thread
test_single_camera_smoke
test_camera1_readback_integration
test_camera1_long_run_stress
```

### D3D12

```text
test_d3d12_core
test_d3d12_shader_reference
test_d3d12_readonly_pipeline
test_d3d12_pooled_converter_device
test_d3d12_dummy_capture_sync_integration
test_d3d12_shader_compile
```

`test_d3d12_pooled_converter_device`:

- real D3D12 device、camera不要。
- FramePool acquire/publish/release。
- real compute conversion。
- fence completion。
- readback pixel compare。
- 4 slot分のinput buffer allocationと再利用。

`test_d3d12_dummy_capture_sync_integration`:

- `ReadOnlyFrameSource` x2。
- `CameraCaptureThread` x2。
- timestamp synchronization。
- output statistics。
- pool return確認。

## 8. Samples

```text
IC4DeviceDiagnostics
SingleCameraReadOnlyReadbackD3D12
MultiCameraReadOnlySyncD3D12
MultiPipelineStressD3D12
```

`MultiPipelineStressD3D12`は10 outputを同時に動作させる。

```text
latest display x3
OpenCV VideoWriter recording x3
HLSL Sobel x1
OpenCV all-frame processing x2
OpenCV processed latest pair display x1
```

CPU/display/video系は各自で独立readbackする。

## 9. Preliminary real-camera validation

2026-07-12時点の予備結果。特定環境の値であり保証値ではない。

### 9.1 Pool 16/64

```text
measurement      60.015 s
syncFps          25.210
syncDropRate     0.357
camera0Read      3187
camera1Read      1516
camera0Timeouts  15
camera1Timeouts  1687
pool0Exhaustion  15
pool1Exhaustion  1687
```

結論:

- 10 outputの共有lifetimeに対してpoolが不足した。
- camera1側でpool exhaustionとcapture timeoutが継続した。
- この状態のsync fpsはcamera hardwareの能力を示さない。

### 9.2 Pool 128/256

```text
measurement      60.004 s
syncInput        6403
syncSets         3202
syncFps          53.363
syncDrops        0
camera0Read      3201
camera1Read      3202
camera timeouts  0 / 0
pool exhaustion  0 / 0
```

結論:

- large poolでcapture stall、pool exhaustion、sync dropが解消した。
- 2台はほぼ同数を供給した。
- この実行の入力rateは約53 fpsであり、160 fpsではない。
- 外部trigger周波数、露光、ROI、JSON、USB帯域、camera設定の切り分けが必要。

### 9.3 Consumer throughput

```text
HLSL Sobel             約53.36 fps, drop 0
OpenCV Canny           約42.61 fps
OpenCV Sobel           約22.98 fps
single AVI recording   約16-17 fps
pair AVI recording     約7-8 fps
```

結論:

- GPU HLSL pathは入力rateへ追従した。
- OpenCV VideoWriterは全フレーム保存の最終方式として不十分。
- hardware video encoder integrationが必要。

## 10. Known correctness/tuning issues

### Timestamp tolerance

30 ms toleranceで同期経路は動作したが、53 fpsのframe period約18.9 ms、160 fpsの6.25 msより大きい。

したがって30 msは最終production値ではない。pool exhaustionを解消した状態でtoleranceを再スイープし、pair timestamp delta分布を確認する。

### Host timestamp

HostReceivedは共通clock domainだが、USB/scheduling/backlogの影響を含む。

### Device timestamp

2台のdevice timestamp epoch/clock domainが一致する場合だけabsolute比較できる。異なる場合はoffset calibrationが必要。

## 11. Not implemented / incomplete

### 11.1 Physical source relocation

public APIとCMake build pathは`IC4Ext::D3D12`へ移行済みだが、一部実装本体が次に残る。

```text
include/IC4Ext/V2
src/V2
```

macro wrapper includeを廃止し、通常のD3D12 pathへ物理移動する必要がある。

### 11.2 Hardware video encoding

OpenCV CPU readback + VideoWriterを置き換えるD3D12 hardware encoder pathが未統合。

必要事項:

```text
ReadOnlyFrame SRV/COPY_SOURCE input
encoder-compatible private surface
GPU color/format conversion
hardware encoder submission
consumer completion lifetime
per-encoder independent queue/backpressure
```

### 11.3 160 fps acceptance

現在の10-pipeline実機入力は約53 fps。次を満たす160 fps testは未完了。

```text
camera0/1 read ~= 160 fps
sync ~= 160 sets/s
pool exhaustion 0
sync drop許容範囲
HLSL drop 0
hardware recording drop 0
10分以上のsoak
```

### 11.4 Timestamp diagnostics

未実装:

```text
pair delta p50/p95/p99/max
camera別host arrival jitter distribution
calibrated device timestamp offset
sync mismatch reason counters
```

### 11.5 Failure-path validation

未実装:

```text
D3D12 device removal
DRED report capture
queue/fence timeout injection
DXC/shader load failure injection
pool corruption/lifetime assertion
camera disconnect/reconnect
```

### 11.6 Interop and formats

```text
D3D12-D3D11 shared resource interop
10/12/16bit
packed Bayer
YUV / YCbCr
polarized
MJPG / NV12 input
```

## 12. Recommended next steps

優先順位:

```text
1. D3D12 implementation bodyをV2 pathから物理移動
2. large poolでtimestamp tolerance再スイープ
3. sync-onlyで160 Hz trigger/camera throughput切り分け
4. stress CSVへpair timestamp deltaとCameraPerformanceSnapshotを追加
5. D3D12 hardware encoder path
6. runtime output updateを含む10分/1時間soak
7. device removal / DRED failure tests
8. 必要に応じてformat拡張
9. 必要に応じてD3D12-D3D11 interop
```

## 13. Authoritative documents

```text
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
samples/MultiPipelineStressD3D12/README.md
```
