# ReadOnlyFrame usage and lifetime

この文書は、IC4Ext v2のReadOnly frame pipelineで受け取った`ReadOnlyFrame`または`ReadOnlyFrameSet`をconsumer側で扱うときの基本規則をまとめる。

v2では、`CameraCaptureThread`と`FrameSyncThread`はoutputごとのGPU texture copyを行わない。複数consumerへ渡されるのは、同じproducer-owned textureを共有保持する`ReadOnlyFrame` handleである。

## 1. 共通の寿命ルール

`ReadOnlyFrame`が参照するGPU textureは、producer側の`FramePool` entryに属する。

```text
CameraCapture-owned FramePool Texture2D
    +-- ReadOnlyFrame handle A
    +-- ReadOnlyFrame handle B
    +-- ReadOnlyFrame handle C
```

`FramePool` entryが再利用可能になるのは、次の条件を満たした後である。

```text
すべてのReadOnlyFrame / ReadOnlyFrameSet参照が解放される
かつ
GPU consumerがlifetime trackerで保持している参照も完了・解放される
```

そのため、次はすべてframe lifetimeを延ばす。

```text
local変数として保持しているReadOnlyFrame
local変数として保持しているReadOnlyFrameSet
output queue内に残っているReadOnlyFrameSet
consumerが処理中のReadOnlyFrameSet
ReadOnlyFrameLifetimeTrackerが保持しているFrame/FrameSet
```

性能問題を調べるときは、まず`FramePoolStats::published`、`FramePoolStats::exhaustionDrops`、output queueのdrop/reject、consumerの処理時間を確認する。

## 2. 参照するだけの使い方

対象例:

```text
metadataを読む
SRVとして一時的に参照する
shader入力として読むだけ
copy sourceとして読むだけ
表示や解析の入力として読むだけ
```

この使い方では、元textureを変更しない。

許可される操作:

```text
SRVとして読む
COPY_SOURCEとして読む
metadataを読む
別resourceへの入力として使う
```

禁止される操作:

```text
元textureへUAV書き込みする
元textureをCOPY_DESTとして上書きする
元textureをrender targetとして変更する
他consumerと競合するmutable resource state変更を行う
```

CPU上でmetadataだけを読んで終わる場合は、読み終わった時点で`ReadOnlyFrame`または`ReadOnlyFrameSet`を破棄してよい。

GPU commandで読む場合は、producer-ready tokenを待ってからGPU commandを投入し、そのGPU commandが入力textureを読み終わるまで入力frameを保持する。

```cpp
Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, frame);
auto consumerDone = SubmitReadOnlyConsumerWorkAndSignal();
lifetimeTracker.retainUntil(frame, consumerDone);
lifetimeTracker.collectCompleted();
```

`producer-ready`はproducerがtextureへの書き込みを終えたことだけを表す。consumer側GPU workの完了は表さない。GPU workを投入した直後に`ReadOnlyFrame`を解放すると、producerがpoolへ戻ったtextureへ次frameを書き込み、まだ実行中のconsumer GPU workと競合する可能性がある。

## 3. 画像処理などの変更が伴う場合

対象例:

```text
resize
format conversion
blur
remap
edge detection
inference preprocessing
描画用変換
```

この使い方では、入力`ReadOnlyFrame`を読み取り専用のsourceとして扱い、結果はconsumerが所有する別resourceへ書き込む。

```text
shared ReadOnlyFrame
    -> consumer-owned processing
    -> consumer-owned destination Texture / output pool
    -> next stage
```

重要な規則:

```text
入力textureを書き換えない
処理結果用textureはconsumerが所有する
処理結果用FramePoolもconsumer側で管理する
入力frameはGPU処理が読み終わるまで保持する
処理結果を後段へ渡す場合は、入力ではなく出力resourceの所有権を渡す
```

D3D12Helperなどのprocessing helperが別resourceへ結果を書き込む場合、その出力resourceはconsumer側のresourceである。resize後のframeを後段へ渡す場合は、元の`ReadOnlyFrame`を加工済みframeとして扱わず、consumer-owned outputを渡す。

```cpp
Pipe::WaitForReadOnlyFrameReadyOnQueue(processingQueue, inputFrame);

auto output = outputPool.acquire();
SubmitResizeOrProcessing(inputFrame, output.resource());
auto consumerDone = SignalProcessingDone();

inputLifetime.retainUntil(inputFrame, consumerDone);

// processing完了後、後段へ渡すのはconsumer-owned output
PublishProcessedOutput(std::move(output));
```

入力frameを早く解放したい場合でも、GPUが入力を読み終わるまでは`ReadOnlyFrameLifetimeTracker`などで保持する必要がある。

## 4. readbackする場合

対象例:

```text
CPU解析
OpenCV処理
CPU表示
VideoWriterへ渡すCPU frame生成
ログや検証用のpixel readback
```

readbackは、GPU textureからCPU memoryへ内容をコピーする処理である。

```text
ReadOnlyFrame / GPU Texture
    -> readback
    -> CpuFrame / cv::Mat / CPU memory
```

同期readback関数が正常に戻った後は、そのreadback処理として元GPU frameを保持し続ける必要はない。CPU側には`CpuFrame`が残るため、以降のOpenCV処理やVideoWriterはCPU memoryだけを使えばよい。

推奨パターン:

```cpp
auto set = queue->waitPopFor(timeout);
if (!set) return;

const auto* frame0 = set->find(0);
if (!frame0) return;

IC4Ext::CpuFrame cpu;
readback.readback(
    frame0->frame,
    IC4Ext::CpuFrameFormat::BGR8,
    cpu,
    5000);

// readback完了後、GPU入力が不要ならすぐ解放する
set = {};

// 以降はCPU memoryだけで処理する
RunOpenCV(cpu);
WriteVideo(cpu);
```

避けるべきパターン:

```cpp
auto set = queue->waitPopFor(timeout);

readback.readback(frame, format, cpu, timeout);

// setを保持したまま長いCPU処理を行う
cv::GaussianBlur(...);
cv::Canny(...);
writer.write(...);

// scope終端までsetが残る
```

この場合、readback自体は完了していても、`set`が生きている間は元`ReadOnlyFrame`の参照も残る。そのため、FramePool entryは戻らない。

readback後に解放できるもの:

```text
readbackに使ったReadOnlyFrame local variable
readbackに使ったReadOnlyFrameSet local variable
queueから取り出したFrameSet
```

readback後も残してよいもの:

```text
CpuFrame
cv::Mat
CPU側の処理結果
VideoWriterへ渡すCPU frame
```

ただし、queue内にまだ積まれている`ReadOnlyFrameSet`は、それぞれ元GPU frameを保持している。consumerが遅く、queue backlogが増えると、readback前の段階でFramePoolを圧迫する。全フレーム処理で`rejectNew`や`queueDrops`が増える場合、そのconsumerは入力rateへ追従できていない。

将来、非同期readbackを実装する場合は、readback copy完了まで入力`ReadOnlyFrame`を保持する必要がある。同期readbackでは、関数が正常に戻った時点でcopy完了済みとして扱える。

## 5. 実装時のチェックリスト

consumerを書くときは次を確認する。

```text
入力ReadOnlyFrameを変更していないか
GPUで読む場合、producer-readyを待っているか
GPU処理完了まで入力frameを保持しているか
変更を伴う処理の出力resourceをconsumerが所有しているか
readback後に不要なReadOnlyFrameSetをすぐ破棄しているか
OpenCVやVideoWriter中にGPU frameを保持し続けていないか
queue backlogがFramePoolを圧迫していないか
```

ReadOnly pipelineの性能問題は、多くの場合「GPU texture copyが遅い」ではなく、「共有ReadOnlyFrameをどのconsumerがどれだけ長く保持しているか」として現れる。
