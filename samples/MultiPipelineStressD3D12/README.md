# MultiPipelineStressD3D12

`MultiPipelineStressD3D12`は、2台のtimestamp同期カメラから生成したReadOnlyフレームを、10個の独立したconsumerへ同時配送する実機ストレスsampleである。

```text
CameraCaptureThread 0 ─┐
CameraCaptureThread 1 ─┴─> FrameSyncThread
                              ├─ latest display x 3
                              ├─ OpenCV recording x 3
                              ├─ HLSL compute x 1
                              ├─ OpenCV all-frame x 2
                              └─ OpenCV processed latest display x 1
```

## 1. Pipeline一覧

| # | Pipeline | Cameras | Queue mode | Workload |
|---:|---|---|---|---|
| 1 | `pair_display` | 0,1 | latest / DropOldest | 2回独立readback、横連結、表示 |
| 2 | `id0_display` | 0 | latest / DropOldest | 独立readback、表示 |
| 3 | `id1_display` | 1 | latest / DropOldest | 独立readback、表示 |
| 4 | `pair_video` | 0,1 | all / RejectNew | 2回独立readback、横連結、AVI保存 |
| 5 | `id0_video` | 0 | all / RejectNew | 独立readback、AVI保存 |
| 6 | `id1_video` | 1 | all / RejectNew | 独立readback、AVI保存 |
| 7 | `hlsl_sobel` | 0,1 | all / RejectNew | HLSL Sobel、専用出力FramePool |
| 8 | `opencv_canny_id0` | 0 | all / RejectNew | 独立readback、GaussianBlur、Canny |
| 9 | `opencv_sobel_id1` | 1 | all / RejectNew | 独立readback、GaussianBlur、Sobel magnitude |
| 10 | `opencv_pair_display` | 0,1 | latest / DropOldest | 2回独立readback、CLAHE、edge overlay、連結表示 |

## 2. Readback分離

CPU/display/video系の各pipelineは、それぞれ独立した以下のリソースを所有する。

```text
D3D12 direct queue
command context
D3D12FrameReadback
readback buffer cache
CpuFrame
cv::Mat
```

別pipelineのreadback結果やCPU画像を共有しない。ペアを扱うpipelineは、camera 0とcamera 1についても別々の`DedicatedReadback`を持つ。

この構成は、GPU-to-CPU転送を実際に複数回発生させるため、通常の最適化済みアプリケーションより意図的に重い。

## 3. Queue方針

### Latest mode

表示系は容量1の`DropOldest` queueと`waitPopLatestFor()`を使う。古い表示frameのdropは正常であり、PASS/FAIL判定では許容される。

### All-frame mode

保存・非表示処理系は`RejectNew` queueを使う。中央`FrameSyncThread`はblockしない。consumerが追いつかずqueueが満杯になると、そのpipelineだけ`dispatchDrop`/`rejectNew`が増え、最終結果はFAILになる。

## 4. Build requirements

- Windows 10/11
- Visual Studio 2022 x64 toolchain
- IC Imaging Control 4 SDK
- D3D12対応GPU
- OpenCV: `core`, `imgproc`, `highgui`, `videoio`
- DXC runtime: `dxcompiler.dll`, `dxil.dll`

IC4Ext本体はOpenCVに依存しない。このsampleだけがOpenCVを要求する。

## 5. OpenCV_DIRの設定例

OpenCV rootが次の場合:

```text
C:\personal\iwatake\library\opencv
```

```bat
set "OPENCV_ROOT=C:\personal\iwatake\library\opencv"
set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc17\lib"
set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc17\bin"
set "PATH=%OPENCV_BIN%;%PATH%"

if not exist "%OpenCV_DIR%\OpenCVConfig.cmake" (
  echo [ERROR] OpenCVConfig.cmake was not found under %OpenCV_DIR%
)
```

配置が異なる場合:

```bat
for /f "delims=" %F in ('dir /s /b "%OPENCV_ROOT%\OpenCVConfig.cmake" 2^>nul') do set "OpenCV_DIR=%~dpF"
echo OpenCV_DIR=%OpenCV_DIR%
```

## 6. Configure and build

失敗してもCMDを閉じない例:

```bat
set "IC4_SDK_ROOT=C:\Users\MiyafujiLab2\AppData\Local\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"
set "OPENCV_ROOT=C:\personal\iwatake\library\opencv"
set "OpenCV_DIR=%OPENCV_ROOT%\build\x64\vc17\lib"
set "OPENCV_BIN=%OPENCV_ROOT%\build\x64\vc17\bin"
set "PATH=%OPENCV_BIN%;%PATH%"
set "IC4EXT_OK=1"

cmake -S . -B out\build\v2_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=ON ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON ^
  -DOpenCV_DIR="%OpenCV_DIR%"

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] CMake configure failed. CMD remains open.

if "%IC4EXT_OK%"=="1" cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target MultiPipelineStressD3D12 ^
  --parallel

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] Build failed. CMD remains open.
if "%IC4EXT_OK%"=="1" echo [OK] MultiPipelineStressD3D12 built.
```

DXC runtimeはCMakeがNuGetから解決し、sample executableと同じdirectoryへ`dxcompiler.dll`と`dxil.dll`をcopyする。

## 7. Executable path

```bat
set "STRESS_EXE="
for /f "delims=" %F in ('dir /s /b out\build\v2_d3d12\MultiPipelineStressD3D12.exe') do set "STRESS_EXE=%F"
echo STRESS_EXE=%STRESS_EXE%
```

## 8. 60秒smoke

pool不足とtimestamp設定を切り分けるため、最初は大きめのpoolを使う。

```bat
"%STRESS_EXE%" ^
  --device0 0 ^
  --device1 1 ^
  --warmup-sec 5 ^
  --duration-sec 60 ^
  --report-ms 5000 ^
  --hardware-trigger ^
  --trigger-source Line1 ^
  --timestamp-source host ^
  --max-diff-us 30000 ^
  --min-sync-fps 50 ^
  --capture-pool-initial 128 ^
  --capture-pool-max 256 ^
  --max-pending-buffers 128 ^
  --ingress-queue 256 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --read-timeout-ms 1000 ^
  --readback-timeout-ms 5000 ^
  --record-fps 160 ^
  --video-codec MJPG ^
  --display-max-width 1280 ^
  --display-max-height 720 ^
  --output-dir stress_output_smoke ^
  --csv stress_output_smoke\metrics.csv
```

`--max-diff-us 30000`は同期経路の動作確認値であり、最終設定ではない。pool exhaustionを解消した後、安定する最小toleranceへ下げる。

## 9. 10分soak

現在の約53 fps baselineに対し最低50 synchronized sets/sを要求する例:

```bat
"%STRESS_EXE%" ^
  --device0 0 ^
  --device1 1 ^
  --warmup-sec 5 ^
  --duration-sec 600 ^
  --report-ms 5000 ^
  --hardware-trigger ^
  --trigger-source Line1 ^
  --timestamp-source host ^
  --max-diff-us 30000 ^
  --min-sync-fps 50 ^
  --capture-pool-initial 128 ^
  --capture-pool-max 256 ^
  --max-pending-buffers 128 ^
  --ingress-queue 256 ^
  --latest-queue 1 ^
  --all-frame-queue 32 ^
  --read-timeout-ms 1000 ^
  --readback-timeout-ms 5000 ^
  --record-fps 160 ^
  --video-codec MJPG ^
  --display-max-width 1280 ^
  --display-max-height 720 ^
  --output-dir stress_output_10min ^
  --csv stress_output_10min\metrics.csv
```

160 fpsを受入条件にする場合は、外部triggerが約160 Hzであることを確認し、例えば次を使う。

```text
--min-sync-fps 150
--min-sync-sets 90000
```

600秒で150 fpsなら必要set数は90000である。

## 10. Headless mode

GUI windowを開かなくても表示pipeline自体のreadback、画像処理、連結は実行する。

```text
--no-display-windows
```

## 11. Camera JSON

カメラごとにJSON stateを指定できる。

```bat
  --ic4-json0 config\camera_state.json ^
  --json-device-index0 0 ^
  --ic4-json1 config\camera_state.json ^
  --json-device-index1 0 ^
```

JSON内の`AcquisitionFrameRate`、露光時間、ROI、PixelFormatが実効fpsへ影響するため、160 fps検証時は内容を確認する。

## 12. Timestamp tuning

`FrameSyncThread`はtimestamp-nearestのみを使う。

```text
160 fps frame period = 6.25 ms
53 fps frame period  = 約18.9 ms
```

30 ms toleranceは1 frame periodより大きく、隣接frameを誤ってpairingする可能性がある。推奨手順:

1. `poolExhaustion == 0`にする。
2. sync-only sampleで入力rateを確認する。
3. 30 msなど大きめの値で経路を確認する。
4. 16 ms、12 ms、8 ms、4 msなどへ徐々に下げる。
5. `syncFps`と`syncDrops`を確認する。
6. 最終的にはpair内timestamp delta分布を計測する。

小さいpoolのままtoleranceを調整すると、pool stallによるhost arrival skewをカメラ同期誤差と誤認する。

## 13. Progress output

進捗ログの先頭にはcapture/sync状態が出る。

```text
sync{input,sets,fps,drops,dropRate}
cam0{read,push,timeout,err,poolPub,poolDrop}
cam1{read,push,timeout,err,poolPub,poolDrop}
```

各pipelineには次が出る。

```text
processed
fps
queue
queueMax
drop
failure
```

## 14. Final outputと合否

### Latest pipeline

次を要求する。

```text
processed > 0
failures == 0
```

`dropOldest`、`popLatestDrop`、`dispatchDrop`は低遅延最新表示のため許容される。

### All-frame pipeline

次を要求する。

```text
processed > 0
failures == 0
dispatchDrop == 0
rejectNew == 0
dropOldest == 0
received == processed
```

### Global sync gate

```text
syncSets >= max(minSyncSets, ceil(minSyncFps * measurementSeconds))
camera read errors == 0
capture pool exhaustion == 0
```

### Exit code

```text
0: overall PASS
1: 初期化、例外、file/codec等の致命的失敗
2: 実行は完了したがstress acceptanceを満たさない
```

## 15. Output files

```text
<output-dir>\pair.avi
<output-dir>\id0.avi
<output-dir>\id1.avi
<csv path>
```

## 16. 現在の予備実測

poolを`initial=128, max=256`へ増やした60秒実行では、次が確認された。

```text
syncFps          53.363
syncDrops        0
camera0Read      3201
camera1Read      3202
camera timeouts  0 / 0
pool exhaustion  0 / 0
```

一方、consumer throughputは次の傾向だった。

```text
HLSL Sobel            約53 fps、dropなし
OpenCV Canny          約43 fps
OpenCV Sobel          約23 fps
single AVI recording  約16-17 fps
pair AVI recording    約7-8 fps
```

この結果から、OpenCV `VideoWriter`経由の保存は高fps全フレーム保存には適さない。GPU textureを直接入力するhardware encoder経路が必要である。

予備実測の詳細、FramePool sizing、timestamp toleranceの注意点は次を参照する。

```text
docs/d3d12/VALIDATION_AND_TUNING.md
```
