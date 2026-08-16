# D3D12 v2 multi-camera pipeline acceptance

この文書は、IC4Ext v2の正式なD3D12多入力・多出力パイプラインを、2台の実カメラで検証する手順を定義する。

対象トポロジは次である。

```text
OpenAndStartMultiCameraGroup
    -> CameraCaptureThread(cameraId=0) --+
    -> CameraCaptureThread(cameraId=1) --+--> one IndexedReadOnlyFrameQueue
                                              -> one FrameSyncThread
                                              -> one ReadOnlyFrameSetQueue
                                              -> acceptance consumer
```

このテストではdirect `CameraCapture`を返さない。2台とも`CameraCaptureThread`として起動し、同じingress queueへshared `ReadOnlyFrame`を提出する。`FrameSyncThread`は完全同期setを作り、同じGPU textureへのReadOnly handleをoutput queueへ渡す。

動的outputのadd / `stopOutputSupply()` / drain-or-clear / `closeOutputChannel()`を160 fps中に反復する実機試験は、`DYNAMIC_OUTPUT_ACCEPTANCE.md`を参照する。

## 1. テストターゲット

ビルド対象:

```text
test_d3d12_multi_camera_pipeline_acceptance
```

同じ実行ファイルを3つのCTest modeで使用する。

```text
test_d3d12_multi_camera_pipeline_e2e
test_d3d12_hardware_trigger_pipeline_smoke
test_d3d12_160fps_long_run_acceptance
```

`hardware_trigger`と`long_run`は外部機器を必要とするmanual testである。明示的なenable環境変数がなければreturn code 77でskipする。

動的output用の独立ターゲット:

```text
test_d3d12_dynamic_output_hardware_acceptance
```

こちらもmanual testであり、`IC4EXT_TEST_ENABLE_DYNAMIC_OUTPUT_ACCEPTANCE=1`が必要である。

## 2. 共通ビルド

160 fps acceptanceを含むため、実機試験はRelease構成で統一する。

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"

cmake -S . -B out\build\v2_pipeline_acceptance ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4_SDK_ROOT="%IC4_SDK_ROOT%" ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=OFF ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON

cmake --build out\build\v2_pipeline_acceptance ^
  --config Release ^
  --target test_d3d12_multi_camera_pipeline_acceptance ^
  --parallel
```

## 3. Free-run end-to-end test

目的:

```text
2 x CameraCaptureThread
    -> FrameSyncThread
    -> ReadOnlyFrameSet
```

という現行v2経路が、起動helperを含めて最後まで動作することを確認する。

標準設定:

```text
30 fps request
20 warmup sets
100 measured sets
20 ms host timestamp tolerance
```

free-runでは2台のcamera phaseが独立しているため、CTest登録時に20 ms toleranceを明示する。hardware-trigger modeの4 ms toleranceとは分けて扱う。

実行:

```bat
set "IC4EXT_TEST_CAMERA0_DEVICE=0"
set "IC4EXT_TEST_CAMERA1_DEVICE=1"
set "IC4EXT_TEST_FORMAT=BGR8"
set "IC4EXT_TEST_FPS=30"

ctest --test-dir out\build\v2_pipeline_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d12_multi_camera_pipeline_e2e$"
```

このmodeでは`ConfigureNoSync()`を明示的に適用し、両カメラの`TriggerMode`をOffにする。

## 4. Hardware-trigger startup smoke test

### 4.1 配線と外部trigger

- 両カメラの同じtrigger inputへ同一信号を配線する。
- default sourceは`Line1`。
- test開始時点では外部trigger generatorを停止しておく。
- helperが2台をopen、prepare-stop、worker start、AcquisitionStartした後に、consoleへ`cameras are armed`と表示される。
- 表示後すぐに外部trigger generatorを開始する。
- `IC4EXT_TEST_TRIGGER_ARM_DELAY_MS=1`は通知を表示させるためだけの最小待機であり、trigger開始前にoutput queueを蓄積させない。

### 4.2 実行例

```bat
set "IC4EXT_TEST_ENABLE_HW_ACCEPTANCE=1"
set "IC4EXT_TEST_CAMERA0_DEVICE=0"
set "IC4EXT_TEST_CAMERA1_DEVICE=1"
set "IC4EXT_TEST_TRIGGER_SOURCE=Line1"
set "IC4EXT_TEST_TRIGGER_ARM_DELAY_MS=1"
set "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS=60"
set "IC4EXT_TEST_FPS=160"
set "IC4EXT_TEST_EXPECTED_FPS=160"
set "IC4EXT_TEST_FORMAT=BayerRG8"
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=4000000"
set "IC4EXT_TEST_WARMUP_SETS=100"
set "IC4EXT_TEST_SYNC_SETS=1000"

ctest --test-dir out\build\v2_pipeline_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d12_hardware_trigger_pipeline_smoke$"
```

`cameras are armed`が表示されたら、外部trigger generatorを直ちに開始する。collection loopはそのまま待機しており、defaultより長い60秒のstartup timeout内に最初の同期setが届けばよい。

このmodeは、次を確認する。

```text
ConfigureHardwareTriggerSync(Line1)
prepare-stop後に2 workerを起動
全Acquisition開始後に外部triggerを開始
100 warmup sets
1000 synchronized sets
host timestamp pair difference <= 4 ms
GPU-ready completion
AcquisitionStop / worker join / FrameSyncThread stop
```

## 5. Two-camera 160 fps long-run acceptance

long-run modeはhardware triggerを前提とする。

標準acceptance設定:

```text
requested / expected fps       160
measurement duration           1800 s (30 min)
warmup                         500 sets
minimum synchronized rate      expected fps x 0.95 = 152 fps
maximum sync drop ratio        0.001 (0.1%)
host timestamp tolerance       4 ms
FramePool initial / max        64 / 256
```

まず60秒程度のdry-runを行い、その後30分の正式試験を実行することを推奨する。

### 5.1 60秒dry-run

```bat
set "IC4EXT_TEST_ENABLE_LONG_RUN_ACCEPTANCE=1"
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
set "IC4EXT_TEST_MIN_RATE_RATIO=0.95"
set "IC4EXT_TEST_MAX_SYNC_DROP_RATIO=0.001"
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=4000000"
set "IC4EXT_TEST_FRAME_POOL_INITIAL=64"
set "IC4EXT_TEST_FRAME_POOL_MAX=256"

ctest --test-dir out\build\v2_pipeline_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d12_160fps_long_run_acceptance$"
```

`cameras are armed`表示後に外部trigger generatorを開始する。

### 5.2 30分acceptance

```bat
set "IC4EXT_TEST_ACCEPTANCE_SECONDS=1800"

ctest --test-dir out\build\v2_pipeline_acceptance ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d12_160fps_long_run_acceptance$"
```

各実行前に外部trigger generatorを停止状態へ戻し、`cameras are armed`表示後に再度開始する。

カメラ固有の160 fps設定にIC Capture JSONが必要な場合は、`IC4EXT_TEST_IC4_JSON_0`と`IC4EXT_TEST_IC4_JSON_1`を設定する。明示的なWidth、Height、PixelFormat、FPSはJSON適用後のtest設定と整合させる。

## 6. Dynamic output hardware acceptance

固定outputの長時間試験とは別に、常設output Aを動作させたまま動的output Bを繰り返し追加・停止・closeする。

```text
register B
stop timingを0 / 1 / 2 / 4 queued setsで変化
stopOutputSupply B
Aをさらに数set流す
Bへのlate pushがないことを確認
drainまたはclear
closeOutputChannel B
```

標準は200 cycleである。詳細なコマンドと合格条件は`DYNAMIC_OUTPUT_ACCEPTANCE.md`を参照する。

## 7. Acceptance criteria

固定outputの全modeで次を要求する。

```text
startup result contains 0 direct captures and exactly 2 running capture threads
logical camera IDs are 0 and 1
complete ReadOnlyFrameSet contains exactly camera 0 and camera 1
both frames have GPU resource and SRV
both producer-ready waits complete
width and height are positive
frame numbers increase monotonically
syncGroupId increases monotonically
host timestamp difference stays within tolerance
camera read errors == 0
camera-to-ingress push failures == 0
ingress DropOldest events == 0
FramePool exhaustionDrops == 0
FramePool waitTimeouts == 0
FrameSync output queueDrops == 0
AcquisitionStop succeeds for both cameras
```

long-run modeでは追加で次を要求する。

```text
camera read timeouts == 0 during measured interval
observed synchronized FPS >= expected FPS x minimum rate ratio
FrameSync droppedFrames / inputFrames <= maximum sync drop ratio
camera read-count imbalance <= 1%
```

warmup完了時のstatsをbaselineとし、外部trigger開始前やstartup中のtimeout/dropはlong-run measured intervalへ含めない。

## 8. 主な環境変数

| Variable | Meaning | Default |
|---|---|---:|
| `IC4EXT_TEST_PIPELINE_MODE` | `free-run`, `hardware-trigger`, `long-run-160fps` | `free-run` |
| `IC4EXT_TEST_CAMERA0_DEVICE` | camera 0の列挙index | 0 |
| `IC4EXT_TEST_CAMERA1_DEVICE` | camera 1の列挙index | 1 |
| `IC4EXT_TEST_TRIGGER_SOURCE` | hardware trigger source | `Line1` |
| `IC4EXT_TEST_TRIGGER_ARM_DELAY_MS` | helper完了後の通知用待機 | 0 |
| `IC4EXT_TEST_FPS` | camera stream request FPS | mode依存 |
| `IC4EXT_TEST_EXPECTED_FPS` | acceptanceで期待する同期FPS | mode依存 |
| `IC4EXT_TEST_WARMUP_SETS` | stats baseline前の同期set数 | mode依存 |
| `IC4EXT_TEST_SYNC_SETS` | free-run / hardware smokeの測定set数 | mode依存 |
| `IC4EXT_TEST_ACCEPTANCE_SECONDS` | long-run測定秒数 | 1800 |
| `IC4EXT_TEST_MIN_RATE_RATIO` | long-run最小rate比 | 0.95 |
| `IC4EXT_TEST_MAX_SYNC_DROP_RATIO` | long-run最大drop比 | 0.001 |
| `IC4EXT_TEST_SYNC_TOLERANCE_NS` | host timestamp pair tolerance | mode依存 |
| `IC4EXT_TEST_FRAME_POOL_INITIAL` | producer FramePool初期容量 | 64 |
| `IC4EXT_TEST_FRAME_POOL_MAX` | producer FramePool最大容量 | 256 |
| `IC4EXT_TEST_ENABLE_HW_ACCEPTANCE` | hardware smokeを有効化 | 0 |
| `IC4EXT_TEST_ENABLE_LONG_RUN_ACCEPTANCE` | long-runを有効化 | 0 |
| `IC4EXT_TEST_ENABLE_DYNAMIC_OUTPUT_ACCEPTANCE` | dynamic output実機試験を有効化 | 0 |
| `IC4EXT_TEST_DYNAMIC_OUTPUT_CYCLES` | add/stop/close反復回数 | 200 |

## 9. 結果出力

固定output成功時には次の形式でsummaryを出す。

```text
[v2-acceptance-result] mode=long-run-160fps sets=... elapsedSec=... syncFps=... expectedFps=160.000 minRateRatio=0.950 maxPairDiffUs=... meanPairDiffUs=... camera0Read=... camera1Read=... camera0Timeouts=0 camera1Timeouts=0 syncInput=... syncDropped=... syncIncomplete=... outputDrops=0 pool0Exhaustion=0 pool1Exhaustion=0
```

途中経過は5秒ごとに表示する。失敗時は最初に違反したacceptance criterionを`[v2-acceptance] FAILED:`として出力する。
