# 14. Current status and roadmap

このファイルは D3DHelper v1.12.1 対応版の現在地と残タスクを整理します。

## Implemented

### Core

```txt
CameraCaptureConfig
CameraStreamRequest
CameraSyncMode
CameraSyncConfig
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
FrameReadbackCacheStats
IC4StreamStatistics
CameraTimingPerformance
CameraTemperatureReading
CameraPerformanceSnapshot
CpuFrame
ErrorInfo
backend selection macros
```

### D3D11

```txt
D3D11CameraCapture
D3D11CameraCaptureThread
D3D11FrameConverter
D3D11FrameCopier
D3D11FrameSyncThread
D3D11DummyCameraCapture
D3D11DummyCameraCaptureGenerator
D3D11FrameReadback
```

`D3D11CameraCapture::performance()` で IC4 stream statistics、最新 frame timing 由来の fps / jitter / frame number gap、DeviceTemperature を取得できます。

`D3D11FrameReadback` は staging texture を size / format / subresource layout ごとに cache し、同一形状の連続 readback では staging texture を再利用します。`cacheStats()` と `resetCache()` で確認・解放できます。

`D3D11CameraCapture` / `D3D11DummyCameraCapture` / `D3D11CameraCaptureThread` は `softwareTrigger()` を持ちます。

### D3D12

```txt
D3D12BackendContext
D3D12CameraCapture
D3D12CameraCaptureThread
D3D12FrameConverter
D3D12FrameCopier
D3D12FrameSyncThread
D3D12DummyCameraCapture
D3D12DummyCameraCaptureGenerator
D3D12FrameReadback
```

D3D12 backend は D3D12Helper 統合済みです。

`D3D12CameraCapture::performance()` で IC4 stream statistics、最新 frame timing 由来の fps / jitter / frame number gap、DeviceTemperature を取得できます。

`D3D12FrameReadback` は必要サイズ以上の readback buffer を保持し、同一またはより小さい footprint の連続 readback では buffer を再利用します。`cacheStats()` と `resetCache()` で確認・解放できます。

`D3D12CameraCapture` / `D3D12DummyCameraCapture` / `D3D12CameraCaptureThread` は `softwareTrigger()` を持ちます。

### Camera sync configuration

露光タイミング制御のため、`CameraSyncConfig` と以下の helper を追加しています。

```txt
ConfigureNoSync(config)
ConfigureHardwareTriggerSync(config, triggerSource, triggerSelector, triggerActivation)
ConfigureSoftwareTriggerSync(config, triggerSelector, softwareTriggerCommand)
```

これらは `CameraCaptureConfig::propertyOverrides` に IC4 property を materialize します。既存の `open()` は property overrides を `streamSetup(... AcquisitionStart)` より前に適用するため、capture 本体の open sequence を変えずに同期設定を使えます。

代表的な設定:

```txt
NoSync:
  TriggerSelector = FrameStart
  TriggerMode = Off

HardwareTrigger:
  TriggerSelector = FrameStart
  TriggerMode = On
  TriggerSource = Line1 など
  TriggerActivation = RisingEdge
  ExposureAuto = Off
  ExposureTime = optional

SoftwareTrigger:
  TriggerSelector = FrameStart
  TriggerMode = On
  TriggerSource = Software
  ExposureAuto = Off
  ExposureTime = optional
```

`softwareTrigger()` は既定で `TriggerSoftware` command property に `"execute"` を設定します。IC4 の command property は string value `"execute"` で実行できます。機種依存の command 名を使う場合は `softwareTrigger("CustomCommandName")` を呼びます。

ハードウェア同期では、外部 TTL / trigger line / master camera output などで露光開始を揃えます。IC4Ext 側はカメラ property の適用、frame 取得、metadata 記録、取得後の `FrameSyncThread` による frame set 化を担当します。

### Performance snapshot

`CameraPerformanceSnapshot` は以下をまとめて返します。

```txt
CameraCaptureStats
IC4StreamStatistics
CameraTimingPerformance
std::vector<CameraTemperatureReading>
```

`IC4StreamStatistics` は IC4 SDK の `Grabber::streamStatistics()` から取得します。

```txt
deviceDelivered
deviceTransmissionError
deviceTransformUnderrun
deviceUnderrun
transformDelivered
transformUnderrun
sinkDelivered
sinkUnderrun
sinkIgnored
```

`CameraTimingPerformance` は `FrameTiming` から計算します。

```txt
deviceFrameIntervalNs / deviceFps / deviceJitterNs
hostReceiveIntervalNs / hostReceiveFps / hostJitterNs
frameNumberGap / estimatedDroppedFrames / accumulatedEstimatedDroppedFrames
```

温度は `DeviceTemperatureSelector` がある場合は selector ごとに `DeviceTemperature` を読みます。selector がない場合は `DeviceTemperature` を直接読みます。温度 property がない機種では `temperatures` は空になります。

### Chunk metadata

IC4 の image buffer に含まれる chunk data を `PropertyMap::connectChunkData()` 経由で読み取り、`FrameChunkMetadata` として D3D11 / D3D12 frame および readback 後の `CpuFrame` に保持します。

対応済みフィールド:

```txt
ChunkBlockId
ChunkExposureTime
ChunkGain
ChunkIMX174FrameId
ChunkIMX174FrameSet
ChunkMultiFrameSetId
ChunkMultiFrameSetFrameId
```

chunk が無効、またはカメラが対象 chunk を持たない場合は、各 `has*` flag が false のままになります。

### Stress / integration tests

以下を追加済みです。

```txt
test_no_camera_pipeline_stress
  - synthetic D3D11 pass-through stress
  - synthetic D3D12 FrameNumberExact / TimestampNearest stress
  - D3D12 FrameSyncThread restart stress

test_camera1_long_run_stress
  - real camera 1000 frame capture by default
  - IC4EXT_TEST_STRESS_FRAMES=10000 で 10000 frame 確認
  - IC4EXT_TEST_STRESS_FPS=160 で 160fps 相当の要求
  - stop / restart repeated path
  - performance snapshot output

test_camera1_readback_integration
  - real camera GPU frame -> CpuFrame readback
  - D3D11 staging texture cache hit/miss check
  - D3D12 readback buffer cache hit/miss check
```

### Settings / control

```txt
IC Capture 4 JSON state loading
NoSync / HardwareTrigger / SoftwareTrigger helper
softwareTrigger() command execution
OffsetX / OffsetY explicit setting
ExposureTime / Gain / Gamma / AcquisitionFrameRate setters
ROI setter
generic IC4 property setter
```

### Tests

```txt
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
test_d3d12_core
test_d3d12_shader_reference
test_d3d12_dummy_camera_capture
test_d3d12_frame_readback
test_d3d12_frame_sync_thread
test_d3d12_shader_compile
test_camera2plus_frame_sync_smoke
```

## Not implemented yet

### 1. D3D12-D3D11 interop

未実装です。Varjo や既存 D3D11 renderer と D3D12 capture を接続するなら重要です。

必要になりそうなもの:

```txt
D3D12 shared texture creation
D3D11 OpenSharedResource / OpenSharedResource1
D3D12/D3D11 fence synchronization
resource state management
D3D12FrameToD3D11Texture helper
```

### 2. Pixel format expansion

現状は 8bit 系のみです。

未対応:

```txt
Mono10 / Mono12 / Mono16
Bayer10 / Bayer12 / Bayer16
Bayer10p / Bayer12p
YUV / YCbCr
Polarized formats
MJPG / NV12
```

方針として、必要になったら追加します。最初に追加するなら `Mono16` / `Bayer*16` が安全です。packed format は bit unpack test を先に用意してから追加します。

### 3. Deeper long-run validation

基本的な long-run / high-fps / readback integration test は追加済みですが、より重い検証は今後の確認事項です。

```txt
複数出力 + readback の長時間併用
FrameSyncThread + readback の2台長時間併用
setter during acquisition の連続実行
GPU memory growth の外部監視
fence wait stall の詳細計測
24時間級 soak test
```

## Recommended next steps

優先順位:

```txt
1. D3D12-D3D11 interop
2. Pixel format expansion if needed
3. Deeper long-run validation / soak tests
```

現在の実装では hardware/software trigger helper、performance snapshot、chunk metadata、readback resource reuse、基本 stress / real-camera readback integration test まで入ったため、次に大きく残っている機能は D3D12-D3D11 interop です。
