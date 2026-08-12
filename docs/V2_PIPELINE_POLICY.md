# IC4Ext v2 Shared ReadOnly Pipeline Policy

この文書は、IC4Ext v2で提供するcamera frame pipelineの正式な構成、責務境界、GPU resourceの所有権、および非責務を定義する。

D3D11とD3D12の両backendに共通して適用する。backend固有のresource state、fence、context同期については、それぞれの`READONLY_PIPELINE.md`を参照する。

## 1. 正式パイプライン

IC4Ext v2が提供する正式なframe pipelineは、次の多入力・多出力構成のみである。

```text
CameraCaptureThread 0 ----+
CameraCaptureThread 1 ----+--> one shared ingress queue
CameraCaptureThread ... --+            |
                                       v
                              one central FrameSyncThread
                                       |
                           +-----------+-----------+
                           v           v           v
                    output queue A output queue B output queue C
                           |           |           |
                           v           v           v
                       consumer A  consumer B  consumer C
```

原則として、1つの同期domainにつき1つの`FrameSyncThread`を使用する。

次の構成はv2の正式パイプラインとして提供しない。

```text
CameraCaptureThreadがconsumerごとのoutput queueへ直接fan-outする構成
処理ごとに独立したFrameSyncThreadを作る構成
FrameSyncThreadが複数のCameraCaptureを内部所有してreadする構成
```

## 2. 各componentの責務

### 2.1 CameraCapture

`CameraCapture`は1台のcameraについて、次を担当する。

- IC4 deviceとstreamの管理
- IC4 ImageBufferの取得
- camera inputからD3D11/D3D12 Texture2DへのGPU変換
- 完成Textureを保持するproducer側FramePool
- frame timing、format、chunk metadataの生成
- producer-ready fence tokenの生成

完成Textureは`CameraCapture`所有のFramePoolに属する。

### 2.2 CameraCaptureThread

`CameraCaptureThread`は1台の`CameraCapture`または1つの`ReadOnlyFrameSource`を継続的にreadし、中央同期用ingress queueへ提出する。

```text
read(ReadMode::NextFrame)
    -> IndexedReadOnlyCameraFrame{cameraId, ReadOnlyFrame}
    -> central ingress queue
```

`CameraCaptureThread`は次を行わない。

- consumerごとのfan-out
- consumerごとのGPU texture copy
- resize、format conversion、post-process
- 複数cameraのmatching

### 2.3 FrameSyncThread

`FrameSyncThread`は複数のcamera inputを受け取る中央の多入力・多出力dispatcherである。

担当範囲は次である。

- cameraごとのframe buffering
- timestamp-nearest matching
- 完全同期setの生成
- outputごとの`requiredCameras`選択
- outputごとのFPS gate
- priority順のdispatch
- output queueの登録、更新、差替え、削除
- queue pushとdrop統計

`FrameSyncThread`は次を行わない。

- camera deviceまたは`CameraCapture`の所有
- cameraからの直接read
- GPU画像処理
- resize
- format conversion
- blur、remap、推論前処理などのpost-process
- outputごとのGPU texture生成

### 2.4 Consumer

consumerは、受け取った`ReadOnlyFrame`または`ReadOnlyFrameSet`を入力として処理する。

resize、format conversion、filter、remap、inference、rendering、encoding、readbackなどはconsumer側の責務である。

入力Textureを書き換える必要がある処理は、consumer専用のdestination resourceまたはoutput poolを確保する。

```text
shared ReadOnlyFrame
    -> consumer-owned processing
    -> consumer-owned destination Texture / output pool
    -> next consumer-specific stage
```

## 3. Copyと共有所有権

capture層とsync層は、fan-outのためのGPU texture copyを行わない。

複数outputへ配送されるのは、同じTextureを共有保持する`ReadOnlyFrame` handleである。

```text
CameraCapture-owned Texture2D
    +-- ReadOnlyFrame handle for output A
    +-- ReadOnlyFrame handle for output B
    +-- ReadOnlyFrame handle for output C
```

`ReadOnlyFrame`のcopyはshared ownershipを増やすだけであり、GPU resourceをcopyしない。`std::move`はC++ handleの所有権を移すだけであり、Textureの物理転送や複製を意味しない。

同期setからoutputごとの部分setを作る場合も、参照する`ReadOnlyFrame`を選択するだけであり、GPU resourceをcopyしない。

## 4. ReadOnly contract

capture/sync層から公開されるTextureはimmutableな入力として扱う。

許可する操作:

```text
SRVとして読む
copy sourceとして読む
metadataを読む
別resourceへの処理入力として使う
```

禁止する操作:

```text
元TextureへのUAV書き込み
元Textureをcopy destinationとして上書き
元Textureをrender targetとして変更
他consumerと競合するmutable操作またはresource state変更
```

書き込み処理はconsumer所有の別resourceへ出力する。

## 5. GPU lifetime

producer-ready tokenはproducerによるTexture書き込み完了を表す。consumerのGPU処理完了は表さない。

consumerは次を満たさなければならない。

1. producer-ready tokenを待ってから入力Textureを使用する。
2. consumerのGPU commandが入力Textureを読み終えるまで、対応する`ReadOnlyFrame`または`ReadOnlyFrameSet`を保持する。
3. GPU処理完了後にlifetime trackerから解放する。

```cpp
WaitForReadOnlyFrameReadyOnQueue(processingQueue, frame);
auto consumerDone = SubmitConsumerWorkAndSignal();
lifetimeTracker.retainUntil(frame, consumerDone);
lifetimeTracker.collectCompleted();
```

入力handleを早期解放すると、FramePoolへ返却されたTextureへproducerが次frameを書き込み、実行中のconsumer GPU workと競合する可能性がある。

## 6. Output configuration

`FrameSyncOutputConfig`は配送制御だけを表す。

```cpp
struct FrameSyncOutputConfig
{
    std::vector<CameraId> requiredCameras;
    FrameRateLimit frameRate;
    std::int32_t priority;
    bool enabled;
};
```

設定項目の意味:

- `requiredCameras`: outputへ含めるcamera参照
- `frameRate`: `Maximum()`または`Fixed(fps)`による配送頻度
- `priority`: `FrameSyncThread`内のdispatch順
- `enabled`: output配送の有効・無効

resize size、resize filter、output format、processing shaderなどの画像処理設定は追加しない。これらはconsumer固有の設定として管理する。

## 7. FPS gate

outputごとのFPS制限は`FrameSyncThread`の配送gateである。

FPS gateは次を削減する。

- output用partial `ReadOnlyFrameSet`生成
- shared handleのcopy
- queue push
- 後段consumer処理

一方、camera captureと完全同期setの構築は常に継続する。`FrameSyncThread`はFPS制限のためにcamera readを停止したり、sleepによってcapture rateを制御したりしない。

## 8. IC4Ext本体の非責務

次はIC4Ext v2のcapture/sync pipelineには含めない。

- outputごとのresize
- outputごとのformat conversion
- blur、edge detection、remapなどの画像処理
- inference-specific preprocessing
- rendering固有のresource生成
- video encoding
- consumer固有のrecording policy

sampleがOpenCVやvideo encoderを使用する場合、それらはconsumer workloadの例であり、IC4Ext本体のpipeline責務を拡張するものではない。

## 9. 実装レビュー時の確認事項

v2 pipelineへ変更を加える際は、少なくとも次を確認する。

- `CameraCaptureThread -> central FrameSyncThread -> output queues`の構成を維持しているか。
- capture/sync層にconsumer固有の処理を追加していないか。
- fan-out時にGPU texture copyを追加していないか。
- input TextureのReadOnly contractを維持しているか。
- consumer GPU completionまで入力frameのlifetimeを保持できるか。
- `FrameSyncOutputConfig`が配送設定に限定されているか。
- resizeなどの出力resource所有者がconsumer側になっているか。
