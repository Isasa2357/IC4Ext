# 00. Overall design

IC4Ext は、IC Imaging Control 4 SDK から得た camera frame を Direct3D 11 / Direct3D 12 の GPU texture として扱うための C++17 ライブラリです。

## 目的

- IC4 camera frame を GPU texture として取得する。
- D3D11 / D3D12 backend を同じ設計思想で扱う。
- 通常 pipeline では GPU 上の frame を維持し、保存・検証・OpenCV 連携が必要なときだけ CPU readback する。
- 1 台の物理カメラから複数 camera index を模した DummyCameraCapture を作れるようにする。
- frame number / timestamp を保持し、同期やログに使えるようにする。

## Layer structure

```txt
IC4 camera / QueueSink
  ↓
IC4Ext Core
  - Config
  - CameraPixelFormat / GpuFrameFormat / CpuFrameFormat
  - FrameTiming
  - CpuFrame
  - ErrorInfo
  ↓
Backend layer
  - D3D11CameraCapture
  - D3D12CameraCapture
  - D3D11/D3D12 FrameConverter
  - D3D11/D3D12 FrameReadback
  ↓
Application
  - renderer
  - Varjo / D3D interop layer
  - encoder
  - OpenCV/debug/readback tools
```

## Normal path and readback path

通常処理では CPU frame に戻さず、GPU texture のまま後段へ渡します。

```txt
Normal path:
  IC4 ImageBuffer
    -> D3D11CameraFrame / D3D12CameraFrame
    -> GPU processing / rendering
```

保存、テスト、OpenCV 連携が必要な場合だけ `CpuFrame` へ readback します。

```txt
Readback path:
  D3D11CameraFrame / D3D12CameraFrame
    -> D3D11FrameReadback / D3D12FrameReadback
    -> CpuFrame
    -> PGM/PPM / checksum / OpenCV
```

## Current backend status

| Backend | 状態 |
|---|---|
| D3D11 | 実装済み |
| D3D12 | D3D12Helper 統合版として実装済み |
| D3D12-D3D11 interop | 未実装 |

D3D12 backend は `D3D12BackendContext::FromCore(...)` を使って初期化します。raw `ID3D12Device*` / `ID3D12CommandQueue*` だけを渡す初期化は helper-integrated backend では unsupported です。

## Frame metadata

現状では IC4 `ImageBuffer::metaData()` から取得できる次の値を `FrameTiming` に入れます。

```txt
device_frame_number -> FrameTiming::frameNumber
device_timestamp_ns -> FrameTiming::deviceTimestampNs
host receive time   -> FrameTiming::hostReceivedTime
```

Chunk metadata は未実装です。必要になった場合だけ `CameraFrameMetadata` のような別型を追加する方針です。

## Current format scope

現在は 8bit 系 format を対象にしています。

```txt
Input:
  Mono8
  BayerRG8 / BayerGR8 / BayerGB8 / BayerBG8
  BGR8
  BGRa8

GPU output:
  R8
  RGBA8

CPU readback:
  Gray8
  RGBA8
  RGB8
  BGR8
```

10/12/16bit、packed Bayer、YUV、Polarized format は未実装です。

## Main public classes

```txt
Core:
  CpuFrame
  FrameTiming
  CameraCaptureConfig
  CameraStreamRequest

D3D11:
  D3D11CameraCapture
  D3D11CameraCaptureThread
  D3D11FrameSyncThread
  D3D11DummyCameraCapture
  D3D11DummyCameraCaptureGenerator
  D3D11FrameReadback

D3D12:
  D3D12BackendContext
  D3D12CameraCapture
  D3D12CameraCaptureThread
  D3D12FrameSyncThread
  D3D12DummyCameraCapture
  D3D12DummyCameraCaptureGenerator
  D3D12FrameReadback
```

## Important design rules

- `CpuFrame` は通常 pipeline の中間表現ではなく、readback の出口として扱う。
- GPU backend の出力は原則 `R8` / `RGBA8` とする。
- OpenCV 用の `BGR8` は CPU readback 側で作る。
- `FrameTiming` は D3D11 / D3D12 共通の同期情報として維持する。
- D3D12 の command / fence / resource 管理は D3D12Helper に寄せる。
