# Copied output sampling

`D3D12CameraCaptureThread` can throttle copied outputs when multiple output queues are registered.

The last output queue still receives every original camera frame. Earlier queues receive copied frames only inside a sampling window. This is intended for a latency-sensitive primary consumer plus a lower-rate secondary consumer, such as live stereo calibration.

The sampling options are:

```cpp
CameraThreadOptions::copiedOutputFrameStride
CameraThreadOptions::copiedOutputFrameBurst
```

When the camera fps is known, these frame-count values are converted to a common steady-clock time window:

```text
period = copiedOutputFrameStride / fps
window = copiedOutputFrameBurst / fps
```

All camera capture threads use the same process steady-clock epoch, so copied-output bursts line up across cameras. If fps is not known, the implementation falls back to per-thread frame-sequence sampling.

Example for 160 fps:

```text
stride = 32
burst  = 8
period = 200 ms
window = 50 ms
copied-output rate ~= 40 fps per camera
```

This keeps the display/output queue at full frame rate while reducing copied-output load.
