# MultiPipelineStressD3D11

`MultiPipelineStressD3D11`は、D3D11 ReadOnly frame pipelineへ10種類のconsumerを同時接続するstress sampleである。実camera modeと、cameraを必要としない`SyntheticFrameSource` modeの両方を持つ。

## Pipelines

| # | name | cameras | queue policy | workload |
|---:|---|---|---|---|
| 1 | `pair_display` | 0,1 | latest / DropOldest | 2画像をreadbackして横連結表示 |
| 2 | `id0_display` | 0 | latest / DropOldest | camera 0表示 |
| 3 | `id1_display` | 1 | latest / DropOldest | camera 1表示 |
| 4 | `pair_video` | 0,1 | all / RejectNew | 2画像を各自readbackして横連結保存 |
| 5 | `id0_video` | 0 | all / RejectNew | camera 0保存 |
| 6 | `id1_video` | 1 | all / RejectNew | camera 1保存 |
| 7 | `hlsl_sobel` | 0,1 | all / RejectNew | D3D11 HLSL Sobel、private output pool |
| 8 | `opencv_canny_id0` | 0 | all / RejectNew | 専用readback後Canny |
| 9 | `opencv_sobel_id1` | 1 | all / RejectNew | 専用readback後Sobel |
| 10 | `opencv_pair_display` | 0,1 | latest / DropOldest | 各自readback、CLAHE/edge overlay、連結表示 |

CPU/display/video consumerはそれぞれ独立した`D3D11FrameReadback`とstaging texture cacheを所有する。D3D11では同一deviceのimmediate contextを共有するため、IC4Extのcontext transaction mutexにより`CopyResource -> Map -> Unmap`をconsumer単位で直列化する。readback結果や`CpuFrame`、`cv::Mat`はconsumer間で共有しない。

## Build

OpenCV rootが次の場合:

```text
C:\personal\iwatake\library\opencv
```

CMD:

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"
set "OPENCV_ROOT=C:\personal\iwatake\library\opencv"
set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc17\lib"
if not exist "%OpenCV_DIR%\OpenCVConfig.cmake" set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc16\lib"
if not exist "%OpenCV_DIR%\OpenCVConfig.cmake" for /f "delims=" %F in ('dir /s /b "%OPENCV_ROOT%\OpenCVConfig.cmake" 2^>nul') do set "OpenCV_DIR=%~dpF"

cmake -S . -B out\build\v2_d3d11 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4_SDK_ROOT="%IC4_SDK_ROOT%" ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=OFF ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DOpenCV_DIR="%OpenCV_DIR%"

cmake --build out\build\v2_d3d11 ^
  --config Release ^
  --target MultiPipelineStressD3D11 ^
  --parallel
```

## Synthetic smoke test

実cameraを使わず、2つのRGBA8 GPU sourceを120 fps、500 us offsetで生成する。

```bat
set "STRESS11_EXE="
for /f "delims=" %F in ('dir /s /b out\build\v2_d3d11\MultiPipelineStressD3D11.exe') do set "STRESS11_EXE=%F"

"%STRESS11_EXE%" ^
  --synthetic ^
  --synthetic-width 1536 ^
  --synthetic-height 1536 ^
  --synthetic-fps 120 ^
  --synthetic-offset-us 500 ^
  --synthetic-pattern hash ^
  --timestamp-source device ^
  --max-diff-us 4000 ^
  --warmup-sec 5 ^
  --duration-sec 60 ^
  --report-ms 5000 ^
  --capture-pool-initial 128 ^
  --capture-pool-max 256 ^
  --ingress-queue 256 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --record-fps 120 ^
  --video-codec MJPG ^
  --output-dir stress_output_d3d11_synthetic ^
  --csv stress_output_d3d11_synthetic\metrics.csv
```

Windowを出さない場合は`--no-display-windows`を追加する。表示pipelineはwindowを生成しない場合も、readback、画像生成、resizeまで実行する。

## Ten-minute synthetic soak

```bat
"%STRESS11_EXE%" ^
  --synthetic ^
  --synthetic-width 1536 ^
  --synthetic-height 1536 ^
  --synthetic-fps 120 ^
  --synthetic-offset-us 500 ^
  --timestamp-source device ^
  --max-diff-us 4000 ^
  --warmup-sec 5 ^
  --duration-sec 600 ^
  --report-ms 5000 ^
  --capture-pool-initial 128 ^
  --capture-pool-max 256 ^
  --ingress-queue 256 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --read-timeout-ms 1000 ^
  --readback-timeout-ms 5000 ^
  --record-fps 120 ^
  --video-codec MJPG ^
  --output-dir stress_output_d3d11_synthetic_10min ^
  --csv stress_output_d3d11_synthetic_10min\metrics.csv ^
  --no-display-windows
```

## Real-camera mode

```bat
"%STRESS11_EXE%" ^
  --device0 0 ^
  --device1 1 ^
  --warmup-sec 5 ^
  --duration-sec 600 ^
  --report-ms 5000 ^
  --hardware-trigger ^
  --trigger-source Line1 ^
  --timestamp-source host ^
  --max-diff-us 30000 ^
  --capture-pool-initial 128 ^
  --capture-pool-max 256 ^
  --max-pending-buffers 128 ^
  --ingress-queue 256 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --readback-timeout-ms 5000 ^
  --record-fps 160 ^
  --video-codec MJPG ^
  --output-dir stress_output_d3d11_camera_10min ^
  --csv stress_output_d3d11_camera_10min\metrics.csv
```

## Result policy

latest pipelineでは`dropOldest`と`droppedByPopLatest`を正常な最新frame選択として許容する。all-frame pipelineでは次を要求する。

```text
worker failures = 0
dispatch queueDrops = 0
RejectNew = 0
DropOldest = 0
received = processed
```

全体ではさらに、minimum sync sets、camera read errors、FramePool exhaustionを検査する。

```text
exit 0: PASS
exit 1: initialization/runtime exception
exit 2: completed, but one or more stress conditions failed
```

OpenCV `VideoWriter`はstress workloadであり、IC4Ext library本体の責務ではない。高fps保存性能の保証にも使用しない。
