# 10. D3D12 backend

このファイルは D3D12Helper 統合版 D3D12 backend の設計メモです。

## Important rule

D3D12 backend は D3D12Helper を前提にします。raw `ID3D12Device*` / `ID3D12CommandQueue*` だけを渡す初期化は unsupported です。

正しい入口:

```cpp
auto core = D3D12CoreLib::D3D12Core::CreateShared();
auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
```

この `backend` を `D3D12CameraCapture::open()`、`D3D12FrameReadback::initialize()` などに渡します。

## D3D12BackendContext

`D3D12BackendContext` は以下を保持します。

```txt
std::shared_ptr<D3D12CoreLib::D3D12Core> core
D3D12CoreLib::D3D12Core* corePtr
D3D12CoreLib::D3D12Queue* queue
ID3D12Device* device
ID3D12CommandQueue* commandQueue
```

`resolve()` は `core` / `queue` から `device` / `commandQueue` を解決します。

## Main classes

```txt
D3D12ReadyToken
D3D12CameraFrame
D3D12FenceManager
D3D12FrameConverter
D3D12FrameCopier
D3D12FrameReadback
D3D12CameraCapture
D3D12CameraCaptureThread
D3D12FrameSyncThread
D3D12DummyCameraCapture
D3D12DummyCameraCaptureGenerator
```

## Frame conversion path

```txt
IC4 ImageBuffer
  ↓ CPU bytes
D3D12 upload buffer
  ↓ compute shader
D3D12 default heap Texture2D
  ↓ D3D12ReadyToken
D3D12CameraFrame
```

`D3D12FrameConverter` は D3D12Helper の command context、upload ring、descriptor allocator、compute pipeline、resource helper を使います。

## Supported conversion

```txt
Mono8  -> R8
Mono8  -> RGBA8
Bayer*8 -> RGBA8
BGR8   -> RGBA8
BGRa8  -> RGBA8
```

## Readback

`D3D12FrameReadback` は `D3D12CameraFrame` の ready token を待ち、copy 用 command を発行し、readback buffer から `CpuFrame` へ tight packed に詰め直します。

```cpp
IC4Ext::D3D12FrameReadback readback;
readback.initialize(backend);

IC4Ext::CpuFrame cpu;
readback.readback(frame, IC4Ext::CpuFrameFormat::BGR8, cpu);
```

## Synchronization

`D3D12FrameSyncThread` は GPU command を発行しません。入力 frame を受け取り、`FrameTiming` に基づいて grouping し、ready token はそのまま保持します。consumer は texture 使用前に `frame.ready.wait()` を呼びます。

## Current limitations

- D3D12-D3D11 shared texture interop は未実装です。
- 10/12/16bit pixel format は未実装です。
- readback resource reuse / ring 化は未実装です。
- 高 fps 長時間の実機 stress test は今後の確認事項です。
