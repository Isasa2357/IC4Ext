# 複数カメラの同期表示・録画・解析

この章では、複数のIC4カメラから取得したフレームを対応付け、1枚の合成映像として表示・録画しながら解析する方法を説明します。

## 対応範囲

IC4Ext v1.0.1では、D3D12 backendを使って次の構成を利用できます。

```text
複数IC4 camera
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

取得後のframe set化はIC4Ext本体では次から選べます。

```text
TimestampNearest : timestamp差が許容値以内のフレームを組にする
FrameNumberExact : frame numberが完全一致するフレームを組にする
```

`FrameNumberExact`は、全カメラのframe counterが同じ起点に揃っていることを保証できる場合だけ使用します。独立したカメラでは開始タイミングにより一定offsetが付くため、解析サンプルでは使用しません。

## timestamp source

`FrameSyncOptions::timestampSource`で、`TimestampNearest`が比較する時計を選択します。

```cpp
IC4Ext::FrameSyncOptions syncOptions;
syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
syncOptions.cameraIndices = {0, 1};
syncOptions.maxTimestampDiffNs = 10'000'000;
syncOptions.maxBufferedFramesPerCamera = 32;
```

選択肢は次のとおりです。

```text
Auto         : hostReceivedTimeを優先し、利用できない場合device timestampへfallback
HostReceived : 同一プロセスのsteady_clockで記録した受信時刻を比較
Device       : cameraのdeviceTimestampNsを直接比較
```

独立したUSBカメラのdevice timestampは、counterの原点がカメラごとに異なる場合があります。free-runでは`HostReceived`を使用します。

解析サンプルの標準許容差は、実機確認済みの10 msです。

```text
maxTimestampDiffNs = 10000000
```

## gamma1標準設定

`MultiCameraAnalysisDisplayD3D12`は標準で次を読み込みます。

```text
samples/MultiCameraAnalysisDisplayD3D12/config/gamma1.json
```

ビルド時には実行ファイル横の`config/gamma1.json`へコピーされます。

JSONの`devices[0].state`を、指定したすべてのカメラへ共通設定として適用します。主要値は次です。

```text
Width                = 1536
Height               = 1536
AcquisitionFrameRate = 160
PixelFormat           = BayerRG8
ExposureAuto          = Off
ExposureTime          = 2000 us
GainAuto              = Off
Gain                  = 12
Gamma                 = 1.0
```

IC Capture 4固有のoverlay、表示倍率、保存先などは使用せず、カメラstateだけを保持しています。

JSON適用後、ROI offsetを明示的に上書きします。

```text
OffsetAutoCenter = Off
OffsetX          = 236
OffsetY          = 0
```

コードでは次の順で設定されます。

```cpp
IC4Ext::CameraCaptureConfig config;
config.ic4StateJson.path = ".../gamma1.json";
config.ic4StateJson.deviceIndex = 0;
config.ic4StateJson.strict = false;
config.ic4StateJson.applyNestedSelectorStates = true;

config.streamRequest.offsetX = 236;
config.streamRequest.offsetY = 0;
```

`streamRequest`のWidth、Height、fpsを0のままにするとJSON値を使用します。CLIで指定した値はJSONより優先されます。

## カメラ同期設定

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

`Line1`は機種によって異なる可能性があります。実機property mapで利用可能な`TriggerSource`を確認してください。

### SW trigger

```cpp
IC4Ext::CameraCaptureConfig config;
IC4Ext::ConfigureSoftwareTriggerSync(config);

capture.open(selector, config, backend);
capture.softwareTrigger();
```

複数台へ順番にcommandを送るため、SW triggerは厳密な同時露光を保証しません。

## 複数カメラの起動ゲート

free-runカメラを1台ずつ通常起動すると、1台目の画像転送中に2台目が`PayloadSize`を問い合わせ、USB3Visionのcontrol transferがtimeoutする場合があります。

解析サンプルは、`--trigger-mode none`の場合もstream準備中だけSoftware Triggerを一時的に有効化します。

```text
camera 0: Software Triggerを一時設定
camera 0: deviceOpen + streamSetup
camera 0: AcquisitionStop
camera 1: Software Triggerを一時設定
camera 1: deviceOpen + streamSetup
camera 1: AcquisitionStop
...
D3D12FrameSyncThreadを開始
全camera worker threadを開始
全camera: TriggerMode=Off
全camera: AcquisitionStart
```

解除後は通常のfree-runです。gamma1内の露出・gain設定は保持されます。

HW triggerの場合、外部trigger信号は全カメラの`AcquisitionStart`成功後に入力してください。

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

解析済みの各映像をグリッドへ配置し、その同じ合成`cv::Mat`を表示と録画へ渡します。

## ビルド

vcpkgを使用する場合:

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

## 標準実行

標準gamma1設定、offset `(236, 0)`、host timer 10 ms同期を使用します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none
```

設定をすべて明示する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --ic4-json samples\MultiCameraAnalysisDisplayD3D12\config\gamma1.json --ic4-json-device-index 0 --offset-x 236 --offset-y 0 --max-timestamp-diff-ns 10000000
```

JSONを無効化する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --no-ic4-json --format BayerRG8 --width 1280 --height 720 --fps 30 --offset-x 236 --offset-y 0
```

HW triggerで実行する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --max-timestamp-diff-ns 10000000
```

録画する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --canvas-width 1920 --canvas-height 1080 --record analyzed_sync.mp4
```

解析サンプルはhost timer同期に固定されています。`--sync-policy frame-number`を指定した場合はエラー終了します。

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

## 性能とUSB帯域

標準gamma1設定は`1536x1536 / 160 fps / BayerRG8`です。2台を同じUSB host controllerで動かす場合、要求帯域が大きくなります。

```text
PayloadSize timeout
frame drop
実効fps低下
```

が発生する場合は、別host controllerへ分けるか、CLIでfpsまたは解像度を下げてください。

解析サンプルは同期済みD3D12 textureをCPUへreadbackしてOpenCVで処理します。高解像度・高fpsではreadbackとCPU解析もボトルネックになります。

## 実機確認項目

```text
各カメラを単独でopenできるか
全カメラをtrigger-gated / acquisition-paused状態で準備できるか
全カメラのAcquisitionStartが成功するか
OffsetX=236 / OffsetY=0が反映されるか
host受信時刻差が10 ms以内で対応付けられるか
同期threadのdroppedFramesが継続的に増えないか
表示と録画のレイアウト・解析結果が一致するか
EscまたはQで正常終了できるか
```
