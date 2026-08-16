# 12. D3D12 FrameSyncThread

この文書は、IC4Ext 2.0.0の`IC4Ext::D3D12::FrameSyncThread`を定義する。

旧D3D12のphysical-copy fan-out、frame-number matching、output queue replacement、one-step unregisterを前提としない。入力と出力はReadOnly handleであり、runtime outputの終了は供給停止とchannel closeの二段階で行う。

## 1. Purpose

`FrameSyncThread`は、複数`CameraCaptureThread`から届くReadOnly frameをtimestampで照合し、全cameraが揃った完全同期setを作る。そのsetから、登録された各outputに必要なcameraだけを選び、優先度順に配送する。

```text
CameraCaptureThread 0 ─┐
CameraCaptureThread 1 ─┼─> one FrameSyncThread
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
FrameSyncOutputState
FrameSyncOutputInfo
FrameSyncOutputStats
ReadOnlyFrame
ReadOnlyFrameSet
IndexedReadOnlyFrameQueue
ReadOnlyFrameSetQueue
FrameSyncThread
```

## 3. Input and complete-set-first matching

入力queue itemは次である。

```cpp
struct IndexedReadOnlyCameraFrame
{
    CameraId cameraId;
    ReadOnlyFrame frame;
};
```

cameraごとにFIFO bufferを持ち、timestamp-nearestで照合する。

```text
1. input frameを対応camera bufferへ格納
2. 全camera bufferのfront timestampを比較
3. max - min <= maxTimestampDiffNsならcomplete setを生成
4. tolerance超過なら最小timestampのfrontをdrop
5. buffer上限またはgroupTimeout超過でも古い候補をdrop
```

初期実装はcomplete-set-firstである。同期domain内の全cameraが揃う前に、camera subsetだけを要求するoutputへ先行配送しない。

frame-number matchingは使用しない。cameraごとにcounter epoch、開始番号、reconnect後の番号が異なり得るためである。

## 4. Timestamp source

`HostReceived`はprocess-wide `steady_clock` domainを比較する。camera間で共通domainだが、USB転送、callback scheduling、queue backlogの影響を含む。

`Device`はdevice timestampを比較する。複数cameraのdevice timestampが比較可能なclock domainにある場合だけ使用する。

`Auto`は利用可能なtimestampを実装規則に従って選ぶ。実機評価では意図したdomainを明示することを推奨する。

## 5. Output registration

```cpp
Pipe::FrameSyncOutputConfig config;
config.requiredCameras = {0, 1};
config.frameRate = Pipe::FrameRateLimit::Maximum();
config.priority = 100;
config.enabled = true;

const auto id = sync.registerOutput(queue, config);
```

`registerOutput()`は次を拒否する。

```text
null queue
既にcloseされたqueue
同じqueue instanceの重複登録
空または重複したrequiredCameras
sync domain外のCameraId
無効なFrameRateLimit
```

output IDとqueueの対応は登録後に不変である。

## 6. Runtime configuration update

```cpp
sync.updateOutput(id, newConfig);
```

`Active`なoutputだけを更新できる。更新可能なものは配送設定である。

```text
requiredCameras
frameRate
priority
enabled
```

queueは変更できない。queueや処理先を切り替える場合は、新しいoutputを追加してから古いoutputを二段階で終了する。

`enabled=false`は一時的なsoft-disableであり、consumer/resourceを破棄できる安全な退役保証ではない。

## 7. Two-stage output retirement

### 7.1 Stop supply

```cpp
sync.stopOutputSupply(id);
```

成功して戻った時点で、次を保証する。

```text
対象outputへのqueue pushは実行中ではない
対象outputへの新しいqueue pushは開始されない
削除前snapshotによるlate pushも存在しない
queueはまだopenである
既にqueueへ入ったReadOnlyFrameSetは残る
他outputへの配送は継続する
```

この保証のため、dispatchと`registerOutput()`、`updateOutput()`、`stopOutputSupply()`、`closeOutputChannel()`を同じlifecycle mutexで線形化する。

通常状態は`Active -> SupplyStopped`である。供給停止はterminalであり、再開APIはない。再開したい場合は新しいoutputを登録する。

### 7.2 Drain or discard

既存frameを使い切る場合:

```cpp
sync.stopOutputSupply(id);
sync.closeOutputChannel(id);
while (auto set = queue->waitPop()) {
    Process(*set);
}
```

既存frameを破棄する場合:

```cpp
sync.stopOutputSupply(id);
queue->clear();
sync.closeOutputChannel(id);
```

### 7.3 Close channel

```cpp
sync.closeOutputChannel(id);
```

`SupplyStopped`または`Faulted`でだけ成功する。`Active`なoutputを直接closeできない。

成功時は次を行う。

```text
queue->close()
待機consumerをwake
FrameSyncThreadのqueue参照を解放
output registryから削除
```

`BlockingQueue::close()`はqueue内要素をclearしない。consumerはclose後も残存frameをdrainできる。

詳細は`docs/OUTPUT_LIFECYCLE.md`を参照する。

## 8. Replacement policy

`replaceOutputQueue()`は提供しない。置換相当の処理は常に次へ分解する。

```text
新consumerとqueueを完全に準備
新outputをregisterOutput
必要なら最初のframeを確認
旧outputをstopOutputSupply
旧backlogをdrainまたはclear
旧outputをcloseOutputChannel
旧consumerをjoin
旧consumer GPU work完了を待つ
旧resourceを破棄
```

v2 fan-outは同じReadOnly resourceへのshared handleを渡すため、一時的に旧新outputが並存してもper-output GPU copy経路へ切り替わらない。

## 9. Output fault isolation

output queueが供給中に外部からcloseされた場合や、1 outputのdispatchで例外が発生した場合、そのoutputだけを`Faulted`へ移す。

```cpp
auto state = sync.outputState(id);
auto stats = sync.outputStats(id);
auto error = sync.outputLastError(id);
```

`Faulted`は新規供給を行わないterminal stateである。中央workerと他outputは継続し、その後`closeOutputChannel()`でregistryから解放する。

追加統計:

```text
dispatchErrors
closedQueuePushes
```

## 10. Priority, FPS gate, and queue pressure

priorityが大きいoutputから先に処理し、同priorityでは登録順を維持する。priorityはdispatch順だけを定義し、consumer thread/GPU完了順を保証しない。

```cpp
Pipe::FrameRateLimit::Maximum();
Pipe::FrameRateLimit::Fixed(30.0);
```

FPS gateはpartial set生成、shared handle copy、queue push、後段処理を削減する。capture、timestamp matching、complete set生成は継続する。

中央sync threadは非blocking `push()`を使用する。

```text
Latest output: capacity=1, DropOldest
All-frame output: bounded capacity, RejectNew
```

queue fullはそのoutputのdropとして記録し、他outputとcaptureを停止しない。

## 11. Statistics

Global:

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

Per output:

```cpp
struct FrameSyncOutputStats
{
    uint64_t consideredSets;
    uint64_t skippedByFrameRate;
    uint64_t emittedSets;
    uint64_t queueDrops;
    uint64_t disabledSkips;
    uint64_t dispatchErrors;
    uint64_t closedQueuePushes;
};
```

`closeOutputChannel()`後はregistry entryが消えるため、最終統計が必要ならclose前にsnapshotする。

## 12. Threading and exception boundary

- input bufferとmatching stateは中央workerが所有する。
- runtime output操作は別threadから呼べる。
- output lifecycle mutexがdispatchとlifecycle操作を線形化する。
- stats/errorは内部同期する。
- output queueはThreadKitのthread-safe queueを前提とする。
- worker最上位でC++例外を捕捉する。
- output dispatch例外はoutput単位で捕捉し、中央workerを終了させない。
- destructorは`stopAndJoin()`相当の安全な停止を行う。

## 13. Tests

```text
test_d3d12_readonly_pipeline
  config/output validation

test_d3d12_dummy_capture_sync_integration
  ReadOnlyFrameSource x2
  CameraCaptureThread x2
  timestamp matching
  output stats
  pool release

test_d3d12_dynamic_output_lifecycle
  permanent output Aを継続
  dynamic output Bを200回add/stop/close
  stop復帰後のlate pushなし
  drainとclearの両方
  Activeの直接close拒否
  closed queue再登録拒否
  BのFaulted状態がAと中央workerへ波及しない
```

実cameraの固定output構成は、2台・1536x1536・hardware trigger・160 fps・30分で検証済みである。dynamic lifecycleの実camera 160 fps反復試験は別途追加する。

## 14. Related documents

```text
docs/OUTPUT_LIFECYCLE.md
docs/V2_PIPELINE_POLICY.md
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
docs/d3d12/MULTI_CAMERA_PIPELINE_ACCEPTANCE.md
samples/MultiPipelineStressD3D12/README.md
```
