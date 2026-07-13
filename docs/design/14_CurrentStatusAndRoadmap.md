# 14. Current status and roadmap

この文書はIC4Ext 2.0.0 branchの実装状態、実機検証結果、未実装項目、優先順位をまとめる。

## 1. Version and public policy

```text
VERSION                 2.0.0
D3D11 public namespace  IC4Ext::D3D11
D3D11 public include    IC4Ext/D3D11/ReadOnlyPipeline.hpp
D3D12 public namespace  IC4Ext::D3D12
D3D12 public include    IC4Ext/D3D12/ReadOnlyPipeline.hpp
compatibility           v1 physical-copy fan-outとの互換性は保証しない
```

D3D11/D3D12とも、capture/sync層が公開する完成GPU frameをReadOnlyへ統一する。IC4Ext本体の責務はcamera frameをGPU resourceとして安全に提供するところまでであり、動画encoder/container管理はconsumer側の責務である。

## 2. Implemented common core

```text
CameraCaptureConfig / CameraStreamRequest / CameraReadOptions
CameraSyncMode / CameraSyncConfig
FrameTiming / FrameFormatMetadata / FrameChunkMetadata
FrameReadbackCacheStats
CameraCaptureStats / CameraPerformanceSnapshot
IC4StreamStatistics / CameraTimingPerformance / CameraTemperatureReading
CpuFrame / ErrorInfo
backend selection macros
IC4 JSON state and runtime property setters
```

## 3. Implemented D3D11 ReadOnly pipeline

### 3.1 Producer

```text
D3D11BackendContext
D3D11ReadyToken / D3D11FenceManager
D3D11FrameConverter core
IC4Ext::D3D11::PooledFrameConverter
IC4Ext::D3D11::FramePool / FrameWriter
IC4Ext::D3D11::ReadOnlyFrame
IC4Ext::D3D11::CameraCapture
```

実camera hot path:

```text
IC4 ImageBuffer CPU bytes
    -> reusable input-buffer slot
    -> D3D11 compute shader
    -> final CameraCapture-owned FramePool Texture2D
    -> producer fence
    -> ReadOnlyFrame
```

中間完成Textureと追加`CopyResource`は生成しない。4 converter slotでinput buffer/constant bufferを再利用し、`waitIdle()`でshutdown前に全slotを待つ。

D3D11 immediate contextでは、同一contextに対する複数call transactionをshared recursive mutexで保護する。

```text
UpdateSubresource
bind SRV/UAV/CB
Dispatch / CopyResource
binding restore
Signal
```

### 3.2 Capture thread

```text
IC4Ext::D3D11::CameraCaptureThread
IC4Ext::D3D11::ReadOnlyFrameSource
```

- `ReadMode::NextFrame`連続取得。
- 中央`IndexedReadOnlyFrameQueue`へReadOnly handleを1回push。
- physical GPU copy fan-outなし。
- output queueの実行中差替え。
- custom/synthetic source injection。

### 3.3 Central synchronization and fan-out

```text
IC4Ext::D3D11::FrameSyncThread
IC4Ext::D3D11::ReadOnlyFrameSet
FrameSyncConfig / FrameSyncOutputConfig / FrameSyncOutputStats
```

- timestamp-nearest matchingのみ。
- complete camera set生成後、outputごとのrequired camerasを選択。
- `Maximum` / fixed FPS gate。
- priority順dispatch。
- runtime register/update/queue replacement/unregister。
- immutable output snapshot。
- non-blocking bounded output queue。

### 3.4 Consumer lifetime and readback

```text
IC4Ext::D3D11::ReadOnlyFrameLifetimeTracker
D3D11FrameReadback(ReadOnlyFrame overload)
```

- producer fence待機。
- consumer completion fenceまでinput handle保持。
- pipelineごとの独立staging texture cache。
- shared immediate-context transaction mutexによるcopy/map直列化。
- metadata/chunk metadata継承。

### 3.5 Synthetic source

```text
IC4Ext::D3D11::SyntheticFrameSource
```

任意size/fps/seed/timestamp offsetのRGBA8 Texture2DをD3D11 compute shaderで生成する。camera I/O以外のFramePool、capture-thread、sync、readback、HLSL consumer、queue pressureを実機なしで検証できる。

## 4. Implemented D3D12 ReadOnly pipeline

### 4.1 Producer

```text
D3D12BackendContext
D3D12ReadyToken / D3D12FenceManager
D3D12FrameConverter core
IC4Ext::D3D12::PooledFrameConverter
IC4Ext::D3D12::FramePool / FrameWriter
IC4Ext::D3D12::ReadOnlyFrame
IC4Ext::D3D12::CameraCapture
```

- UploadRingを使ったCPU-to-GPU転送。
- command slotごとのdefault-heap input buffer再利用。
- CameraCapture所有完成Texture2D pool。
- producer-ready fence token。
- lazy pool creationとshape変更時のpool切替。

### 4.2 Capture/sync/consumer

D3D11と同じconsumer-facing contractを持つ。

```text
CameraCaptureThread
ReadOnlyFrameSource
central FrameSyncThread
runtime output registry
ReadOnlyFrameLifetimeTracker
ReadOnly readback
SyntheticFrameSource
```

D3D12ではresource stateとqueue/fenceが明示的であり、producer-ready、consumer wait、consumer completionを分離する。

## 5. Samples

### D3D11

```text
SingleCameraReadOnlyReadbackD3D11
MultiCameraReadOnlySyncD3D11
MultiPipelineStressD3D11
```

### D3D12

```text
IC4DeviceDiagnostics
SingleCameraReadOnlyReadbackD3D12
MultiCameraReadOnlySyncD3D12
MultiPipelineStressD3D12
```

両backendのMultiPipelineStress sampleは10 outputを同時に動作させる。

```text
latest display x3
OpenCV VideoWriter recording x3
HLSL Sobel x1
OpenCV all-frame processing x2
OpenCV processed latest pair display x1
```

CPU/display/video consumerは各自で独立readbackする。OpenCVはsample dependencyでありlibrary本体のdependencyではない。

D3D11 stress sampleは`--synthetic`を持ち、cameraなしでも10 consumerを実行できる。

## 6. Automated tests

### Common

```text
test_core
test_cpu_frame
test_backend_config
test_chunk_metadata
test_no_camera_pipeline_stress
```

### D3D11

```text
test_d3d11_frame_readback
test_d3d11_dummy_camera_capture
test_d3d11_frame_sync_thread
test_d3d11_readonly_pipeline
test_d3d11_pooled_converter_device
test_d3d11_synthetic_source_sync_integration
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
test_d3d12_synthetic_source_sync_integration
test_d3d12_shader_compile
```

## 7. Preliminary D3D12 real-camera validation

2026-07-12時点の予備結果。特定環境の値であり保証値ではない。

### Pool 16/64

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

small poolでは10 outputの共有lifetimeを吸収できず、camera1側でpool exhaustionとcapture timeoutが継続した。

### Pool 128/256

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

large poolでcapture stall、pool exhaustion、sync dropが解消した。この実行の入力rateは約53 fpsであり160 fpsではない。

### Consumer throughput

```text
HLSL Sobel             約53.36 fps, drop 0
OpenCV Canny           約42.61 fps
OpenCV Sobel           約22.98 fps
single AVI recording   約16-17 fps
pair AVI recording     約7-8 fps
```

OpenCV VideoWriterはstress workloadであり、高fps保存の最終方式ではない。

## 8. Known tuning issues

### Timestamp tolerance

30 ms toleranceは53 fpsのframe period約18.9 ms、160 fpsの6.25 msより大きい。動作確認には使えるがproduction値ではない。pool exhaustionを解消した状態でtoleranceを再スイープする。

### Host timestamp

HostReceivedは共通clock domainだが、USB、scheduling、queue backlogの影響を含む。

### Device timestamp

2台のdevice timestamp epoch/clock domainが一致する場合だけabsolute比較できる。異なる場合はoffset calibrationが必要。

### D3D11 immediate context

安全性のためmulti-call transactionを直列化している。これはD3D11固有のthroughput ceilingになり得る。必要になった場合はdeferred context + command list submissionを次段階の最適化として検討する。

## 9. Not implemented / incomplete

### 9.1 D3D12 implementation-body relocation

public APIとCMake build pathは`IC4Ext::D3D12`へ移行済みだが、一部実装本体が`include/IC4Ext/V2` / `src/V2`に残る。機能差ではなくsource整理項目である。

### 9.2 Timestamp diagnostics

```text
pair delta p50/p95/p99/max
camera別host arrival jitter distribution
calibrated device timestamp offset
sync mismatch reason counters
```

### 9.3 Failure-path validation

```text
D3D11/D3D12 device removal
DRED report capture (D3D12)
queue/fence timeout injection
shader load failure injection
pool lifetime assertion
camera disconnect/reconnect
```

### 9.4 Format extensions

```text
10/12/16bit
packed Bayer
YUV / YCbCr
polarized
MJPG / NV12 input
```

### 9.5 Optional interop

```text
D3D12-D3D11 shared resource interop
```

## 10. Recommended next steps

```text
1. Windows/MSVCでD3D11 direct camera pathと10-pipeline sampleをbuild/run
2. D3D11 synthetic 60秒 -> 10分soak
3. D3D11実cameraでpool/timestamp tuning
4. D3D12 implementation bodyをV2 pathから物理移動
5. pair timestamp delta diagnostics追加
6. runtime output updateを含む長時間soak
7. device removal / timeout failure tests
8. 必要に応じてformat/interop拡張
```

## 11. Authoritative documents

```text
docs/d3d11/READONLY_PIPELINE.md
docs/d3d11/SYNTHETIC_FRAME_SOURCE.md
samples/MultiPipelineStressD3D11/README.md
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
samples/MultiPipelineStressD3D12/README.md
```
