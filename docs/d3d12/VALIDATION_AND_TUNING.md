# D3D12 ReadOnly パイプラインの検証・チューニングガイド

この文書は、IC4Ext 2.0.0 の D3D12 ReadOnly パイプラインを実カメラで評価するときの手順、指標、合否判定、既知の実測結果をまとめる。

対象は主に次の経路である。

```text
CameraCapture x 2
    -> CameraCaptureThread x 2
    -> IndexedReadOnlyFrameQueue
    -> FrameSyncThread
    -> ReadOnlyFrameSetQueue x N
    -> GPU / CPU / recording consumers
```

## 1. 重要な前提

### 1.1 ReadOnlyフレームは共有される

`CameraCaptureThread`と`FrameSyncThread`は、出力ごとにGPU textureを複製しない。各出力は同じ`ReadOnlyFrame`ストレージへの共有参照を保持する。

したがって、遅いconsumerがフレームを長時間保持すると、そのフレームが属する`FramePool` entryは再利用できない。

### 1.2 FramePool容量はin-flight上限である

`capture-pool-max`は単なる事前確保数ではない。次を合計した、同時に生存できるproducer frame数の上限である。

```text
FrameSyncThreadの内部buffer
各出力queueの待機中FrameSet
各consumerが処理中のFrameSet
GPU consumer completion待ちで保持されるFrame
```

出力数、queue容量、consumer latencyが増えるほど必要なpool容量は増える。

### 1.3 「全フレーム処理」は無限bufferを意味しない

全フレーム系queueは`RejectNew`を使う。queueが満杯の場合、中央`FrameSyncThread`をblockせず、その出力だけをdropとして記録する。

これはリアルタイム追従可否を測るための仕様である。`rejectNew > 0`または`FrameSyncOutputStats::queueDrops > 0`なら、そのconsumerは入力レートへ追従できていない。

### 1.4 表示系のdropは正常である

表示系は容量1の`DropOldest` queueと`waitPopLatestFor()`を使う。古い表示frameのdropは、低遅延表示を維持するための正常動作である。

## 2. MultiPipelineStressD3D12

このsampleは10個の独立した出力を同時に動作させる。

| # | Pipeline | Cameras | Mode | 主な負荷 |
|---:|---|---|---|---|
| 1 | pair display | 0,1 | latest | 各カメラを独立readbackし横連結表示 |
| 2 | camera 0 display | 0 | latest | 独立readbackと表示 |
| 3 | camera 1 display | 1 | latest | 独立readbackと表示 |
| 4 | pair AVI recording | 0,1 | all | 2回readback、連結、OpenCV VideoWriter |
| 5 | camera 0 AVI recording | 0 | all | readback、OpenCV VideoWriter |
| 6 | camera 1 AVI recording | 1 | all | readback、OpenCV VideoWriter |
| 7 | HLSL Sobel | 0,1 | all | GPU SRV入力、専用UAV出力pool |
| 8 | OpenCV Canny | 0 | all | 独立readback、GaussianBlur、Canny |
| 9 | OpenCV Sobel | 1 | all | 独立readback、GaussianBlur、Sobel magnitude |
| 10 | OpenCV pair display | 0,1 | latest | 2回readback、CLAHE、edge overlay、連結表示 |

CPU/display/video consumer間では、次を共有しない。

```text
D3D12 queue
command context
readback buffer cache
D3D12FrameReadback
CpuFrame
cv::Mat
OpenCV処理結果
```

これにより、各consumerがそれぞれGPU-to-CPU転送負荷を発生させる。

## 3. 主要メトリクス

### 3.1 Capture

```text
cameraNRead       成功したread数
cameraNPushed     sync ingressへ提出した数
cameraNTimeouts   read timeout数
cameraNErrors     timeout以外のread error数
poolNPublished    実行終了時点で生存中の公開frame数
poolNExhaustion   pool acquire失敗数
```

正常なsteady stateでは、`camera0Read`と`camera1Read`が近く、`cameraNErrors == 0`、`poolNExhaustion == 0`であることを期待する。

### 3.2 Synchronization

```text
syncInput       FrameSyncThreadへの入力frame数
syncSets        完成した同期set数
syncFps         syncSets / measurementSeconds
syncDrops       timestamp matchingで破棄したframe数
syncDropRate    syncDrops / syncInput
```

2カメラでdropがない場合、おおむね次が成立する。

```text
syncInput ~= camera0Read + camera1Read
syncSets ~= min(camera0Read, camera1Read)
syncDrops == 0
```

### 3.3 Output pipeline

```text
emitted        FrameSyncThreadがqueueへ正常提出したset数
dispatchDrop   queue満杯等で提出できなかったset数
received       workerがqueueから取得したset数
processed      workerが処理を完了したset数
fps            processed / measurementSeconds
queueMax       最大queue深度
rejectNew      RejectNewで拒否された数
dropOldest     latest queueが古い要素を捨てた数
failures       readback、GPU処理、保存等の失敗数
```

全フレーム系の基本合格条件は次である。

```text
dispatchDrop == 0
rejectNew == 0
dropOldest == 0
failures == 0
received == processed
```

## 4. 2026-07-12時点の予備実測

以下は特定のPC、2台のカメラ、1536x1536級の入力、ハードウェアトリガ環境で得られた予備結果であり、製品保証値ではない。

### 4.1 小さいpool: initial=16, max=64

主な結果:

```text
measurement      60.015 s
syncFps          25.210
syncDrops        1679
syncDropRate     0.357
camera0Read      3187
camera1Read      1516
camera0Timeouts  15
camera1Timeouts  1687
pool0Exhaustion  15
pool1Exhaustion  1687
```

解釈:

- camera1側でpool枯渇が継続し、capture throughputが約25 fpsへ低下した。
- `camera1Timeouts`と`pool1Exhaustion`が同数であり、下流が共有frameを保持し続けたことによるpool不足が主要因と考えられる。
- この状態の`syncFps`をカメラ本来のfpsとして扱ってはならない。

### 4.2 大きいpool: initial=128, max=256

主な結果:

```text
measurement      60.004 s
syncInput        6403
syncSets         3202
syncFps          53.363
syncDrops        0
camera0Read      3201
camera1Read      3202
camera0Timeouts  0
camera1Timeouts  0
pool0Exhaustion  0
pool1Exhaustion  0
```

解釈:

- poolを増やすことでcapture timeout、pool exhaustion、sync dropが解消した。
- 2台はほぼ同数を供給し、timestamp matcherもdropなしで追従した。
- この実行でパイプラインへ届いた入力は約53.36 synchronized sets/sであり、160 fpsではない。
- 53 fpsの原因はこの結果だけでは確定できない。外部トリガ周波数、露光時間、ROI、IC4 JSON内のframe rate、USB帯域、カメラ設定を個別に確認する必要がある。

### 4.3 Consumer throughput

pool=128/256の実行で得られた代表値:

```text
HLSL Sobel             53.363 fps  drop 0
OpenCV Canny           42.614 fps  reject 645
OpenCV Sobel           22.982 fps  reject 1823
Pair OpenCV recording   7.683 fps  reject 2741
Camera 0 recording     16.982 fps  reject 2183
Camera 1 recording     16.449 fps  reject 2215
```

結論:

- HLSL GPU処理は入力同期rateへ追従した。
- CPU readback + OpenCV Cannyは高いが、約53 fpsには完全追従しなかった。
- OpenCV VideoWriter経由の保存は明確なボトルネックである。
- 高fps全フレーム保存には、GPU resourceを直接受け取るhardware encoder経路が必要である。

## 5. Timestamp toleranceの調整

`FrameSyncThread`はtimestamp-nearestのみを使用する。`maxTimestampDiffNs`を超える場合、最古のfront frameをdropする。

### 5.1 大きすぎるtoleranceの危険

frame periodは次である。

```text
160 fps -> 6.25 ms
 53 fps -> 約18.9 ms
```

`--max-diff-us 30000`は30 msであり、53 fpsの1周期より大きく、160 fpsなら約4.8周期分である。この値は同期経路の動作確認には使えるが、隣接frameを誤ってpairingする可能性があるため、最終設定として無条件に採用してはならない。

### 5.2 推奨調整順

1. pool exhaustionを0にする。
2. sync-only sampleでcapture rateを確認する。
3. toleranceを大きめから開始する。
4. `syncDrops`、`syncFps`、実際のpair内timestamp差を確認しながら徐々に下げる。
5. 安定して同期できる最小値を採用する。
6. 最終的にはp50/p95/p99/maxのpair timestamp deltaを記録する。

小さいpoolの状態でtoleranceを調整すると、pool stallによるhost arrival skewと本来のカメラ同期誤差を区別できない。

### 5.3 Timestamp source

```text
HostReceived
  1プロセスのsteady_clock domainで比較可能。
  USB転送、thread scheduling、pool stallの影響を受ける。

Device
  camera device timestampを直接比較する。
  2台のtimestamp epoch/clock domainが共有されている場合のみ有効。

Auto
  実装の規則に従って利用可能なtimestampを選ぶ。
```

独立したdevice clockのabsolute timestampをそのまま比較できない場合、将来はtimestamp offset calibrationが必要になる。

## 6. 推奨テスト段階

### Stage A: capture/syncのみ

処理consumerを最小化し、次を確認する。

```text
camera0Read ~= camera1Read
poolExhaustion == 0
syncDropsが許容範囲
syncFpsが目標rateに近い
```

### Stage B: GPU処理のみ

HLSL pipelineを追加し、GPU command queueとFramePool lifetimeを確認する。

### Stage C: CPU処理を1つずつ追加

Canny、Sobel、display readbackを順に追加し、どのconsumerでqueueが増え始めるかを見る。

### Stage D: recording

OpenCV VideoWriterは基準負荷として使えるが、高fps全フレーム保存の最終実装とはみなさない。

### Stage E: soak test

60秒のsmokeが安定してから、10分、1時間、24時間へ延長する。

## 7. Build例

OpenCVが次にある例:

```text
C:\personal\iwatake\library\opencv
```

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
if "%IC4EXT_OK%"=="0" echo [ERROR] configure failed. CMD remains open.

if "%IC4EXT_OK%"=="1" cmake --build out\build\v2_d3d12 ^
  --config Release ^
  --target MultiPipelineStressD3D12 ^
  --parallel

if errorlevel 1 set "IC4EXT_OK=0"
if "%IC4EXT_OK%"=="0" echo [ERROR] build failed. CMD remains open.
```

## 8. 60秒smoke例

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
  --output-dir stress_output_smoke ^
  --csv stress_output_smoke\metrics.csv
```

このcommandの`max-diff-us=30000`は動作確認値であり、tolerance sweep後に小さくする。

## 9. 10分soak例

現在の約53 fps baselineを最低50 fpsとして監視する例:

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
  --output-dir stress_output_10min ^
  --csv stress_output_10min\metrics.csv
```

160 fpsを受入条件にする場合は、外部triggerが160 Hzであることを確認した上で、例えば次を追加する。

```text
--min-sync-fps 150
--min-sync-sets 90000
```

600秒で`min-sync-fps=150`なら自動計算される最低set数も90000であるため、通常はどちらか一方でもよい。

## 10. 現在残る検証課題

- large pool構成でtimestamp toleranceを再スイープする。
- pairごとのtimestamp delta分布をログ/CSVへ追加する。
- hardware trigger入力周波数を外部測定し、約53 fpsの原因を切り分ける。
- IC4 stream statisticsと`CameraPerformanceSnapshot`をstress CSVへ統合する。
- OpenCV VideoWriterをD3D12 hardware encodeへ置き換える。
- 160 fpsで10分、1時間、24時間の段階的soakを実施する。
- device removal、DRED、queue/fence timeoutのfailure pathを試験する。
