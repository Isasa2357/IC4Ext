# MultiCameraAnalysisDisplayD3D12

同期済みの複数 IC4 カメラフレームを D3D12 で取得し、OpenCV で解析しながらグリッド表示するサンプルです。

解析後の映像をそのまま表示し、`--record` を指定した場合は同じ解析済み合成フレームを動画へ保存します。

## 解析内容

各カメラごとに次を計算します。

- グレースケール平均輝度
- 直前フレームとの差分率
- 差分から抽出した動領域
- 動領域数

表示には次を反映します。

- 動領域の黄色矩形
- 物理 device index
- 平均輝度
- Motion percentage
- 検出領域数

## host timerによる同期

このサンプルは、各フレームを受信した時点の `std::chrono::steady_clock` 時刻を使って対応付けます。

```text
camera 0 frame -> hostReceivedTime
camera 1 frame -> hostReceivedTime
                  |
                  +-> 時刻差が maxTimestampDiffNs 以内の組を選択
```

独立したカメラのframe numberは、カメラごとの取得開始タイミングや内部counterの初期値によってずれる可能性があります。そのため、このサンプルでは `FrameNumberExact` を使用しません。

`--sync-policy` は互換性のため残していますが、受け付ける値は `timestamp` だけです。`frame-number` を指定した場合は、誤った対応付けを避けるため理由付きで終了します。

IC4Ext本体の `FrameNumberExact` 機能は残っているため、frame counterが同じ基準に揃っていることを保証できる別用途では引き続き利用できます。

## 複数カメラの起動ゲート

free-runのカメラを1台ずつ通常起動すると、1台目の画像転送中に2台目が `PayloadSize` を問い合わせ、USB3Visionのcontrol transferがtimeoutする場合があります。

このサンプルは、`--trigger-mode none` の場合もstream準備中だけ一時的にSoftware Triggerを有効化します。これにより、`streamSetup`は完了してもTriggerSoftwareが送られるまでは画像転送が始まりません。

```text
camera 0: Software Triggerを一時設定
camera 0: deviceOpen + streamSetup
camera 0: AcquisitionStop
camera 1: Software Triggerを一時設定
camera 1: deviceOpen + streamSetup
camera 1: AcquisitionStop
...
全 camera worker / sync threadを開始
全 camera: TriggerMode=Off
全 camera: AcquisitionStart
```

`TriggerMode=Off`へ戻した後は通常のfree-runです。露出auto設定は起動ゲートによって変更しません。

`--trigger-mode software`または`hardware`の場合は、指定されたtrigger設定そのものが起動ゲートになるため、free-run用の解除処理は行いません。

## ビルド

OpenCV 4.x の CMake package が見つかる環境で、次を追加して configure します。

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
```

vcpkgを使う場合は、CMake toolchainを指定します。

```bat
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

OpenCV はこのサンプルだけで使用します。IC4Ext 本体は OpenCV に依存しません。

## 実行例

同期なし、カメラ0と1を表示します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp --format BayerRG8 --width 1280 --height 720 --fps 30 --camera-setup-delay-ms 1000 --camera-open-retries 3 --camera-retry-delay-ms 3000 --max-timestamp-diff-ns 100000000
```

HW trigger を使います。露光開始はhardware triggerで揃え、取得後のframe対応付けにはhost timerを使います。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy timestamp --format BayerRG8 --fps 60 --max-timestamp-diff-ns 10000000
```

SW trigger を使います。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode software --sync-policy timestamp --format BayerRG8 --max-timestamp-diff-ns 10000000
```

解析済みの合成映像を MP4 に保存します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy timestamp --format BayerRG8 --fps 60 --max-timestamp-diff-ns 10000000 --canvas-width 1920 --canvas-height 1080 --record analyzed_sync.mp4
```

## 引数

```text
--devices 0,1[,2,...]
--trigger-mode none|hardware|software
--trigger-source Line1
--sync-policy timestamp
--max-timestamp-diff-ns 100000000
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

旧引数 `--camera-start-delay-ms` と `--camera-start-retries` は互換aliasとして引き続き受け付けます。

`Esc` または `Q` で終了します。

## 統計ログ

30表示setごと、または同期setが1秒以上得られない場合に、次の統計を出力します。

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

これにより、カメラ取得、queue送信、frame同期、表示消費のどこで滞留したかを切り分けられます。

## 処理経路

```text
IC4 camera stream setup (software-trigger gated or native-trigger gated)
  -> acquisition paused
  -> all camera worker threads ready
  -> free-run only: TriggerMode=Off
  -> all camera AcquisitionStart
  -> D3D12CameraCaptureThread
  -> hostReceivedTime
  -> D3D12FrameSyncThread / TimestampNearest
  -> synchronized frame set
  -> D3D12FrameReadback
  -> OpenCV motion/luminance analysis
  -> analysis overlay
  -> grid composition
  -> imshow
  -> optional VideoWriter
```

表示と録画には同じ解析済み `cv::Mat` を使用するため、録画にも矩形と数値がそのまま含まれます。

## PayloadSize timeoutが残る場合

起動ゲート後もtimeoutする場合は、次を確認します。

- 2台を別のUSB host controllerへ接続する
- USB hubを介さず直接接続する
- `--format BayerRG8` または `--format Mono8` を使う
- 解像度とfpsを下げる
- 各カメラが単体で同じformat・解像度・fpsを使用できるか確認する
- IC Capture 4など、別プロセスがカメラを開いていないか確認する
