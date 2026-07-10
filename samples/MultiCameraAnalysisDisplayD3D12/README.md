# MultiCameraAnalysisDisplayD3D12

同期済みの複数IC4カメラフレームをD3D12で取得し、OpenCVで解析しながらグリッド表示するサンプルです。

解析後の映像をそのまま表示し、`--record`を指定した場合は同じ解析済み合成フレームを動画へ保存します。

## 標準カメラ設定

このサンプルは、標準で次のファイルを読み込みます。

```text
samples/MultiCameraAnalysisDisplayD3D12/config/gamma1.json
```

ビルド時には実行ファイル横の`config/gamma1.json`にもコピーされます。

JSONの`devices[0].state`を、指定したすべてのカメラへ共通設定として適用します。標準設定の主要値は次です。

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

IC Capture 4固有のoverlay、表示倍率、録画保存先などはIC4Extでは使用しません。サンプル用JSONにはカメラの`state`だけを保持しています。

JSON適用後、ROI offsetを必ず次の値で上書きします。

```text
OffsetAutoCenter = Off
OffsetX          = 236
OffsetY          = 0
```

別の値を使う場合は`--offset-x`と`--offset-y`で上書きできます。

## host timerによる同期

各フレームを受信した時点の`std::chrono::steady_clock`時刻を使って対応付けます。

```text
camera 0 frame -> hostReceivedTime
camera 1 frame -> hostReceivedTime
                  |
                  +-> 時刻差がmaxTimestampDiffNs以内の組を選択
```

標準の許容差は、実機確認済みの10 msです。

```text
maxTimestampDiffNs = 10000000
```

独立したカメラのframe numberは、取得開始タイミングやcounter初期値によってずれるため、このサンプルでは`FrameNumberExact`を使用しません。

`--sync-policy`は互換性のため残していますが、受け付ける値は`timestamp`だけです。

## 複数カメラの起動ゲート

free-runカメラを1台ずつ通常起動すると、1台目の画像転送中に2台目が`PayloadSize`を問い合わせ、USB3Visionのcontrol transferがtimeoutする場合があります。

`--trigger-mode none`の場合も、stream準備中だけ一時的にSoftware Triggerを有効化します。

```text
camera 0: Software Triggerを一時設定
camera 0: deviceOpen + streamSetup
camera 0: AcquisitionStop
camera 1: Software Triggerを一時設定
camera 1: deviceOpen + streamSetup
camera 1: AcquisitionStop
...
全camera worker / sync threadを開始
全camera: TriggerMode=Off
全camera: AcquisitionStart
```

`TriggerMode=Off`へ戻した後は通常のfree-runです。JSON内の露出・gain設定は保持されます。

## ビルド

OpenCV 4.xを導入し、次を有効にしてconfigureします。

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
```

vcpkgを使う場合は次も指定します。

```bat
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

OpenCVはこのサンプルだけで使用します。IC4Ext本体はOpenCVに依存しません。

## 実行

標準のgamma1設定、offset `(236, 0)`、host timer 10 ms同期でカメラ0と1を表示します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none
```

設定ファイルを明示する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --ic4-json samples\MultiCameraAnalysisDisplayD3D12\config\gamma1.json --ic4-json-device-index 0 --offset-x 236 --offset-y 0 --max-timestamp-diff-ns 10000000
```

JSONを使わず、CLIだけで設定する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --no-ic4-json --format BayerRG8 --width 1280 --height 720 --fps 30 --offset-x 236 --offset-y 0
```

HW triggerを使う場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --max-timestamp-diff-ns 10000000
```

SW triggerを使う場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode software --max-timestamp-diff-ns 10000000
```

解析済み合成映像を保存する場合:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --canvas-width 1920 --canvas-height 1080 --record analyzed_sync.mp4
```

## 引数

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
--width 1920
--height 1080
--fps 60

--camera-setup-delay-ms 1000
--camera-open-retries 3
--camera-retry-delay-ms 3000
--canvas-width 1920
--canvas-height 1080
--sets 1000
--motion-threshold 24
--min-motion-area 400
--record analyzed_sync.mp4
```

`--format`、`--width`、`--height`、`--fps`を指定した値はJSONより優先されます。offsetは常にJSON適用後に上書きされます。

`Esc`または`Q`で正常終了します。コンソールの`Ctrl+C`ではなく、ウィンドウ側から終了してください。

## 解析内容

各カメラごとに次を計算し、映像へ重ねて表示します。

```text
平均輝度
直前フレームとの差分率
動領域の黄色矩形
動領域数
```

表示と録画には同じ解析済み`cv::Mat`を使用します。

## 統計ログ

30表示setごと、または同期setが1秒以上得られない場合に次を表示します。

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

## 帯域上の注意

標準のgamma1設定は`1536x1536 / 160 fps / BayerRG8`です。2台を同じUSB host controllerで動かす場合、理論データ量は大きくなります。`PayloadSize` timeout、frame drop、実効fps低下が発生する場合は、別host controllerへ分けるか、CLIでfpsまたは解像度を下げてください。
