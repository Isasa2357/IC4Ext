# 12. D3D12 FrameSyncThread

この文書は、IC4Ext 2.0.0の`IC4Ext::D3D12::FrameSyncThread`を定義する。

旧D3D12の`D3D12IndexedCameraFrame` / `D3D12SyncedFrameSet` / frame-number policyを前提としない。入力と出力はReadOnly handleである。

## 1. Purpose

`FrameSyncThread`は、複数`CameraCaptureThread`から届くReadOnly frameをtimestampで照合し、全cameraが揃った完全同期setを作る。そのsetから、登録された各outputに必要なcameraだけを選び、優先度順に配送する。

```text
CameraCaptureThread 0 ─┐
CameraCaptureThread 1 ─┼─> FrameSyncThread
CameraCaptureThread N ─┘        |
                                  +-> output A {0,1}
                                  +-> output B {0}
                                  +-> output C {1,N}
```

1つの同期domainにつき、原則として1つの`FrameSyncThread`を使う。

## 2. Public types

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D12;
```

```text
CameraId
SyncGroupId
FrameSyncOutputId
FrameRateLimit
FrameSyncTimestampSource
FrameSyncConfig
FrameSyncStats
FrameSyncOutputConfig
FrameSyncOutputInfo
FrameSyncOutputStats
ReadOnlyFrame
ReadOnlyFrameSet
IndexedReadOnlyFrameQueue
ReadOnlyFrameSetQueue
FrameSyncThread
```

## 3. Input

```cpp
std::shared_ptr<Pipe::IndexedReadOnlyFrameQueue>
```

queue itemは概念的に次である。

```cpp
struct IndexedReadOnlyCameraFrame
{
    CameraId cameraId;
    ReadOnlyFrame frame;
};
```

`CameraCaptureThread`は同じ中央input queueへpushする。

## 4. Output

outputごとに独立したqueueを登録する。

```cpp
std::shared_ptr<Pipe::ReadOnlyFrameSetQueue>
```

`ReadOnlyFrameSet`は次を持つ。

```text
syncGroupId
referenceTimestampNs
completedTime
selected ReadOnlyFrame handles
```

GPU textureはcopyされない。

## 5. Configuration

```cpp
Pipe::FrameSyncConfig config;
config.cameraIds = {0, 1};
config.timestampSource = Pipe::FrameSyncTimestampSource::HostReceived;
config.maxTimestampDiffNs = 4'000'000;
config.maxBufferedFramesPerCamera = 16;
config.groupTimeout = std::chrono::milliseconds(100);
```

validation:

```text
cameraIdsが空でない
cameraIdsに重複がない
maxTimestampDiffNs > 0
maxBufferedFramesPerCamera > 0
groupTimeout > 0
```

`cameraIds`と同期方式は`start()`前に固定する。output registryだけを実行中に変更できる。

## 6. Timestamp-nearest only

frame-number matchingは意図的に非対応である。

理由:

- cameraごとにframe counterのepochが異なる場合がある。
- open/start順序により開始番号がずれる。
- hardware triggerで露光が同期していてもabsolute frame numberが一致する保証はない。
- relative frame number補正もreconnect/drop後の再同期が複雑になる。

D3D12 ReadOnly pipelineでは必ずtimestampを使う。

## 7. Timestamp sources

### HostReceived

`FrameTiming::hostReceivedTime`をprocess-wide `steady_clock` domainで比較する。

利点:

- camera間で同じclock domain。
- device timestamp epochが異なっても比較可能。

影響を受けるもの:

- USB転送時間
- IC4 callback scheduling
- OS thread scheduling
- pending queue backlog
- FramePool exhaustionによるcapture stall

### Device

`FrameTiming::deviceTimestampNs`を直接比較する。

使用条件:

- 2台のdevice timestampが同じepoch/clock domainに属する、または外部同期されている。

独立clockのabsolute timestampをそのまま比較してはならない。

### Auto

利用可能なtimestampを実装規則に従って選ぶ。実機検証では、意図したdomainを明確にするため`HostReceived`または`Device`を明示することを推奨する。

## 8. Matching algorithm

cameraごとにFIFO bufferを持つ。

```text
buffers[camera0]
buffers[camera1]
...
```

基本loop:

```text
1. input frameをcamera bufferへ格納
2. 全camera bufferが非emptyか確認
3. 各front frameのtimestampを取得
4. min timestamp / max timestampを求める
5. max - min <= tolerance:
     全frontをpop
     complete setを作成
     dispatch
6. tolerance超過:
     最小timestampのfrontをdrop
     再試行
```

bufferが`maxBufferedFramesPerCamera`を超える場合は古いframeをdropする。

`groupTimeout`を超えて完全setにならない候補も破棄対象になる。

## 9. Complete-set-first rule

初期実装では、必ず同期domain内の全cameraが揃ってからcomplete setを作る。

```text
camera0 arrived
camera1 arrived
camera2 missing
    -> complete setなし
    -> camera2を必要としないoutputにもまだ配送しない
```

この制約は設計を単純化する。将来、outputごとのrequired subsetが揃った時点で先行配送する最適化を追加できるが、初期APIの意味は変えない。

## 10. Output registration

```cpp
Pipe::FrameSyncOutputConfig output;
output.requiredCameras = {0, 1};
output.frameRate = Pipe::FrameRateLimit::Maximum();
output.priority = 100;
output.enabled = true;

const Pipe::FrameSyncOutputId id =
    sync.registerOutput(queue, output);
```

validation:

```text
queue != nullptr
requiredCamerasが空でない
requiredCamerasに重複がない
全CameraIdがFrameSyncConfig::cameraIdsに含まれる
FrameRateLimitが有効
```

`requiredCameras`の順序はoutput `ReadOnlyFrameSet`内の順序として維持する。

## 11. Runtime changes

```cpp
sync.updateOutput(id, config);
sync.replaceOutputQueue(id, newQueue);
sync.unregisterOutput(id);
```

変更可能なもの:

```text
requiredCameras
frameRate
priority
enabled
output queue
outputの追加/削除
```

変更不可:

```text
sync domain cameraIds
timestamp source
tolerance
buffer limit
group timeout
```

同期設定まで実行中変更すると、既存bufferのtimestamp解釈と未完成groupの扱いが複雑になるため、初期実装では固定する。

## 12. Snapshot semantics

output tableはcopy-on-writeで更新する。

```text
current immutable table
    -> copy
    -> modify
    -> stable sort by priority
    -> atomic publish
```

1つのcomplete setをdispatchするとき、最初にoutput snapshotを取得する。

```text
SyncGroup 100 dispatch中にupdate
  group 100: old snapshot
  group 101: new snapshot
```

`unregisterOutput()`完了前にsnapshotへ入ったsetは、旧queueへ1回届く可能性がある。

## 13. Priority

priorityが大きいoutputから先に処理する。同priorityでは登録順を維持する。

```text
priority 1000
priority  900
priority  100
```

priorityはdispatch順だけを定義する。

保証しないもの:

```text
consumer thread priority
GPU queue priority
完了順
低priority outputの省略
```

## 14. FPS gate

```cpp
Pipe::FrameRateLimit::Maximum();
Pipe::FrameRateLimit::Fixed(30.0);
```

`FrameSyncThread`はsleepしない。timestampに基づきsetを選ぶ。

```text
complete set
    -> output enabled check
    -> FPS gate
    -> required camera handle selection
    -> queue push
```

FPS gateはpartial set生成前に行うため、skipしたoutputについてhandle copyやqueue pushを省略できる。

capture、timestamp matching、complete set生成は常に行う。

## 15. Queue push and backpressure

中央sync threadは1 outputの遅延でblockしてはならない。

### Latest output

```text
capacity = 1
policy = DropOldest
consumer = waitPopLatestFor
```

### All-frame output

```text
capacity = bounded
policy = RejectNew
consumer = FIFO
```

queue fullの場合:

```text
そのoutputのqueueDrops++
他outputのdispatchは継続
capture/sync threadは継続
```

all-frame outputでqueue dropが発生した場合、そのconsumerは入力rateへ追従できていない。

## 16. Dispatch algorithm

```text
complete set created
    -> load output snapshot
    -> for output in priority order
         if disabled:
             disabledSkips++
             continue
         consideredSets++
         if FPS gate rejects:
             skippedByFrameRate++
             continue
         select required ReadOnlyFrame handles
         construct ReadOnlyFrameSet
         non-blocking push
         success: emittedSets++
         failure: queueDrops++
```

## 17. Statistics

### Global

```cpp
struct FrameSyncStats
{
    uint64_t inputFrames;
    uint64_t completedSets;
    uint64_t ignoredFrames;
    uint64_t droppedFrames;
    uint64_t incompleteSets;
    uint64_t totalOutputSets;
    uint64_t totalOutputQueueDrops;
};
```

### Per output

```cpp
struct FrameSyncOutputStats
{
    uint64_t consideredSets;
    uint64_t skippedByFrameRate;
    uint64_t emittedSets;
    uint64_t queueDrops;
    uint64_t disabledSkips;
};
```

## 18. Tolerance selection

frame period:

```text
160 fps = 6.25 ms
53 fps  = 約18.9 ms
```

`tolerance > frame period`は、隣接frameの誤pairingリスクがある。

一方、pool exhaustion、USB scheduling、host callback jitterがある状態では、`HostReceived`差が大きくなり、厳しいtoleranceで大量dropする。

推奨順序:

1. FramePool exhaustionを0にする。
2. capture/sync-onlyで入力rateを確認する。
3. 大きめのtoleranceで経路を確認する。
4. toleranceを段階的に下げる。
5. pair timestamp delta分布を確認する。
6. 安定する最小値を採用する。

## 19. Preliminary result

10-output実機試験で、poolを16/64から128/256へ増やすと次が変化した。

```text
small pool:
  synchronized rate 約25 fps
  pool exhaustion / timeout / sync dropあり

large pool:
  synchronized rate 約53.36 fps
  pool exhaustion 0
  capture timeout 0
  sync drop 0
```

この結果は、timestamp toleranceだけでなくFramePool lifetime/backlogを先に解消すべきことを示す。

## 20. Threading

- input bufferとmatching stateはworker threadが所有する。
- output registry updateは別threadから呼べる。
- stats/errorは内部同期する。
- output queueはThreadKitのthread-safe queueを前提とする。
- `requestStop()`はworkerへ停止要求を出す。
- destructorは`stopAndJoin()`相当の安全な停止を行う。

## 21. Tests

```text
test_d3d12_readonly_pipeline
  config/output validation

test_d3d12_dummy_capture_sync_integration
  ReadOnlyFrameSource x2
  CameraCaptureThread x2
  timestamp match
  output registration stats
  pool release
```

## 22. Related documents

```text
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
samples/MultiPipelineStressD3D12/README.md
```
