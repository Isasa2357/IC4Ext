# MultiPipelineStressD3D12

Two timestamp-synchronized camera capture threads feed one central `FrameSyncThread`. The synchronized immutable frames are fanned out to ten independent processing queues.

| # | Pipeline | Cameras | Queue policy |
|---|---|---|---|
| 1 | Pair display | 0+1 | Latest / DropOldest |
| 2 | Camera 0 display | 0 | Latest / DropOldest |
| 3 | Camera 1 display | 1 | Latest / DropOldest |
| 4 | Pair AVI recording | 0+1 | All frames / RejectNew |
| 5 | Camera 0 AVI recording | 0 | All frames / RejectNew |
| 6 | Camera 1 AVI recording | 1 | All frames / RejectNew |
| 7 | HLSL Sobel compute | 0+1 | All frames / RejectNew |
| 8 | OpenCV Canny | 0 | All frames / RejectNew |
| 9 | OpenCV Sobel magnitude | 1 | All frames / RejectNew |
| 10 | OpenCV edge-overlay pair display | 0+1 | Latest / DropOldest |

Every CPU/display/video pipeline owns independent D3D12 direct queues, command contexts, readback buffer caches, `CpuFrame` objects and OpenCV images. No CPU readback result is shared between pipelines.

The HLSL pipeline reads the shared camera texture as an SRV, writes Sobel output to its own `FramePool`, and retains the source/output handles until its compute fence completes.

## Build

```bat
cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON

cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target MultiPipelineStressD3D12 ^
  --parallel
```

OpenCV must be discoverable by CMake. Set `OpenCV_DIR` when required.

## Run

```bat
MultiPipelineStressD3D12.exe ^
  --device0 0 ^
  --device1 1 ^
  --warmup-sec 5 ^
  --duration-sec 30 ^
  --hardware-trigger ^
  --trigger-source Line1 ^
  --timestamp-source host ^
  --max-diff-us 4000 ^
  --capture-pool-initial 16 ^
  --capture-pool-max 64 ^
  --ingress-queue 128 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --record-fps 160 ^
  --video-codec MJPG ^
  --output-dir stress_output ^
  --csv stress_output\metrics.csv
```

Press Escape or Q in an OpenCV window, or press Ctrl+C, to stop early. Add `--no-display-windows` to run display pipelines without opening GUI windows; they still perform their own readback and image composition.

The process exits with code 0 only when every all-frame pipeline processes every frame dispatched to it without queue rejection, readback failure, processing failure or frame-pool exhaustion. Latest-display drops are expected and are reported separately.
