# D3D12 dynamic output hardware acceptance

この文書は、2台・hardware trigger・160 fpsの実カメラ入力中に、`FrameSyncThread`のoutputを動的に追加・供給停止・closeしても、常設outputと中央pipelineが停止しないことを検証する手順を定義する。

## 1. 対象

テストターゲット:

```text
test_d3d12_dynamic_output_hardware_acceptance
```

対象トポロジ:

```text
CameraCaptureThread 0 ----+
CameraCaptureThread 1 ----+--> one ingress queue
                                  |
                                  v
                           one FrameSyncThread
                             |             |
                             v             v
                       permanent A     dynamic B
                       always active    add/stop/close repeatedly
```

このテストはv1で観測された「outputの追加・削除タイミングによってアプリケーション全体または他outputまで停止する」種類の問題を、v2の二段階output lifecycleに対して直接検証する。

## 2. 検証するライフサイクル

動的output Bは次を繰り返す。

```text
registerOutput(B)
    ↓
0 / 1 / 2 / 4 setsのいずれかまで待つ
    ↓
stopOutputSupply(B)
    ↓
常設Aをさらに数set流す
    ↓
BのemittedSetsとqueue sizeが増えていないことを確認
    ↓
偶数cycle: close -> drain
奇数cycle: clear -> close
```

stop timingを4通りに変えることで、register直後、dispatch直前・直後、backlogありの境界を繰り返し通す。

`stopOutputSupply()`が戻った後、Bへのlate pushは1件でも失敗とする。

## 3. 常設output A

Aは試験中ずっとactiveであり、専用consumer threadがFIFOで継続的に受信する。

確認内容:

```text
ReadOnlyFrameSetがcamera 0とcamera 1を含む
GPU resource / SRVが有効
syncGroupIdが単調増加
camera 0 / 1 frameNumberが単調増加
output queue drop == 0
dispatch error == 0
closed queue push == 0
```

Bを追加・停止・closeしている間も、Aの配送rateが`expected fps x minimum rate ratio`以上であることを要求する。

標準では160 x 0.95 = 152 fps以上を要求する。

## 4. その他の合格条件

測定区間で次を要求する。

```text
application crash                    0
FrameSyncThread unexpected stop      0
CameraCaptureThread unexpected stop  0
camera read errors                   0
camera read timeouts                 0
camera -> ingress push failures      0
ingress DropOldest                   0
FramePool exhaustionDrops            0
FramePool waitTimeouts               0
sync drop ratio                      <= 0.001
permanent A output drops             0
permanent A dispatch errors          0
permanent A closed queue pushes      0
dynamic B late push after stop       0
AcquisitionStop failure              0
```

## 5. ビルド

PR #14のbranchへ切り替える。

```bat
git fetch --prune origin

git switch -C agent/two-stage-output-retirement ^
  origin/agent/two-stage-output-retirement

git branch --set-upstream-to=origin/agent/two-stage-output-retirement

git status -sb
git log -1 --oneline
```

Release build:

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "IC4PATH=%IC4_SDK_ROOT%"

if exist out\build\dynamic_output_hw_d3d12 (
    rmdir /s /q out\build\dynamic_output_hw_d3d12
)

cmake -S . -B out\build\dynamic_output_hw_d3d12 ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DIC4_SDK_ROOT="%IC4_SDK_ROOT%" ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON ^
  -DIC4EXT_BUILD_SAMPLES=OFF ^
  -DIC4EXT_BUILD_TESTS=ON ^
  -DIC4EXT_FETCH_DXC_RUNTIME=ON

cmake --build out\build\dynamic_output_hw_d3d12 ^
  --config Release ^
  --target ^
    test_d3d12_dynamic_output_lifecycle ^
    test_d3d12_dynamic_output_hardware_acceptance ^
  --parallel
```

まずcamera-free lifecycle testも再実行する。

```bat
ctest --test-dir out\build\dynamic_output_hw_d3d12 ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d12_dynamic_output_lifecycle$"
```

## 6. 160 fps実機設定

外部trigger generatorはテスト開始時に停止する。

```bat
set "IC4EXT_TEST_ENABLE_DYNAMIC_OUTPUT_ACCEPTANCE=1"

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
set "IC4EXT_TEST_DYNAMIC_OUTPUT_CYCLES=200"
set "IC4EXT_TEST_DYNAMIC_OUTPUT_POST_STOP_SETS=4"
set "IC4EXT_TEST_DYNAMIC_OUTPUT_TIMEOUT_SECONDS=180"
set "IC4EXT_TEST_DYNAMIC_OUTPUT_QUEUE_CAPACITY=32"

set "IC4EXT_TEST_MIN_RATE_RATIO=0.95"
set "IC4EXT_TEST_MAX_SYNC_DROP_RATIO=0.001"
set "IC4EXT_TEST_FRAME_POOL_INITIAL=64"
set "IC4EXT_TEST_FRAME_POOL_MAX=256"
set "IC4EXT_TEST_SYNC_INPUT_QUEUE_CAPACITY=512"
set "IC4EXT_TEST_SYNC_OUTPUT_QUEUE_CAPACITY=512"
set "IC4EXT_TEST_READ_TIMEOUT_MS=1000"
set "IC4EXT_TEST_INTER_CAMERA_DELAY_MS=1000"
```

JSONが不要なら空にする。

```bat
set "IC4EXT_TEST_IC4_JSON="
set "IC4EXT_TEST_IC4_JSON_0="
set "IC4EXT_TEST_IC4_JSON_1="
set "IC4EXT_TEST_IC4_JSON_DEVICE_INDEX="
set "IC4EXT_TEST_IC4_JSON_DEVICE_INDEX_0="
set "IC4EXT_TEST_IC4_JSON_DEVICE_INDEX_1="
```

## 7. 実行

```bat
ctest --test-dir out\build\dynamic_output_hw_d3d12 ^
  -C Release ^
  --output-on-failure ^
  -V ^
  -R "^test_d3d12_dynamic_output_hardware_acceptance$"
```

次が表示されたら外部trigger generatorを160 Hzで開始する。

```text
[dynamic-output-acceptance] cameras are armed. Start the external hardware trigger now
```

25 cycleごとに途中経過を表示する。

```text
[dynamic-output-acceptance] cycles=25/200 permanentSets=... dynamicEmittedBeforeStop=... drained=... discarded=...
```

成功時の最終出力:

```text
[dynamic-output-acceptance-stats] permanentSets=... elapsedSec=... permanentFps=... camera0Read=... camera1Read=... camera0Timeouts=0 camera1Timeouts=0 syncInput=... syncDropped=... syncIncomplete=... permanentDrops=0 pool0Exhaustion=0 pool1Exhaustion=0
[dynamic-output-acceptance-result] cycles=200 permanentSets=... dynamicEmittedBeforeStop=... drained=... discarded=... permanentDrops=0 latePushes=0 syncRunningThroughChurn=1
test_d3d12_dynamic_output_hardware_acceptance passed
```

## 8. 段階的な実行

最初は50 cycleで確認してもよい。

```bat
set "IC4EXT_TEST_DYNAMIC_OUTPUT_CYCLES=50"
```

50 cycleが通ったら200 cycleへ戻す。

```bat
set "IC4EXT_TEST_DYNAMIC_OUTPUT_CYCLES=200"
```

200 cycleで問題がなければ、必要に応じて1000 cycleまで上げる。

```bat
set "IC4EXT_TEST_DYNAMIC_OUTPUT_CYCLES=1000"
set "IC4EXT_TEST_DYNAMIC_OUTPUT_TIMEOUT_SECONDS=600"
```

この不具合クラスは経過時間より状態遷移回数の方が重要なため、固定outputの30分試験とは別のacceptanceとして扱う。
