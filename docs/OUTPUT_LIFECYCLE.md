# FrameSyncThread output lifecycle

この文書は、IC4Ext v2のD3D11/D3D12 `FrameSyncThread`における動的outputの追加と削除を定義する。

## 1. 基本方針

output queueの置換APIは提供しない。処理先を切り替える場合も、必ず次の独立した操作として表現する。

```text
新しいoutputを追加
    ↓
古いoutputへの供給を停止
    ↓
古い通信路をclose
```

output IDとqueueの対応は登録後に変更しない。同じqueue instanceを複数output IDへ登録することも認めない。

削除は1命令ではなく、次の2段階で行う。

```text
stopOutputSupply(outputId)
    供給側の同期barrier

closeOutputChannel(outputId)
    通信路のcloseとFrameSyncThread側参照の解放
```

## 2. 状態

```cpp
enum class FrameSyncOutputState
{
    Active,
    SupplyStopped,
    Faulted,
};
```

通常の状態遷移は次である。

```text
Active
    ↓ stopOutputSupply()
SupplyStopped
    ↓ closeOutputChannel()
registryから削除
```

`SupplyStopped`から`Active`へ戻す操作はない。再開したい場合は新しいqueueを準備し、新しいoutput IDとして登録する。

queueが供給中に外部からcloseされた場合や、そのoutputだけのdispatchで例外が発生した場合は`Faulted`へ遷移する。`Faulted`も新規供給が行われないterminal stateであり、`closeOutputChannel()`で終了する。

## 3. 追加

consumerを完全に準備した後でoutputを公開する。

```cpp
auto queue = std::make_shared<Pipe::ReadOnlyFrameSetQueue>(queueOptions);

Consumer consumer(queue);
if (!consumer.initialize()) return false;
if (!consumer.start()) return false;
consumer.waitUntilReady();

Pipe::FrameSyncOutputConfig config;
config.requiredCameras = {0, 1};
config.frameRate = Pipe::FrameRateLimit::Maximum();
config.priority = 100;

const auto outputId = sync.registerOutput(queue, config);
if (outputId == Pipe::InvalidFrameSyncOutputId) {
    consumer.requestStop();
    consumer.join();
    return false;
}
```

`registerOutput()`は次を拒否する。

```text
null queue
既にcloseされたqueue
同じqueue instanceの重複登録
無効なrequiredCameras / frameRate
```

## 4. 第一段階: 供給停止

```cpp
if (!sync.stopOutputSupply(outputId)) {
    const auto error = sync.lastError();
}
```

成功して戻った時点で、次を保証する。

```text
対象outputへのqueue pushは実行中ではない
対象outputへの新しいqueue pushは開始されない
古いdispatch snapshotによるlate pushも残っていない
queueはcloseされていない
queue内の既存ReadOnlyFrameSetは保持される
他outputへの配送は継続する
```

この保証のため、output dispatchと`registerOutput()`、`updateOutput()`、`stopOutputSupply()`、`closeOutputChannel()`は同じlifecycle mutexで線形化される。

`stopOutputSupply()`はidempotentである。既に`SupplyStopped`または`Faulted`なら成功する。

供給停止後のoutputは更新できない。別設定で再開する場合は、新しいoutputを追加する。

## 5. 第二段階: 通信路のclose

```cpp
if (!sync.closeOutputChannel(outputId)) {
    const auto error = sync.lastError();
}
```

`closeOutputChannel()`は`SupplyStopped`または`Faulted`のoutputにだけ使用できる。`Active`なoutputへ直接呼ぶと失敗する。

成功時は次を行う。

```text
queue->close()
待機consumerをwake
FrameSyncThreadのqueue参照を解放
output registryから削除
```

`BlockingQueue::close()`は内部要素をclearしない。そのため、処理側は既存frameを使い切るか、破棄するかを選択できる。

## 6. 既存frameを使い切る

```cpp
sync.stopOutputSupply(outputId);
sync.closeOutputChannel(outputId);

// close後も残存要素はpopできる。
while (auto frameSet = queue->waitPop()) {
    Process(*frameSet);
}

consumer.join();
consumer.waitForGpuCompletion();
```

queueがcloseされ、かつemptyになると`waitPop()`は`std::nullopt`を返す。

## 7. 既存frameを破棄する

```cpp
sync.stopOutputSupply(outputId);

// 新規pushはもう発生しないため、安全に既存参照を解放できる。
queue->clear();

sync.closeOutputChannel(outputId);
consumer.requestStop();
consumer.join();
consumer.waitForGpuCompletion();
```

`ReadOnlyFrameSet`をclearすると、その中のReadOnly参照も解放される。consumerがGPU workを発行済みなら、resource破棄前にconsumer fenceと`ReadOnlyFrameLifetimeTracker`の完了を待つ。

## 8. 切替

新しい処理先Bへ切り替える場合は、queue replacementではなくadd + two-stage retirementを使う。

```text
Bのresourceを初期化
B consumerを開始
BをregisterOutput
必要ならBの最初のframeを確認
AをstopOutputSupply
A backlogをdrainまたはclear
AをcloseOutputChannel
A consumerをjoin
AのGPU完了を待つ
A resourceを破棄
```

AとBを一時的に同時供給しても、v2は同じReadOnly resourceへのhandleをfan-outするだけであり、output数によってper-output GPU copy経路へ切り替わらない。

## 9. 障害分離

1つのoutput queueが不正にcloseされた場合や、output dispatchで例外が発生した場合、そのoutputだけを`Faulted`にする。中央`FrameSyncThread`と他outputは継続する。

確認API:

```cpp
auto state = sync.outputState(outputId);
auto stats = sync.outputStats(outputId);
auto error = sync.outputLastError(outputId);
```

追加統計:

```text
dispatchErrors
closedQueuePushes
```

`Faulted` outputは`stopOutputSupply()`を呼んでも成功し、その後`closeOutputChannel()`で通信路を閉じる。

## 10. 禁止する終了手順

次の順序は使用しない。

```cpp
consumer.reset();
queue.reset();
// その後で供給停止
```

また、`enabled=false`は一時的な配送制御であり、terminalな削除操作ではない。consumer/resourceを破棄するときは必ず`stopOutputSupply()`の同期barrierを通す。

## 11. 回帰テスト

D3D11/D3D12それぞれに次のtestを用意する。

```text
test_d3d11_dynamic_output_lifecycle
test_d3d12_dynamic_output_lifecycle
```

検証内容:

```text
常設output Aを動作させ続ける
動的output Bを200回追加
Bを即時またはbacklogありでstop
stop復帰後にBのemittedSetsが増えない
B backlogをdrainする場合とclearする場合を両方確認
close前のActive outputを拒否
close後にregistryから消える
closed queueを登録できない
1 outputのFaulted状態がAとFrameSyncThreadへ波及しない
```
