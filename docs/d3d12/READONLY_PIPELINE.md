# IC4Ext D3D12 read-only frame pipeline

This document describes the D3D12 read-only frame pipeline API. Public user code should prefer the semantic namespace and class names under `IC4Ext::D3D12`.

The older `IC4Ext::V2` namespace remains only as a temporary implementation/migration layer while source files are moved. It is not the intended public API surface.

## Public naming

Use these names in new code:

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

namespace D3D12Pipe = IC4Ext::D3D12;

D3D12Pipe::CameraCapture capture;
D3D12Pipe::CameraCaptureThread captureThread(...);
D3D12Pipe::FrameSyncThread syncThread(...);
D3D12Pipe::ReadOnlyFrame frame;
D3D12Pipe::ReadOnlyFrameSet frameSet;
D3D12Pipe::FramePool pool;
D3D12Pipe::FrameWriter writer;
```

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
  - copies the pool UAV descriptor into the converter's per-slot descriptor heap
  - publishes the completed texture as a read-only frame after queue signaling
- `IC4Ext::D3D12::CameraCapture`
  - owns the pooled producer path
  - opens IC4 devices with `CameraCaptureConfig`
  - lazily creates or replaces the frame pool from the negotiated frame shape
  - returns `ReadOnlyFrame` from `read()`
  - preserves hardware/software trigger and property control APIs
  - exposes capture and frame-pool statistics
- `IC4Ext::D3D12::CameraCaptureThread`
  - continuously reads `ReadOnlyFrame`
  - submits one shared handle to the central sync ingress queue
  - has no physical GPU-copy fan-out path
  - supports runtime replacement of the central ingress queue
- `IC4Ext::D3D12::ReadOnlyFrameSet`
  - immutable selected-camera frame set
- `IC4Ext::D3D12::FrameSyncThread`
  - one complete synchronized set across all registered cameras
  - runtime output registration/update/replacement/removal
  - required-camera selection per output
  - `Maximum` or fixed FPS rate gate
  - priority-ordered dispatch
  - bounded ThreadKit output queues
- DXC runtime restore/deployment
  - resolves an explicit `IC4EXT_DXC_RUNTIME_DIR` first
  - searches existing `packages/` and the user NuGet cache
  - restores `Microsoft.Direct3D.DXC` from NuGet when missing
  - copies both `dxcompiler.dll` and `dxil.dll` next to IC4Ext sample/test executables

## End-to-end D3D12 path

```text
IC4 camera buffer
    -> IC4Ext::D3D12::CameraCapture
    -> existing UploadRing and compute conversion
    -> CameraCapture-owned FramePool texture
    -> ReadOnlyFrame
    -> CameraCaptureThread
    -> central FrameSyncThread input queue
    -> complete synchronized set
    -> priority/FPS/required-camera output selection
    -> processing queues
```

No output texture is duplicated by `CameraCaptureThread` or `FrameSyncThread`.

## Dependency policy

Dependency versions remain unchanged from the v1.x branch while this D3D12 read-only pipeline is introduced:

- D3D11Helper `v1.12.1`
- D3D12Helper `v1.12.1`
- ThreadKit `main`
- nlohmann/json `v3.11.3`

Recording via `D3DVideoEncoder` is not enabled by default because its current `main` branch can require D3D12Helper headers newer than the pinned IC4Ext helper dependency.

## DXC runtime behavior

The build defines these cache variables:

```text
IC4EXT_DXC_RUNTIME_DIR     explicit directory containing dxcompiler.dll and dxil.dll
IC4EXT_FETCH_DXC_RUNTIME   ON by default; restores Microsoft.Direct3D.DXC when missing
IC4EXT_DXC_NUGET_PACKAGE   Microsoft.Direct3D.DXC
IC4EXT_DXC_NUGET_VERSION   optional; empty means NuGet resolves the latest package
IC4EXT_DXC_NUGET_ROOT      build-local restore/extract directory
```

Every IC4Ext-created executable target should call `ic4ext_copy_dxc_runtime_to_target(target)`. The helper copies `dxcompiler.dll` and `dxil.dll` to `$<TARGET_FILE_DIR:target>` after build.

## Lifecycle contract

The owning application must stop and join `CameraCaptureThread` before closing or destroying its capture. A processing consumer must retain every input `ReadOnlyFrame` until the consumer's GPU work that reads the frame has completed.

The capture waits for its producer queue to become idle during normal close. Published frames can outlive the capture because they retain the pool state and D3D12 resource references independently.

## Remaining D3D12 work

1. Move implementation files out of the temporary `IC4Ext::V2` namespace into `IC4Ext::D3D12`.
2. Add a reusable consumer-side GPU lifetime tracker and queue-wait helper.
3. Reuse the pooled converter's per-slot default-heap input buffers instead of rebuilding them for each conversion.
4. Add a real D3D12 device pool/converter test.
5. Add dummy-camera capture-thread-to-sync-thread integration tests.
6. Add real two-camera 160 fps stress and runtime-output-update tests.
7. Add a sample using hardware trigger, two capture threads and one central sync thread.
8. Remove the v1 physical-copy fan-out path after migration samples are complete.

## Resource-state contract

A pool entry records the state in which it was last published. A writer exposes both `initialState()` and the requested `writeState()` so the producer can record the required transition before writing. Before `publish()`, the producer must transition the resource to the pool's `publishedState()`.

Published frames are read-only. Consumers may use them as SRV/copy sources without modifying the original resource. A consumer that needs to write must allocate its own destination resource and retain the input frame until its GPU completion fence has passed.
