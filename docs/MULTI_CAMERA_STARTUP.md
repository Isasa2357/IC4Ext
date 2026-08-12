# Multi-camera startup helper

IC4Ext provides the same prepare-stop startup helper for the D3D11 and D3D12 ReadOnly backends.

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D12;
```

The helper opens a mixed group of cameras where each camera is selected independently as either:

- a direct `CameraCapture`, returned to the caller for explicit `read()` calls; or
- a `CameraCaptureThread`, started by the helper and connected to a caller-owned output queue.

`FrameSyncThread` is not owned, started, stopped, or configured by this API. A threaded camera's queue may be consumed by the central `FrameSyncThread` or by another caller-owned consumer.

## API

```cpp
Pipe::MultiCameraStartupResult Pipe::OpenAndStartMultiCameraGroup(
    const IC4Ext::D3D12BackendContext& backend,
    const std::vector<Pipe::CameraCaptureStartupConfig>& captureConfigs,
    const std::vector<Pipe::CameraCaptureThreadStartupConfig>& captureThreadConfigs,
    Pipe::MultiCameraStartupOptions options = {});
```

D3D11 exposes the corresponding API and types in `IC4Ext::D3D11` and accepts `IC4Ext::D3D11BackendContext`.

## Startup sequence

The two configuration vectors are merged by `openOrder`. The helper then performs:

```text
for each camera in openOrder:
    force AcquisitionStartMode::Immediate on an internal config copy
    open CameraCapture
    stopAcquisition (prepare-stop)
    retain as CameraCapture, or move into CameraCaptureThread

start every requested CameraCaptureThread worker

for each prepared camera in openOrder:
    startAcquisition
```

On success:

- every returned direct `CameraCapture` is open and acquiring;
- every returned `CameraCaptureThread` has a running worker and active acquisition;
- an external hardware trigger may be enabled after the helper returns.

The caller should prepare and start queue consumers before calling the helper when frames must be accepted immediately after acquisition starts.

## Configuration

A direct camera uses:

```cpp
Pipe::CameraCaptureStartupConfig direct;
direct.cameraId = 0;
direct.selector.deviceIndex = 0;
direct.captureConfig = cameraConfig;
direct.captureOptions = captureOptions;
direct.openOrder = 0;
```

A threaded camera contains the same capture configuration plus thread-specific options and a caller-owned queue:

```cpp
Pipe::CameraCaptureThreadStartupConfig threaded;
threaded.capture.cameraId = 1;
threaded.capture.selector.deviceIndex = 1;
threaded.capture.captureConfig = cameraConfig;
threaded.capture.captureOptions = captureOptions;
threaded.capture.openOrder = 1;
threaded.threadOptions.readTimeoutMs = 1000;
threaded.outputQueue = ingressQueue;
```

`openOrder` is shared across both vectors. Equal values are deterministic: direct configurations retain their vector order first, followed by threaded configurations in their vector order. Use unique values when an exact mixed open order matters.

An optional delay can be inserted after one camera has opened and paused, before the next open:

```cpp
Pipe::MultiCameraStartupOptions options;
options.interCameraOpenDelay = std::chrono::milliseconds(1000);
```

## Mixed example

```cpp
auto ingressQueue =
    std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(queueOptions);

Pipe::CameraCaptureStartupConfig direct;
direct.cameraId = 0;
direct.selector.deviceIndex = 0;
direct.captureConfig = config0;
direct.openOrder = 0;

Pipe::CameraCaptureThreadStartupConfig threaded;
threaded.capture.cameraId = 1;
threaded.capture.selector.deviceIndex = 1;
threaded.capture.captureConfig = config1;
threaded.capture.openOrder = 1;
threaded.outputQueue = ingressQueue;

// Configure/start FrameSyncThread externally when ingressQueue is its input.

auto started = Pipe::OpenAndStartMultiCameraGroup(
    backend,
    {direct},
    {threaded});

if (!started) {
    std::cerr << started.error.where << ": "
              << started.error.message << '\n';
    return false;
}

// Explicit read path for camera 0.
auto frame0 = started.captures[0].capture.read(
    IC4Ext::CameraReadOptions{IC4Ext::ReadMode::NextFrame, 1000});

// Camera 1 is already read by the returned CameraCaptureThread.
auto& camera1Thread = started.captureThreads[0];
```

`CameraCaptureThread` is non-copyable and non-movable, so the result stores threads as `std::unique_ptr<CameraCaptureThread>`.

## Trigger configuration

Trigger properties remain part of each `CameraCaptureConfig`.

```cpp
IC4Ext::ConfigureHardwareTriggerSync(config, "Line1");
```

For hardware trigger operation, do not begin the external trigger signal until `OpenAndStartMultiCameraGroup()` succeeds.

For free-run operation, the helper performs the common Immediate-open / prepare-stop / restart sequence. If a temporary software-trigger gate is required to prevent frames during device preparation, configure and release that gate explicitly in the application; the startup helper does not infer or rewrite trigger policy.

## Failure and rollback

All requests are validated before any camera is opened. Validation rejects:

- an empty request;
- duplicate logical camera IDs across the two vectors;
- invalid capture or thread options;
- a threaded camera with a null output queue; and
- a negative inter-camera delay.

If open, prepare-stop, worker start, or acquisition start fails, the helper:

```text
stops every acquisition whose start was attempted, in reverse order
stops and joins every created CameraCaptureThread, in reverse order
closes every retained direct CameraCapture, in reverse order
returns empty captures and captureThreads vectors with ErrorInfo
```

The error message includes the logical camera ID and the failed startup stage.

## Real-camera correctness test

`test_multi_camera_startup_integration` is a short two-camera correctness test, not a stress test. It starts camera 0 as a direct `CameraCapture` and camera 1 as a `CameraCaptureThread`, receives one valid GPU frame from each path, waits both producer-ready tokens, and shuts the group down.

```bat
ctest --test-dir out\build\multi_camera_startup_d3d12 ^
  -C Debug ^
  --output-on-failure ^
  -R "^test_multi_camera_startup_integration$" ^
  -V
```

The test does not create or use `FrameSyncThread`. Device indices, timeouts, and the optional open delay can be supplied with:

```text
IC4EXT_TEST_DIRECT_DEVICE
IC4EXT_TEST_THREADED_DEVICE
IC4EXT_TEST_READ_TIMEOUT_MS
IC4EXT_TEST_GPU_READY_TIMEOUT_MS
IC4EXT_TEST_INTER_CAMERA_DELAY_MS
```

## Shutdown

The caller owns the returned objects. A safe shutdown sequence is:

```cpp
for (auto& thread : started.captureThreads) {
    thread->stopAcquisition();
}
for (auto& entry : started.captures) {
    entry.capture.stopAcquisition();
}
for (auto& thread : started.captureThreads) {
    thread->stopAndJoin();
}
for (auto& entry : started.captures) {
    entry.capture.close();
}
```

Stop an external hardware trigger source before this shutdown sequence.
