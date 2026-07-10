# 09 DummyCameraCapture and ControlSink

This document describes the v3 dummy multi-camera simulation extension.

## Goal

When only one physical IC4 camera is available, `D3D11DummyCameraCaptureGenerator` can simulate multiple camera sources. It reads from one real `D3D11CameraCapture`, copies the resulting GPU texture once per dummy output, and publishes the copied frames to `D3D11DummyCameraCapture` frame queues.

## Ownership

`D3D11DummyCameraCaptureGenerator` owns the real `D3D11CameraCapture`. It does not own or keep `D3D11DummyCameraCapture` objects. It only stores weak references to the frame queues created for each dummy camera.

`D3D11DummyCameraCapture` owns its frame queue and a weak reference to an `ID3D11CameraControlSink`. The control sink is owned by the generator. If the generator is destroyed, the control sink expires and dummy setters fail.

## Frame flow

1. Generator reads one source frame from the real camera with `ReadMode::NextFrame`.
2. Generator snapshots all live dummy frame queues.
3. Generator enqueues one D3D11 `CopyResource` per dummy queue.
4. Generator signals one D3D11 fence after all copy commands.
5. Generator assigns the same `D3D11ReadyToken` to all copied frames.
6. Generator pushes frames to dummy queues.

This is the “publish after enqueue copies” design. Consumers call `frame.ready.wait()` when they need to guarantee GPU copy completion.

## Control flow

`D3D11CameraCapture` and `D3D11DummyCameraCapture` both implement `ID3D11Camera`.

Dummy setters such as `setExposureTime`, `setGain`, `setOffset`, `setRoi`, and `setIC4Property` are converted into `CameraControlCommand` values and forwarded to the generator's `ID3D11CameraControlSink`.

If the real camera is already opened, the generator immediately applies the command to its internal `D3D11CameraCapture`. If it is not opened yet, the generator updates the pending `CameraCaptureConfig` so the setting is applied during `open()`.

Because all dummy cameras represent one physical source, camera-control setters are global. Calling `dummy0->setExposureTime(1000.0)` and then `dummy1->setExposureTime(2000.0)` results in the real camera using the last successful value.
