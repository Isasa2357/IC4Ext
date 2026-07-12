# IC4Ext D3D12 read-only frame pipeline

This document describes the D3D12 read-only frame pipeline API. New D3D12 user code should include the semantic API under `IC4Ext::D3D12`.

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

namespace Pipe = IC4Ext::D3D12;

Pipe::CameraCapture capture;
Pipe::CameraCaptureThread captureThread(...);
Pipe::FrameSyncThread syncThread(...);
Pipe::ReadOnlyFrame frame;
Pipe::ReadOnlyFrameSet frameSet;
Pipe::FramePool pool;
Pipe::FrameWriter writer;
Pipe::ReadOnlyFrameLifetimeTracker lifetimeTracker;
```

## Public implementation status

The D3D12 read-only pipeline is now the D3D12 build path:

- Public API names live under `IC4Ext::D3D12`.
- The old D3D12 physical-copy fan-out sources are no longer built.
- The old D3D12 camera-capture, camera-capture-thread, frame-sync-thread, and frame-copier public headers were removed.
- Transitional wrapper source files under `src/D3D12` currently include the former implementation bodies and compile them into `IC4Ext::D3D12`. The old `include/IC4Ext/V2` and `src/V2` files remain only as implementation source material while the code is physically relocated; they are not public API or CMake build entries.

## Implemented components

- `IC4Ext::D3D12::ReadOnlyFrame`
  - shared immutable frame handle
  - producer-ready fence token
  - SRV handles, published resource state and metadata
- `IC4Ext::D3D12::FramePool`
  - CameraCapture-owned reusable texture pool
  - bounded dynamic growth
  - `DropNewest` and timed-wait exhaustion policies
  - move-only `FrameWriter` lease and one-way `publish()` transition
- `IC4Ext::D3D12::PooledFrameConverter`
  - reuses the existing D3D12 converter shader pipelines, upload rings and command slots
  - writes directly into a frame-pool lease
  - keeps one default-heap input buffer cache per command slot
  - grows a slot buffer only when a larger input is required
  - transitions a reused buffer from shader-read to copy-destination and back
  - exposes allocation/reuse/cache statistics through `PooledFrameConverterStats`
- `IC4Ext::D3D12::CameraCapture`
  - owns the pooled producer path
  - opens IC4 devices with `CameraCaptureConfig`
  - lazily creates or replaces the frame pool from the negotiated frame shape
  - returns `ReadOnlyFrame` from `read()`
  - preserves hardware/software trigger and property control APIs
- `IC4Ext::D3D12::CameraCaptureThread`
  - continuously reads `ReadOnlyFrame`
  - submits one shared handle to the central sync ingress queue
  - has no physical GPU-copy fan-out path
  - accepts an injected `ReadOnlyFrameSource` for camera-free integration tests and custom producers
- `IC4Ext::D3D12::FrameSyncThread`
  - timestamp-nearest matching only
  - runtime output registration/update/replacement/removal
  - required-camera selection per output
  - `Maximum` or fixed FPS rate gate
  - priority-ordered dispatch
- `IC4Ext::D3D12::ReadOnlyFrameLifetimeTracker`
  - retains input `ReadOnlyFrame` handles until a consumer completion fence passes
- `IC4Ext::D3D12::WaitForReadOnlyFrameReadyOnQueue`
  - queues a GPU-side wait from a consumer queue to the producer-ready fence

## Timestamp-only synchronization

D3D12 read-only synchronization intentionally does not support frame-number matching. Independent cameras can expose different device frame-number counter domains even when they are hardware-triggered together, so matching absolute or relative frame numbers is fragile.

`FrameSyncThread` always uses timestamp-nearest matching:

```text
sync timestamp = device timestamp when available, otherwise host-received timestamp
```

This can be configured with:

```text
FrameSyncConfig::timestampSource      Auto | HostReceived | Device
FrameSyncConfig::maxTimestampDiffNs   allowed timestamp difference
```

For hardware-triggered cameras, prefer `timestampSource=host` first to validate host arrival skew, then test `device` if the camera exposes reliable device timestamps.

## End-to-end D3D12 path

```text
IC4 camera buffer
    -> IC4Ext::D3D12::CameraCapture
    -> UploadRing and compute conversion
    -> per-slot reusable default-heap input buffer
    -> CameraCapture-owned FramePool texture
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> central FrameSyncThread input queue
    -> complete synchronized set
    -> priority/FPS/required-camera output selection
    -> processing queues
```

No output texture is duplicated by `CameraCaptureThread` or `FrameSyncThread`.

## Consumer-side GPU lifetime contract

A producer-ready token only says that the camera/producer queue has finished writing the read-only texture. It does not keep the input alive until a downstream consumer queue finishes reading it. Consumers that submit GPU work using a `ReadOnlyFrame` must either hold the frame manually or use `ReadOnlyFrameLifetimeTracker`.

```cpp
namespace Pipe = IC4Ext::D3D12;

const Pipe::ReadOnlyFrame* frame = frameSet.find(0);
Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, *frame);

// Record and submit GPU work that reads frame->resource() as SRV/COPY_SOURCE.
IC4Ext::D3D12ReadyToken consumerDone = SubmitProcessingAndSignalFence();

lifetimeTracker.retainUntil(*frame, consumerDone);
lifetimeTracker.collectCompleted();
```

For a whole synchronized set:

```cpp
lifetimeTracker.retainUntil(frameSet, consumerDone);
```

## Automated D3D12 tests

### `test_d3d12_pooled_converter_device`

Requires a usable D3D12 device but no camera. It performs eight real GPU conversions and validates:

- pool acquire/publish/release
- producer fence completion
- R8 GPU readback contents
- four initial input-buffer allocations for the four converter slots
- four subsequent input-buffer reuses
- cached input-buffer count and byte total

The test returns 77 when no D3D12 device can be created, so CTest reports it as skipped rather than failed.

### `test_d3d12_dummy_capture_sync_integration`

Requires a usable D3D12 device but no physical camera. Two injected dummy `ReadOnlyFrameSource` instances publish deterministic device timestamps through the real:

```text
CameraCaptureThread x2
    -> IndexedReadOnlyFrameQueue
    -> FrameSyncThread
    -> ReadOnlyFrameSetQueue
```

The test validates thread statistics, timestamp matching, synchronized output count, output registration statistics, queue-drop counts, and frame-pool release after consumption.

## Samples

### SingleCameraReadOnlyReadbackD3D12

```bat
SingleCameraReadOnlyReadbackD3D12.exe --device-index 0 --frames 5 --out readonly_frame.ppm
```

### MultiCameraReadOnlySyncD3D12

```bat
MultiCameraReadOnlySyncD3D12.exe --device0 0 --device1 1 --duration-sec 10 --timestamp-source host --max-diff-us 4000
```

For hardware-triggered cameras:

```bat
MultiCameraReadOnlySyncD3D12.exe --device0 0 --device1 1 --hardware-trigger --trigger-source Line1 --timestamp-source host --max-diff-us 4000
```

## Dependency policy

Dependency versions remain unchanged from the v1.x branch:

- D3D11Helper `v1.12.1`
- D3D12Helper `v1.12.1`
- ThreadKit `main`
- nlohmann/json `v3.11.3`

Recording via `D3DVideoEncoder` is not enabled by default because its current `main` branch can require D3D12Helper headers newer than the pinned IC4Ext helper dependency.

## Remaining D3D12 work

1. Physically relocate the remaining implementation bodies from `src/V2` / `include/IC4Ext/V2` into normal `src/D3D12` / `include/IC4Ext/D3D12` files instead of wrapper-including them.
2. Add real two-camera 160 fps long-run stress and runtime-output-update tests.
3. Add explicit device-removal/DRED failure-path tests for the pooled producer path.

## Resource-state contract

A pool entry records the state in which it was last published. A writer exposes both `initialState()` and the requested `writeState()` so the producer can record the required transition before writing. Before `publish()`, the producer must transition the resource to the pool's `publishedState()`.

Published frames are read-only. Consumers may use them as SRV/copy sources without modifying the original resource. A consumer that needs to write must allocate its own destination resource and retain the input frame until its GPU completion fence has passed.
