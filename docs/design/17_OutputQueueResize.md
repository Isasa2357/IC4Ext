# CameraCaptureThread per-output queue resize

## Overview

`D3D11CameraCaptureThread` and `D3D12CameraCaptureThread` can resize a frame independently for each registered output queue immediately before dispatch.

The existing two-argument registration remains a passthrough path and preserves the original frame size.

```cpp
captureThread.addOutputQueue(0, originalQueue);

captureThread.addOutputQueue(
    1,
    previewQueue,
    IC4Ext::CameraOutputResizeOptions{
        640,
        360,
        IC4Ext::CameraOutputResizeFilter::Linear,
    });
```

## Configuration

```cpp
struct CameraOutputResizeOptions
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CameraOutputResizeFilter filter = CameraOutputResizeFilter::Linear;
};
```

- `width == 0 && height == 0`: passthrough without resize.
- `width > 0 && height > 0`: GPU resize to the requested dimensions.
- Only one dimension being zero is invalid; the queue is not registered.
- `Point` and `Linear` filters are available.

## Dispatch behavior

Resize is performed after `CameraCaptureThread` reads the captured GPU frame and immediately before the selected output queue receives it.

Each resized registration receives its own output texture and ready fence token. A single source frame can therefore be dispatched simultaneously as, for example, original size, 1280x720, and 640x360.

Timing and chunk metadata are preserved. `FrameFormatMetadata::width` and `height` are replaced with the output dimensions.

Passthrough outputs retain the existing copy behavior controlled by `CameraThreadOptions::copyPerOutputQueue`. Resized outputs are independent textures and are still produced when `copyPerOutputQueue` is false.

## Backend implementation

- D3D11 uses `D3D11Helper` v1.13 `D3D11Resizer::DispatchResizeView`.
- D3D12 uses `D3D12Helper` v1.13 `D3D12Resizer::RecordResizeView` with explicit before/after resource states.
- Borrowed source resources are passed through the helpers' non-owning resource-view APIs.
- The current implementation supports `GpuFrameFormat::RGBA8` / `DXGI_FORMAT_R8G8B8A8_UNORM` for resized outputs.
- Passthrough remains available for other output formats.

`CameraThreadOptions::outputProcessingShaderDirectory` may override the helper Processing shader directory. When empty, the helper's default runtime search path is used.

## Statistics

`CameraThreadStats` includes:

```cpp
std::uint64_t resizedFrames;
std::uint64_t resizeFailures;
```

`resizedFrames` counts successfully generated resized output frames. `resizeFailures` counts per-binding resize failures.

## Tests

```text
test_d3d11_camera_capture_thread_resize
test_d3d12_camera_capture_thread_resize
```

Each test dispatches one 4x4 RGBA frame to three queues:

- original 4x4 passthrough
- linear resize to 2x3
- point resize to 3x2

The tests verify texture dimensions, frame metadata, timing/chunk metadata preservation, ready tokens, and resize statistics.
