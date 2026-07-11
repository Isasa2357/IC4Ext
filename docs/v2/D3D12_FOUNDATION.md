# IC4Ext v2.0.0 D3D12 foundation

This branch introduces the v2 D3D12 API alongside the existing v1 API so that the migration can remain buildable while CameraCapture and CameraCaptureThread are moved in later commits.

## Implemented in this foundation

- `IC4Ext::V2::D3D12ReadOnlyFrame`
  - shared immutable frame handle
  - producer-ready fence token
  - SRV handles, published resource state and metadata
- `IC4Ext::V2::D3D12FramePool`
  - CameraCapture-owned reusable texture pool
  - bounded dynamic growth
  - `DropNewest` and timed-wait exhaustion policies
  - move-only writable lease and one-way `publish()` transition
- `IC4Ext::V2::D3D12ReadOnlyFrameSet`
  - immutable selected-camera frame set
- `IC4Ext::V2::D3D12FrameSyncThread`
  - one complete synchronized set across all registered cameras
  - runtime output registration/update/replacement/removal
  - required-camera selection per output
  - `Maximum` or fixed FPS rate gate
  - priority-ordered dispatch
  - bounded ThreadKit output queues

## Deliberately retained from v1.x

Dependency versions remain unchanged:

- D3D11Helper `v1.12.1`
- D3D12Helper `v1.12.1`
- ThreadKit `main`
- nlohmann/json `v3.11.3`

The existing v1 D3D11 and D3D12 APIs remain compiled during the migration. New code is temporarily placed in `IC4Ext::V2` and `include/IC4Ext/V2` until the v2 capture path is complete.

## Next D3D12 implementation steps

1. Change `D3D12FrameConverter` to record into `D3D12FrameWriter` resources instead of allocating a new output texture every frame.
2. Make `D3D12CameraCapture` own and configure `D3D12FramePool`.
3. Return `D3D12ReadOnlyFrame` from the v2 capture interface.
4. Replace CameraCaptureThread physical-copy fan-out with shared read-only handle delivery.
5. Add a real D3D12 device pool test and dummy-camera synchronization tests.
6. Add consumer-side GPU lifetime retention helpers.

## Resource-state contract

A pool entry records the state in which it was last published. A writer exposes both `initialState()` and the requested `writeState()` so the producer can record the required transition before writing. Before `publish()`, the producer must transition the resource to the pool's `publishedState()`.

Published frames are read-only. Consumers may use them as SRV/copy sources without modifying the original resource. A consumer that needs to write must allocate its own destination resource and retain the input frame until its GPU completion fence has passed.
