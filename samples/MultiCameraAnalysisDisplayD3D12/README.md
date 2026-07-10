# MultiCameraAnalysisDisplayD3D12

複数のIC4カメラをD3D12で取得し、対応するフレームをOpenCVで解析しながらグリッド表示するサンプルです。`--record`を指定すると、表示と同じ解析済み合成フレームを保存します。

## 実機検証状況

```text
free-run + HostReceived : 利用可能。標準許容差10 ms
HW trigger + Line1      : 推奨。160 fps、許容差4 msで実機確認済み
SW trigger              : 実験的。安定同期を確認できていないため非推奨
```

検証済みHW条件では、次の結果を確認しています。

```text
Camera model         : DFK 33UX252 x2
Resolution           : 1536 x 1536
PixelFormat          : BayerRG8
Requested frame rate : 160 fps
OffsetX / OffsetY    : 236 / 0
Trigger source       : Line1
Frame matching       : TimestampNearest
Timestamp source     : HostReceived
Tolerance            : 4,000,000 ns
Validated sets       : 1,000
Maximum observed diff: 438,100 ns
syncDropped          : 1
camera0 read          : 1,002
camera1 read          : 1,002
camera0/1 timeouts   : 0 / 0
camera0/1 errors     : 0 / 0
```

さらに、同条件で8.5万組以上の長時間動作も確認しています。ここでいう4 msは`HostReceived`によるフレーム対応付けの許容差であり、露光開始時刻そのものの差を4 ms以内と保証する値ではありません。

## 検証済みmulti-camera acquisition lifecycle

現在のDFK 33UX252とIC4 1.6.0.894では、`DeferAcquisitionStart`経路で2台目が安定してフレームを出さない場合がありました。そのため、実運用サンプルでは次の互換prepare-stop方式を使用します。

```text
camera 0: 通常のstreamSetupでopen
camera 0: AcquisitionStop command
camera 1: 通常のstreamSetupでopen
camera 1: AcquisitionStop command
...
sync threadを開始
全camera worker threadを開始
全camera: AcquisitionStart command
```

公開APIからは文字列commandを直接呼ばず、型付きAPIを使用します。

```cpp
cameraThread.startAcquisition();
cameraThread.stopAcquisition();
```

D3D11/D3D12のIC4Ext所有captureでは、workerのblocking readが制御commandを妨げないようにしています。これにより、両カメラをほぼ同時に再開でき、起動時dropを1まで抑えた実機結果を確認しています。

`AcquisitionStartMode::Deferred`は実験的APIとして残していますが、現在の実機構成では推奨しません。

## 標準カメラ設定

標準で次を読み込みます。

```text
samples/MultiCameraAnalysisDisplayD3D12/config/gamma1.json
```

ビルド時には実行ファイル横の`config/gamma1.json`にもコピーされます。`devices[0].state`を指定した全カメラへ共通適用します。

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

JSON適用後にROI offsetを明示的に上書きします。

```text
OffsetAutoCenter = Off
OffsetX          = 236
OffsetY          = 0
```

## フレーム対応付け

各フレームをPCが受信した`std::chrono::steady_clock`時刻を比較します。

```text
FrameSyncPolicy          = TimestampNearest
FrameSyncTimestampSource = HostReceived
```

独立カメラのframe numberは取得開始位置やcounter初期値が異なるため、このサンプルでは`FrameNumberExact`を使用しません。`--sync-policy`で受け付ける値は`timestamp`だけです。

## ビルド

OpenCV 4.xを導入し、次を有効にしてconfigureします。

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
```

vcpkgを使う場合:

```bat
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

OpenCVはこのサンプルだけで使用します。IC4Ext本体はOpenCVへ依存しません。

## 実行

### free-run

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --max-timestamp-diff-ns 10000000
```

### HW trigger（推奨・実機確認済み）

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --max-timestamp-diff-ns 4000000 --fps 160
```

外部trigger信号は、全カメラの`Acquisition started`が表示された後に入力します。

### SW trigger（実験的・非推奨）

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode software --max-timestamp-diff-ns 10000000
```

現在は各カメラへ`TriggerSoftware`を順番に送るため、送信遅延とjitterを十分に抑えられていません。

### 録画

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --max-timestamp-diff-ns 4000000 --fps 160 --canvas-width 1920 --canvas-height 1080 --record analyzed_hw_sync.mp4 --record-fps 160
```

## 主な引数

```text
--devices 0,1[,2,...]
--trigger-mode none|hardware|software
--trigger-source Line1
--sync-policy timestamp
--max-timestamp-diff-ns 10000000

--ic4-json PATH
--ic4-json-device-index 0
--no-ic4-json
--offset-x 236
--offset-y 0

--format BayerRG8|BayerGR8|BayerGB8|BayerBG8|Mono8|BGR8|BGRa8
--width 1536
--height 1536
--fps 160

--camera-setup-delay-ms 1000
--camera-open-retries 3
--camera-retry-delay-ms 3000
--canvas-width 1920
--canvas-height 1080
--sets 1000
--motion-threshold 24
--min-motion-area 400
--record analyzed_sync.mp4
--record-fps 160
```

`--format`、`--width`、`--height`、`--fps`はJSONより優先されます。offsetはJSON適用後に上書きされます。

## 終了

OpenCVウィンドウを選択し、`Esc`または`Q`で終了します。終了時は次の順で停止します。

```text
全camera: stopAcquisition()
全camera worker: stopAndJoin()
sync thread: stopAndJoin()
```

コンソールの`Ctrl+C`では正常終了処理を通らないため避けてください。

## 統計ログ

```text
sets
syncInput
syncEmitted
syncDropped
syncIgnored
cameraN.read
cameraN.pushed
cameraN.timeouts
cameraN.errors
```

標準設定は`1536x1536 / 160 fps / BayerRG8`です。同一USB host controllerで帯域不足、`PayloadSize` timeout、frame dropが発生する場合は別controllerへ分けるか、fpsまたは解像度を下げてください。