# 複数カメラの同期表示・録画・解析

この章では、複数の IC4 カメラから取得したフレームを対応付け、1枚の合成映像として表示・録画しながら解析する方法を説明します。

## 対応範囲

IC4Ext v1.0.1 では、D3D12 backend を使って次の構成を利用できます。

```text
複数 IC4 camera
  -> D3D12CameraCaptureThread
  -> D3D12IndexedFrameQueue
  -> D3D12FrameSyncThread
  -> D3D12SyncedFrameSet
  -> 表示 / 録画 / 解析
```

カメラ側の露光制御は次から選びます。

```text
None             : free-run
HardwareTrigger  : 外部入力信号で露光開始を揃える
SoftwareTrigger  : PCからTriggerSoftware commandを各カメラへ送る
```

取得後のframe set化は、IC4Ext本体では次から選べます。

```text
TimestampNearest : timestamp差が許容値以内のフレームを組にする
FrameNumberExact : frame numberが完全一致するフレームを組にする
```

`FrameNumberExact` は、全カメラのframe counterが同じ起点に揃っていることを外部条件として保証できる場合だけ使用します。独立したカメラを別々に起動すると、同じ瞬間のフレームでもframe numberに一定offsetが付く可能性があります。

## timestamp source

`FrameSyncOptions::timestampSource` で、`TimestampNearest`が比較する時計を選択します。

```cpp
IC4Ext::FrameSyncOptions syncOptions;
syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
syncOptions.cameraIndices = {0, 1};
syncOptions.maxTimestampDiffNs = 100'000'000;
syncOptions.maxBufferedFramesPerCamera = 32;
```

選択肢は次のとおりです。

```text
Auto         : hostReceivedTimeを優先し、利用できない場合device timestampへfallback
HostReceived : 同一プロセスのsteady_clockで記録した受信時刻を比較
Device       : cameraのdeviceTimestampNsを直接比較
```

独立したUSBカメラのdevice timestampは、counterの原点がカメラごとに異なる場合があります。そのため、free-runカメラの組み合わせでは`HostReceived`を使用します。

`Device`は、カメラclockが外部同期されている、または同一clock domainで比較可能であることを確認できる場合に使用します。

## カメラ設定

### 同期なし

```cpp
IC4Ext::CameraCaptureConfig config;
IC4Ext::ConfigureNoSync(config);
```

### HW trigger

```cpp
IC4Ext::CameraCaptureConfig config;
IC4Ext::ConfigureHardwareTriggerSync(
    config,
    "Line1",
    "FrameStart",
    "RisingEdge");
```

`Line1`はカメラ機種によって異なる可能性があります。実機property mapで利用可能な`TriggerSource`を確認してください。

### SW trigger

```cpp
IC4Ext::CameraCaptureConfig config;
IC4Ext::ConfigureSoftwareTriggerSync(config);

capture.open(selector, config, backend);
capture.softwareTrigger();
```

複数台へ順番にcommandを送るため、SW triggerは厳密な同時露光を保証しません。実際のtimestamp差を計測してください。

## 複数カメラの二段階起動

解析サンプルは、全カメラを準備してから取得を開始します。

```text
Phase 1: 全カメラのstreamを準備
  camera 0: deviceOpen + streamSetup
  camera 0: AcquisitionStop
  camera 1: deviceOpen + streamSetup
  camera 1: AcquisitionStop
  ...

Phase 2: 全カメラを開始
  D3D12FrameSyncThreadを開始
  全camera worker threadを開始
  camera 0..N: AcquisitionStart
```

実装上は各`D3D12CameraCapture::open()`の直後にcommand propertyを実行します。

```cpp
capture.open(selector, config, backend);
capture.setIC4Property("AcquisitionStop", std::string("execute"));
```

全台を準備し、capture threadへ移した後に取得を開始します。

```cpp
cameraThread.setIC4Property("AcquisitionStart", std::string("execute"));
```

HW triggerの場合、外部trigger信号は全カメラの`AcquisitionStart`成功後に入力してください。

解析サンプルの既定値は次です。

```text
format                  = BayerRG8
camera-setup-delay-ms   = 1000
camera-open-retries     = 3
camera-retry-delay-ms   = 3000
max-timestamp-diff-ns   = 100000000
sync policy             = TimestampNearest
timestamp source         = HostReceived
```

## 同期表示と録画

`MultiCameraSyncDisplayD3D12`は、同期済みフレームをアスペクト比を保ったままグリッドへ配置します。

```text
2台       : 左右並び
3台以上   : 正方形に近いグリッド
--record  : 表示と同じ合成canvasを録画
```

frame counterが揃っていないカメラでは、`TimestampNearest + HostReceived`を使用してください。

## 解析しながら表示・録画

`MultiCameraAnalysisDisplayD3D12`はOpenCVをサンプル内だけで利用します。IC4Ext本体はOpenCVに依存しません。

各カメラについて次を解析します。

```text
平均輝度
直前フレームとの差分率
動領域の矩形
動領域数
```

解析結果は各映像へ直接描画されます。

```text
黄色矩形 : 検出した動領域
Luma      : 平均輝度
Motion    : 変化画素率
Regions   : 動領域数
```

解析済みの各映像をグリッドへ配置し、その同じ合成`cv::Mat`を表示と録画へ渡します。そのため、録画にも解析結果が含まれます。

### ビルド

vcpkgを使用する場合:

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

### free-runで実行

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp --format BayerRG8 --width 1280 --height 720 --fps 30 --camera-setup-delay-ms 1000 --camera-open-retries 3 --camera-retry-delay-ms 3000 --max-timestamp-diff-ns 100000000
```

### HW triggerで実行

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy timestamp --format BayerRG8 --fps 60 --max-timestamp-diff-ns 10000000
```

### 録画も行う場合

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy timestamp --format BayerRG8 --fps 60 --max-timestamp-diff-ns 10000000 --canvas-width 1920 --canvas-height 1080 --motion-threshold 24 --min-motion-area 400 --record analyzed_sync.mp4
```

解析サンプルはhost timer同期に固定されています。`--sync-policy frame-number`を指定した場合は、frame counter offsetによる誤対応を避けるためエラー終了します。

## 統計ログ

30表示setごと、または同期setを1秒以上取得できない場合、次を表示します。

```text
sets
syncInput
syncEmitted
syncDropped
syncIgnored
cameraN.read
cameraN.pushed
cameraN.errors
```

表示set数がsync emitted set数より少ない場合、OpenCV解析・readback・表示がカメラ入力速度より遅く、output queueで古いsetが置き換えられている可能性があります。

## 性能上の注意

解析サンプルは、同期済みD3D12 textureをCPUへreadbackしてOpenCVで処理します。

```text
GPU capture
  -> GPU to CPU readback
  -> OpenCV analysis
  -> CPU grid composition
  -> display / VideoWriter
```

高解像度・高fps・多数カメラではreadbackとCPU解析がボトルネックになります。性能が不足する場合は、D3D12ProcessingまたはDirectMLによるGPU解析へ置き換えてください。

## 実機確認項目

```text
各カメラを単独でopenできるか
全カメラをacquisition-paused状態で準備できるか
全カメラのAcquisitionStartが成功するか
TriggerSourceの実機名が正しいか
TriggerSoftware commandが実行できるか
host受信時刻差が許容値以内か
同期threadのdroppedFramesが継続的に増えないか
表示と録画のレイアウト・解析結果が一致するか
OpenCV VideoWriterが使用するcodecを環境が提供しているか
```
