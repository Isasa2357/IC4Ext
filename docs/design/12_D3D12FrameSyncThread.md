# 12 D3D12FrameSyncThread

## Purpose

`D3D12FrameSyncThread` synchronizes multiple `D3D12IndexedCameraFrame` streams into `D3D12SyncedFrameSet` values. It is the D3D12 equivalent of the D3D11 frame sync layer, but the D3D12 implementation supports all current `FrameSyncPolicy` values.

## Input and output

Input:

```cpp
std::shared_ptr<D3D12IndexedFrameQueue>
```

Output:

```cpp
std::shared_ptr<D3D12SyncedFrameQueue>
```

The input queue is expected to receive frames from `D3D12CameraCaptureThread` instances or from `D3D12DummyCameraCaptureGenerator` driven paths. Each input frame has a `cameraIndex` and a `D3D12CameraFrame`.

## Policies

### PassThroughSingleCamera

Only the first `cameraIndices` entry is forwarded. Other cameras are counted as ignored.

### FrameNumberExact

The sync thread buffers frames per required camera and emits a set only when all front frames share the same `FrameTiming::frameNumber`. Stale frames with smaller frame numbers are dropped.

### TimestampNearest

The sync thread buffers frames per required camera and emits a set when the front timestamps are within `maxTimestampDiffNs`. The timestamp source is:

1. `FrameTiming::deviceTimestampNs` when non-zero.
2. `FrameTiming::hostReceivedTime` otherwise.

If the front timestamps are too far apart, the oldest front frame is dropped.

## D3D12Helper relationship

`D3D12FrameSyncThread` performs CPU-side queue synchronization only. It does not allocate D3D12 resources, record command lists, transition states, or signal GPU fences. It therefore does not directly use `D3D12CommandContext`, `D3D12UploadRing`, or other D3D12Helper command/resource helpers.

The important D3D12-specific behavior is that `D3D12ReadyToken` is preserved in each frame. Downstream consumers decide when to wait on the ready token before accessing the texture.

## Threading

The worker thread waits on the input queue with a short timeout so that `requestStop()` can terminate without closing the queue. Buffers are owned by the worker thread. Stats are protected by a mutex.

## Tests

`test_d3d12_frame_sync_thread` covers:

- Pass-through single-camera behavior.
- `FrameNumberExact` pairing and stale frame dropping.
- `TimestampNearest` pairing and stale frame dropping.
- Duplicate camera index validation.
- Multi-camera policy validation.
