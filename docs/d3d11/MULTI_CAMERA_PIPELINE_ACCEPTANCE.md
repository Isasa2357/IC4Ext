# D3D11 v2 multi-camera pipeline acceptance

この文書は、IC4Ext v2のD3D11 ReadOnly multi-camera pipelineを2台の実カメラで検証する手順を定義する。

対象トポロジは次である。

```text
OpenAndStartMultiCameraGroup
    -> CameraCaptureThread(cameraId=0) --+
    -> CameraCaptureThread(cameraId=1) --+--> one IndexedReadOnlyFrameQueue
                                              -> one FrameSyncThread
                                              -> one ReadOnlyFrameSetQueue
                                              -> acceptance consumer
```

D3D12 acceptanceと同じconsumer-facing contractをD3D11でも検証する。D3D11固有の違いは、同一immediate context上のproducer transactionがbackend context mutexで直列化される点である。

## 1. Test target

Build target:

```text
test_d3d11_multi_camera_pipeline_acceptance
```

同じexecutableを3つのCTest modeで使用する。

```text
test_d3d11_multi_camera_pipeline_e2e
test_d3d11_hardware_trigger_pipeline_smoke
test_d3d11_160fps_long_run_acceptance
```

hardware-triggerとlong-runはmanual testであり、明示的なenable環境変数がなければreturn code 77でskipする。

## 2. Common build

160 fps acceptanceを含むためRelease構成で統一する。

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"

if exist out\build\d3d11_v2_acceptance (
    rmdir /s /q out\build\d3d11_v2_acceptance
)

cmake -S . -B out\build\d3d11_v2_acceptance ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4_SDK_ROOT="%IC4_SDK_ROOT%" ^
  -DIC4EXT_ENABLE_D3D11=ON ^
  -DIC4EXT_ENABLE_D3D12=OFF ^
  -DIC4EXT_BUILD_SAMPLES=OFF ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON

cmake --build out\build\d3d11_v2_acceptance ^
  --config Release ^
  --target test_d3d11_multi_camera_pipeline_acceptance ^
  --parallel
```

## 3. Free-run end-to-end

目的は、2 x `CameraCaptureThread -> FrameSyncThread -> ReadOnlyFrameSet`という現行D3D11 v2経路が最後まで動作することの確認である。

標準設定:

```text
30 fps request
20 warmup sets
100 measured sets
20 ms host timestamp tolerance
```

```bat
set "IC4EXT_TEST_CAMERA0_DEVICE=0"
set "IC4EXT_TEST_CAMERA1_DEVICE=1"
set "IC4EXT_TEST_FORMAT=BGR8"
set "IC4EXT_TEST_FPS=30"
set "IC4EXT_TEST_ENABLE_D3D11_HW_ACCEPTANCE="
set "IC4EXT_TEST_ENABLE_D3D11_LONG_RUN_ACCEPTANCE="

ctest --test-dir out\build\d3d11_v2_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d11_multi_camera_pipeline_e2e$"
```

free-runでは`ConfigureNoSync()`を明示し、2台のcamera phaseが独立しているため20 ms toleranceを使用する。

## 4. Hardware-trigger startup smoke

### 4.1 Wiring

- 両cameraの同じtrigger inputへ同一信号を配線する。
- default sourceは`Line1`。
- test開始時点では外部trigger generatorを停止する。
- `cameras are armed`表示後にgeneratorを160 Hzで開始する。

### 4.2 Command

```bat
set "IC4EXT_TEST_ENABLE_D3D11_HW_ACCEPTANCE=1"
set "IC4EXT_TEST_CAMERA0_DEVICE=0"
set "IC4EXT_TEST_CAMERA1_DEVICE=1"
set "IC4EXT_TEST_TRIGGER_SOURCE=Line1"
set "IC4EXT_TEST_TRIGGER_ARM_DELAY_MS=1"
set "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS=60"
set "IC4EXT_TEST_FORMAT=BayerRG8"
set "IC4EXT_TEST_WIDTH=1536"
set "IC4EXT_TEST_HEIGHT=1536"
set "IC4EXT_TEST_FPS=160"
set "IC4EXT_TEST_EXPECTED_FPS=160"
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=4000000"
set "IC4EXT_TEST_WARMUP_SETS=100"
set "IC4EXT_TEST_SYNC_SETS=1000"
set "IC4EXT_TEST_FRAME_POOL_INITIAL=64"
set "IC4EXT_TEST_FRAME_POOL_MAX=256"
set "IC4EXT_TEST_SYNC_INPUT_QUEUE_CAPACITY=512"
set "IC4EXT_TEST_SYNC_OUTPUT_QUEUE_CAPACITY=128"
set "IC4EXT_TEST_READ_TIMEOUT_MS=1000"
set "IC4EXT_TEST_GPU_READY_TIMEOUT_MS=5000"
set "IC4EXT_TEST_INTER_CAMERA_DELAY_MS=1000"

ctest --test-dir out\build\d3d11_v2_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d11_hardware_trigger_pipeline_smoke$"
```

`cameras are armed`表示後に外部trigger generatorを開始する。

## 5. 160 fps long-run acceptance

D3D12と同じ合格基準をD3D11へ適用する。

```text
requested / expected fps       160
measurement duration           1800 s
warmup                         500 sets
minimum synchronized rate      152 fps (160 x 0.95)
maximum sync drop ratio        0.001
host timestamp tolerance       4 ms
FramePool initial / max        64 / 256
```

### 5.1 60-second dry run

```bat
set "IC4EXT_TEST_ENABLE_D3D11_LONG_RUN_ACCEPTANCE=1"
set "IC4EXT_TEST_ENABLE_D3D11_HW_ACCEPTANCE="
set "IC4EXT_TEST_CAMERA0_DEVICE=0"
set "IC4EXT_TEST_CAMERA1_DEVICE=1"
set "IC4EXT_TEST_TRIGGER_SOURCE=Line1"
set "IC4EXT_TEST_TRIGGER_ARM_DELAY_MS=1"
set "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS=60"
set "IC4EXT_TEST_FORMAT=BayerRG8"
set "IC4EXT_TEST_WIDTH=1536"
set "IC4EXT_TEST_HEIGHT=1536"
set "IC4EXT_TEST_FPS=160"
set "IC4EXT_TEST_EXPECTED_FPS=160"
set "IC4EXT_TEST_ACCEPTANCE_SECONDS=60"
set "IC4EXT_TEST_WARMUP_SETS=500"
set "IC4EXT_TEST_MIN_RATE_RATIO=0.95"
set "IC4EXT_TEST_MAX_SYNC_DROP_RATIO=0.001"
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=4000000"
set "IC4EXT_TEST_FRAME_POOL_INITIAL=64"
set "IC4EXT_TEST_FRAME_POOL_MAX=256"
set "IC4EXT_TEST_SYNC_INPUT_QUEUE_CAPACITY=512"
set "IC4EXT_TEST_SYNC_OUTPUT_QUEUE_CAPACITY=128"
set "IC4EXT_TEST_READ_TIMEOUT_MS=1000"
set "IC4EXT_TEST_GPU_READY_TIMEOUT_MS=5000"
set "IC4EXT_TEST_INTER_CAMERA_DELAY_MS=1000"

ctest --test-dir out\build\d3d11_v2_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d11_160fps_long_run_acceptance$"
```

### 5.2 30-minute acceptance

60-second dry runが成功したらgeneratorを停止し、次を設定して再実行する。

```bat
set "IC4EXT_TEST_ACCEPTANCE_SECONDS=1800"

ctest --test-dir out\build\d3d11_v2_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d11_160fps_long_run_acceptance$"
```

各実行で`cameras are armed`表示後にgeneratorを開始する。

## 6. Acceptance criteria

全mode:

```text
startup result contains exactly 2 running CameraCaptureThread instances
logical camera IDs are 0 and 1
complete ReadOnlyFrameSet contains exactly camera 0 and 1
both frames expose D3D11 resource and SRV
both producer-ready waits complete
frame dimensions are positive
frame numbers increase monotonically
syncGroupId increases monotonically
host timestamp difference <= tolerance
camera read errors == 0
camera-to-ingress push failures == 0
ingress DropOldest events == 0
FramePool exhaustionDrops == 0
FramePool waitTimeouts == 0
FrameSync output queueDrops == 0
FrameSync output dispatchErrors == 0
AcquisitionStop succeeds for both cameras
output two-stage retirement succeeds
```

long-run mode additionally requires:

```text
camera read timeouts == 0 during measured interval
observed synchronized FPS >= expected FPS x minimum rate ratio
FrameSync droppedFrames / inputFrames <= maximum sync drop ratio
camera read-count imbalance <= 1%
```

warmup完了時のstatsをbaselineにするため、trigger開始前やstartup中の値はlong-run measured intervalへ含めない。

## 7. Result format

成功時は次のsummaryを出す。

```text
[d3d11-v2-acceptance-result] mode=long-run-160fps sets=... elapsedSec=... syncFps=... expectedFps=160.000 minRateRatio=0.950 maxPairDiffUs=... meanPairDiffUs=... camera0Read=... camera1Read=... camera0Timeouts=0 camera1Timeouts=0 syncInput=... syncDropped=... syncIncomplete=... outputDrops=0 outputErrors=0 pool0Exhaustion=0 pool1Exhaustion=0
```

途中経過は5秒ごとに表示する。
