# 14. Current status and roadmap

このファイルは D3DHelper v1.12.1 対応版の現在地と残タスクを整理します。

## Implemented

### Core

```txt
CameraCaptureConfig
CameraStreamRequest
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
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

### Settings / control

```txt
IC Capture 4 JSON state loading
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
test_d3d11_frame_readback
test_d3d11_dummy_camera_capture
test_d3d11_frame_sync_thread
test_single_camera_smoke
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

### 3. Readback performance optimization

readback API は実装済みですが、高 fps で毎 frame readback する用途向けの resource reuse はまだです。

候補:

```txt
D3D11 staging texture cache
D3D12 readback buffer cache
readback ring
size/format ごとの resource reuse
```

### 4. Long-run / high-fps stress test

基本 smoke/staged test は増やしましたが、長時間・高fps前提の stress test はまだです。

確認したい内容:

```txt
1000 / 10000 frame capture
160fps 相当の連続 capture
DummyCameraCapture 複数出力
FrameSyncThread と readback の併用
stop / restart repeated
queue overflow behavior
setter during acquisition
GPU memory growth
fence wait stall
```

### 5. Real-camera readback integration test

synthetic texture readback test はありますが、実カメラから取得した frame を readback する optional integration test はまだありません。

## Recommended next steps

優先順位:

```txt
1. D3D12-D3D11 interop
2. Long-run / high-fps stress tests
3. Real-camera readback integration test
4. Readback resource reuse optimization
5. Pixel format expansion if needed
```

現在の実装では chunk metadata まで入ったため、次に大きく残っている機能は D3D12-D3D11 interop です。
