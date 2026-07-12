# IC4Ext v2.0.0 D3D12 foundation

This branch introduces the v2 D3D12 API alongside the existing v1 API so that the migration remains buildable while applications move to immutable pooled frames.

## Implemented

- `IC4Ext::V2::D3D12ReadOnlyFrame`
  - shared immutable frame handle
  - producer-ready fence token
  - SRV handles, published resource state and metadata
- `IC4Ext::V2::D3D12FramePool`
  - CameraCapture-owned reusable texture pool
  - bounded dynamic growth
  - `DropNewest` and timed-wait exhaustion policies
  - move-only writable lease and one-way `publish()` transition
- `IC4Ext::V2::D3D12PooledFrameConverter`
  - reuses the v1 converter shader pipelines, upload rings and command slots
  - writes directly into a frame-pool lease
  - copies the pool UAV descriptor into the converter's per-slot descriptor heap
  - publishes the completed texture as a read-only frame after queue signaling
- `IC4Ext::V2::D3D12CameraCapture`
  - owns the pooled producer path
  - opens IC4 devices with the existing `CameraCaptureConfig`
  - lazily creates or replaces the frame pool from the negotiated frame shape
  - returns `D3D12ReadOnlyFrame` from `read()`
  - preserves hardware/software trigger and property control APIs
  - exposes capture and frame-pool statistics
- `IC4Ext::V2::D3D12CameraCaptureThread`
  - continuously reads `D3D12ReadOnlyFrame`
  - submits one shared handle to the central sync ingress queue
  - has no physical GPU-copy fan-out path
  - supports runtime replacement of the central ingress queue
- `IC4Ext::V2::D3D12ReadOnlyFrameSet`
  - immutable selected-camera frame set
- `IC4Ext::V2::D3D12FrameSyncThread`
  - one complete synchronized set across all registered cameras
  - runtime output registration/update/replacement/removal
  - required-camera selection per output
  - `Maximum` or fixed FPS rate gate
  - priority-ordered dispatch
  - bounded ThreadKit output queues

## End-to-end v2 D3D12 path

```text
IC4 camera buffer
    -> D3D12CameraCapture
    -> existing UploadRing and compute conversion
    -> CameraCapture-owned D3D12FramePool texture
    -> D3D12ReadOnlyFrame
    -> D3D12CameraCaptureThread
    -> central D3D12FrameSyncThread input queue
    -> complete synchronized set
    -> priority/FPS/required-camera output selection
    -> processing queues
```

No output texture is duplicated by `D3D12CameraCaptureThread` or `D3D12FrameSyncThread`.

## Deliberately retained from v1.x

Dependency versions remain unchanged:

- D3D11Helper `v1.12.1`
- D3D12Helper `v1.12.1`
- ThreadKit `main`
- nlohmann/json `v3.11.3`

The existing v1 D3D11 and D3D12 APIs remain compiled during the migration. New code is temporarily placed in `IC4Ext::V2` and `include/IC4Ext/V2`.

## Lifecycle contract

The owning application must stop and join `D3D12CameraCaptureThread` before closing or destroying its capture. A processing consumer must retain every input `D3D12ReadOnlyFrame` until the consumer's GPU work that reads the frame has completed.

The capture waits for its producer queue to become idle during normal close. Published frames can outlive the capture because they retain the pool state and D3D12 resource references independently.

## Remaining D3D12 work

1. Add a reusable consumer-side GPU lifetime tracker and queue-wait helper.
2. Reuse the pooled converter's per-slot default-heap input buffers instead of rebuilding them for each conversion.
3. Add a real D3D12 device pool/converter test.
4. Add dummy-camera capture-thread-to-sync-thread integration tests.
5. Add real two-camera 160 fps stress and runtime-output-update tests.
6. Add a migration sample using hardware trigger, two capture threads and one central sync thread.
7. Remove the v1 physical-copy fan-out path after migration samples are complete.

## Resource-state contract

A pool entry records the state in which it was last published. A writer exposes both `initialState()` and the requested `writeState()` so the producer can record the required transition before writing. Before `publish()`, the producer must transition the resource to the pool's `publishedState()`.

Published frames are read-only. Consumers may use them as SRV/copy sources without modifying the original resource. A consumer that needs to write must allocate its own destination resource and retain the input frame until its GPU completion fence has passed.
