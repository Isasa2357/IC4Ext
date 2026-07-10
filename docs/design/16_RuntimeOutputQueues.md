# CameraCaptureThread runtime output queues

## Overview

`D3D11CameraCaptureThread` and `D3D12CameraCaptureThread` allow output queues to be added and removed while the worker thread is running.

The existing `addOutputQueue(cameraIndex, queue)` API remains source-compatible. Runtime removal uses the same `cameraIndex` and queue instance that were used for registration.

```cpp
captureThread.start();

captureThread.addOutputQueue(0, outputQueue);

// Later, while capture is still running.
const std::size_t removed =
    captureThread.removeOutputQueue(0, outputQueue);
```

## API

```cpp
void addOutputQueue(std::uint32_t cameraIndex,
                    std::shared_ptr<IndexedFrameQueue> queue);

std::size_t removeOutputQueue(
    std::uint32_t cameraIndex,
    const std::shared_ptr<IndexedFrameQueue>& queue);

std::size_t clearOutputQueues();
std::size_t outputQueueCount() const;
```

`IndexedFrameQueue` is backend-specific:

- D3D11: `D3D11IndexedFrameQueue`
- D3D12: `D3D12IndexedFrameQueue`

`removeOutputQueue()` removes every registration matching both the camera index and the queue instance, and returns the number removed. Passing a null queue or a non-registered binding returns zero.

`clearOutputQueues()` removes all current registrations and returns the number removed. Neither removal API closes a queue or clears frames already stored in it.

## Synchronization semantics

The output binding list is protected by `outputMutex_`. Frame dispatch copies the current binding list while holding the mutex, then releases the mutex before pushing frames.

Consequently:

- Adding and removing queues is safe before or after `start()`.
- Queue push operations do not hold `outputMutex_`.
- A dispatch that already captured its binding snapshot may still deliver the in-flight frame after `removeOutputQueue()` returns.
- The snapshot owns `shared_ptr` references, preventing queue lifetime races during an in-flight dispatch.

## Tests

The following no-camera tests use `DummyCameraCapture` sources:

```text
test_d3d11_camera_capture_thread_outputs
test_d3d12_camera_capture_thread_outputs
```

They cover runtime registration, duplicate binding removal, delivery after registration, delivery stopping after removal, re-registration to a different queue, and clearing all outputs.
