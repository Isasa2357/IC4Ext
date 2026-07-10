# 複数カメラの同期表示・録画・解析

この章では、複数の IC4 カメラから取得したフレームを同期し、1枚の合成映像として表示・録画しながら解析する方法を説明します。

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
None             : free-run。取得後に timestamp または frame number で対応付ける
HardwareTrigger  : 外部入力信号で露光開始を揃える
SoftwareTrigger  : PC から TriggerSoftware command を各カメラへ送る
```

取得後の frame set 化は次から選びます。

```text
TimestampNearest : timestamp 差が許容値以内のフレームを組にする
FrameNumberExact : frame number が完全一致するフレームを組にする
```

HW trigger では `FrameNumberExact`、free-run または SW trigger では `TimestampNearest` を最初に試すことを推奨します。

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

`Line1` はカメラ機種によって異なる可能性があります。実機 property map で利用可能な `TriggerSource` を確認してください。

### SW trigger

```cpp
IC4Ext::CameraCaptureConfig config;
IC4Ext::ConfigureSoftwareTriggerSync(config);

capture.open(selector, config, backend);
capture.softwareTrigger();
```

複数台へ順番に command を送るため、SW trigger は厳密な同時露光を保証しません。実際の timestamp 差を計測してください。

## 複数カメラの安全な起動

`D3D12CameraCaptureThread::start()` は内部でカメラをopenし、IC4の `streamSetup(... AcquisitionStart)` まで実行します。この間、IC4はカメラから `PayloadSize` などを問い合わせます。

1台目を高帯域でstreamingした直後に2台目を初期化すると、USB controller、USB hub、camera transportの状態によっては次のtimeoutが発生することがあります。

```text
Failed to query payload size from device
PayloadSize read failed (...: Timeout)
```

複数カメラサンプルでは次を行います。

```text
カメラを1台ずつ順番に起動
起動間に待機時間を入れる
起動失敗時に再試行する
slot / deviceIndex / attempt番号をログへ出す
raw Bayer / Mono入力を選択可能にする
```

解析サンプルの既定値は次です。

```text
format                 = BayerRG8
camera-start-delay-ms  = 2000
camera-start-retries   = 3
camera-retry-delay-ms  = 3000
```

高解像度・高fpsのUSBカメラを2台使用する場合は、まず安全側で次を指定してください。

```bat
--format BayerRG8 --camera-start-delay-ms 5000 --camera-start-retries 3 --camera-retry-delay-ms 5000 --width 1280 --height 720 --fps 30
```

`BGR8` は3 byte/pixelのため、raw BayerまたはMono入力より転送量が増えます。カメラが対応している場合、複数台では `BayerRG8` などの1 byte/pixel formatを優先してください。

同じtimeoutが続く場合は、次を確認します。

```text
2台を別のUSB controllerへ接続する
USB hubを避けてPCへ直接接続する
解像度またはfpsを下げる
カメラが実際に対応するPixelFormatを指定する
1台ずつ単独起動して正常性を確認する
```

## 同期 thread

```cpp
IC4Ext::FrameSyncOptions syncOptions;
syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
syncOptions.cameraIndices = {0, 1};
syncOptions.maxTimestampDiffNs = 1'000'000;
syncOptions.maxBufferedFramesPerCamera = 32;

IC4Ext::D3D12FrameSyncThread syncThread(
    inputQueue,
    outputQueue,
    syncOptions);

syncThread.start();
```

カメラ thread からは、camera index を付けて同じ indexed queue へ出力します。

```cpp
camera0.addOutputQueue(0, inputQueue);
camera1.addOutputQueue(1, inputQueue);
```

## 同期表示と録画

`MultiCameraSyncDisplayD3D12` は、同期済みフレームをアスペクト比を保ったままグリッドへ配置します。

- 2台: 左右並び
- 3台以上: 正方形に近いグリッド
- 表示と録画は同じ合成 canvas を使用
- `--record` を指定した場合だけ録画

例:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --fps 60 --canvas-width 1920 --canvas-height 1080 --record synchronized.mp4
```

## 解析しながら表示・録画

`MultiCameraAnalysisDisplayD3D12` は OpenCV をサンプル内だけで利用します。IC4Ext 本体は OpenCV に依存しません。

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

解析済みの各映像をグリッドへ配置し、その同じ合成 `cv::Mat` を表示と録画へ渡します。そのため、録画にも解析結果が含まれます。

### ビルド

vcpkgを使用する場合:

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

### 実行

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp --format BayerRG8 --camera-start-delay-ms 5000 --max-timestamp-diff-ns 100000000
```

録画も行う場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --format BayerRG8 --camera-start-delay-ms 5000 --fps 60 --canvas-width 1920 --canvas-height 1080 --motion-threshold 24 --min-motion-area 400 --record analyzed_sync.mp4
```

## 性能上の注意

解析サンプルは、同期済み D3D12 texture を CPU へ readback して OpenCV で処理します。

```text
GPU capture
  -> GPU to CPU readback
  -> OpenCV analysis
  -> CPU grid composition
  -> display / VideoWriter
```

高解像度・高fps・多数カメラでは readback と CPU 解析がボトルネックになります。まず解析内容と同期精度を確認する参照実装として使用し、性能が不足する場合は D3D12Processing または DirectML による GPU 解析へ置き換えてください。

## 実機確認項目

```text
各カメラを単独でopenできるか
複数カメラ起動時にPayloadSize timeoutが発生しないか
TriggerSource の実機名が正しいか
TriggerSoftware command が実行できるか
frame number / timestamp 差が期待範囲か
同期 thread の droppedFrames が増え続けないか
表示と録画のレイアウト・解析結果が一致するか
OpenCV VideoWriter が使用する codec を環境が提供しているか
```
