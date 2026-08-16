# 14. Current status and roadmap

この文書はIC4Ext 2.0.0の実装状態、実機検証結果、未実装項目、優先順位をまとめる。

## 1. Version and public policy

```text
VERSION                 2.0.0
D3D11 public namespace  IC4Ext::D3D11
D3D11 public include    IC4Ext/D3D11/ReadOnlyPipeline.hpp
D3D12 public namespace  IC4Ext::D3D12
D3D12 public include    IC4Ext/D3D12/ReadOnlyPipeline.hpp
compatibility           v1 physical-copy fan-outとの互換性は保証しない
```

D3D11/D3D12とも、capture/sync層が公開する完成GPU frameをReadOnlyへ統一する。動画encoder、consumer固有のpost-process、resource管理はconsumer側の責務である。

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
multi-camera startup helper
```

## 3. D3D11 ReadOnly pipeline

### Producer

```text
D3D11BackendContext
D3D11ReadyToken / D3D11FenceManager
PooledFrameConverter
FramePool / FrameWriter
ReadOnlyFrame
CameraCapture
```

実camera hot pathはIC4 CPU bytesをreusable input bufferから最終CameraCapture-owned FramePool Texture2Dへ直接compute変換する。中間完成Textureと追加`CopyResource`は生成しない。

D3D11 immediate contextのmulti-call transactionはshared recursive mutexで保護する。

### Capture / sync / output lifecycle

```text
CameraCaptureThread
ReadOnlyFrameSource
one central FrameSyncThread
ReadOnlyFrameSet
FrameSyncConfig / FrameSyncOutputConfig / FrameSyncOutputStats
```

- `CameraCaptureThread`は中央ingressへReadOnly handleを1回pushする。
- physical GPU copy fan-outなし。
- timestamp-nearest matchingのみ。
- complete set生成後、outputごとのrequired camerasを選択。
- Maximum / fixed FPS gate。
- priority順dispatch。
- runtime output add/update。
- queue replacementなし。
- `stopOutputSupply()`による同期的no-late-push barrier。
- `closeOutputChannel()`によるqueue closeとregistry解放。
- 1 output faultを他outputと中央workerから分離。

### Consumer lifetime / readback

```text
ReadOnlyFrameLifetimeTracker
D3D11FrameReadback(ReadOnlyFrame overload)
SyntheticFrameSource
```

consumer completion fenceまでinput handleを保持する。複数CPU pipelineは独立staging cacheを持ち、immediate context transactionは共通mutexで直列化する。

## 4. D3D12 ReadOnly pipeline

```text
D3D12BackendContext
D3D12ReadyToken / D3D12FenceManager
PooledFrameConverter
FramePool / FrameWriter
ReadOnlyFrame
CameraCapture
CameraCaptureThread
ReadOnlyFrameSource
central FrameSyncThread
ReadOnlyFrameLifetimeTracker
ReadOnly readback
SyntheticFrameSource
```

D3D11と同じconsumer-facing contractを持つ。D3D12ではresource stateとqueue/fenceが明示的であり、producer-ready、consumer wait、consumer completionを分離する。

runtime output lifecycleもD3D11と共通である。

```text
registerOutput
updateOutput while Active
stopOutputSupply
consumer chooses drain or clear
closeOutputChannel
```

output IDとqueueの対応は不変であり、置換は新outputの追加と旧outputの二段階退役へ分解する。

D3D12 source treeは通常pathへ整理済みである。public headerは`include/IC4Ext/D3D12`、translation unitと実装detailは`src/D3D12`に置く。top-level `include/IC4Ext/V2` / `src/V2`は使用しない。

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

MultiPipelineStress sampleはlatest display、OpenCV recording、HLSL/OpenCV処理など10 outputを同時実行する。OpenCVはsample dependencyでありlibrary本体のdependencyではない。

## 6. Automated tests

### Common

```text
test_core
test_cpu_frame
test_backend_config
test_chunk_metadata
test_multi_camera_startup_core
test_no_camera_pipeline_stress
test_multi_camera_startup_integration
```

### D3D11

```text
test_d3d11_frame_readback
test_d3d11_dummy_camera_capture
test_d3d11_frame_sync_thread
test_d3d11_readonly_pipeline
test_d3d11_multi_camera_startup
test_d3d11_pooled_converter_device
test_d3d11_synthetic_source_sync_integration
test_d3d11_dynamic_output_lifecycle
test_single_camera_smoke
test_camera1_readback_integration
test_camera1_long_run_stress
```

### D3D12

```text
test_d3d12_core
test_d3d12_shader_reference
test_d3d12_readonly_pipeline
test_d3d12_multi_camera_startup
test_d3d12_serial_selection_integration
test_d3d12_pooled_converter_device
test_d3d12_dummy_capture_sync_integration
test_d3d12_synthetic_source_sync_integration
test_d3d12_dynamic_output_lifecycle
test_d3d12_dynamic_output_hardware_acceptance
test_d3d12_shader_compile
test_d3d12_multi_camera_pipeline_e2e
test_d3d12_hardware_trigger_pipeline_smoke
test_d3d12_160fps_long_run_acceptance
```

Dynamic output lifecycle testは、常設output Aを動作させたまま動的output Bを200回add/stop/closeする。stop復帰後のlate pushなし、drain/clearの両方、1 output faultの非波及を確認する。

## 7. Real-camera validation

### Startup and serial selection

D3D11/D3D12のmixed direct/threaded startup helperを2台実機で確認した。D3D12ではserial選択、未知serialのdeviceIndex fallback拒否、direct/threaded frame deliveryも確認した。

### D3D12 v2 fixed-output acceptance

```text
cameras                 2
resolution              1536 x 1536
trigger                 hardware, Line1
expected rate           160 fps
measurement             1800 s
synchronized sets       287,997
observed rate           159.998 fps
camera timeouts         0 / 0
sync drops              0
incomplete sets         0
output queue drops      0
FramePool exhaustion    0 / 0
maximum host pair diff  3.5564 ms
```

固定output構成では30分間の定常安定性を確認済みである。

### D3D12 dynamic-output acceptance

```text
cameras                     2
resolution                  1536 x 1536
trigger                     hardware, 160 Hz
churn cycles                200
permanent synchronized sets 1991
permanent rate              159.988 fps
camera reads                1991 / 1991
late pushes after stop      0
permanent output drops      0
camera timeouts             0 / 0
sync dropped / incomplete   0 / 0
FramePool exhaustion        0 / 0
```

常設output Aを維持したままoutput Bを200回add/stop/closeしても、`stopOutputSupply()`復帰後のlate pushと他output/central workerへの障害波及が発生しないことを確認した。

## 8. Output lifecycle policy

output queue replacementとone-step unregisterは提供しない。

```text
new consumer/queueを準備
registerOutput(new)
stopOutputSupply(old)
old backlogをdrainまたはclear
closeOutputChannel(old)
consumer join
GPU completion wait
resource destroy
```

`stopOutputSupply()`の成功復帰後、対象outputへのpushは実行中でも今後開始されることもない。`closeOutputChannel()`はqueueをcloseするが、既存要素を自動clearしない。

詳細は`docs/OUTPUT_LIFECYCLE.md`を参照する。

## 9. Known tuning issues

### FramePool sizing

多数consumerがReadOnlyFrameSetを保持する構成では、queue backlogとGPU completion待ちを含む十分なpool容量が必要である。D3D12 10-output予備試験ではsmall poolでexhaustionが発生し、pool拡大で解消した。

### Timestamp tolerance

`tolerance > frame period`は隣接frameの誤pairingリスクがある。pool exhaustionとcapture stallを先に解消し、pair delta分布を測定して安定する最小値を選ぶ。

### D3D11 immediate context

安全性のためmulti-call transactionを直列化している。必要になった場合はdeferred context + command-list submissionを次段階の最適化として検討する。

## 10. Not implemented / incomplete

### D3D12 internal normalization

source-tree top-levelのV2 pathは整理済みだが、3つの移動済みimplementation bodyは挙動を変えないため、内部で歴史的な`V2` namespace tokenと旧include名をまだ使用する。これらは`src/D3D12/Detail`内のprivate forwardingで吸収しており、public API/install headerには露出しない。完全なtoken/include正規化は任意の内部cleanupとして分離する。

### Diagnostics

```text
pair delta p50/p95/p99/max
camera別host arrival jitter distribution
calibrated device timestamp offset
sync mismatch reason counters
```

### Failure paths

```text
D3D11/D3D12 device removal
DRED report capture
queue/fence timeout injection
shader load failure injection
pool lifetime assertion
camera disconnect/reconnect
```

### Format extensions

```text
10/12/16bit
packed Bayer
YUV / YCbCr
polarized
MJPG / NV12 input
```

### Optional interop

```text
D3D12-D3D11 shared resource interop
```

## 11. Recommended next steps

```text
1. D3D11実cameraで160 fps hardware-trigger smokeとlong-run acceptance
2. pair timestamp delta diagnostics追加
3. device removal / timeout failure tests
4. 必要に応じてD3D12 Detail内のV2 token/includeを完全正規化
5. 必要に応じてformat/interop拡張
```

## 12. Authoritative documents

```text
docs/OUTPUT_LIFECYCLE.md
docs/V2_PIPELINE_POLICY.md
docs/d3d11/READONLY_PIPELINE.md
docs/d3d11/SYNTHETIC_FRAME_SOURCE.md
samples/MultiPipelineStressD3D11/README.md
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
docs/d3d12/MULTI_CAMERA_PIPELINE_ACCEPTANCE.md
docs/d3d12/DYNAMIC_OUTPUT_ACCEPTANCE.md
samples/MultiPipelineStressD3D12/README.md
```
