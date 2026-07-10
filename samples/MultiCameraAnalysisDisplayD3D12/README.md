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
- Camera index
- 平均輝度
- Motion percentage
- 検出領域数

## ビルド

OpenCV 4.x の CMake package が見つかる環境で、次を追加して configure します。

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
-DCMAKE_TOOLCHAIN_FILE:FILEPATH=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET:STRING=x64-windows
```

OpenCV はこのサンプルだけで使用します。IC4Ext 本体は OpenCV に依存しません。

## 実行例

同期なし、カメラ0と1を表示します。複数USBカメラ向けの安全側の例として、転送量が小さい `BayerRG8` と起動間隔5秒を指定しています。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp --format BayerRG8 --camera-start-delay-ms 5000 --max-timestamp-diff-ns 100000000
```

HW trigger を使います。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --format BayerRG8 --camera-start-delay-ms 5000 --fps 60
```

SW trigger を使います。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode software --sync-policy timestamp --format BayerRG8 --camera-start-delay-ms 5000
```

解析済みの合成映像を MP4 に保存します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --format BayerRG8 --camera-start-delay-ms 5000 --fps 60 --canvas-width 1920 --canvas-height 1080 --record analyzed_sync.mp4
```

## 引数

```text
--devices 0,1[,2,...]
--trigger-mode none|hardware|software
--trigger-source Line1
--sync-policy timestamp|frame-number
--max-timestamp-diff-ns 1000000
--format BayerRG8|BayerGR8|BayerGB8|BayerBG8|Mono8|BGR8|BGRa8
--width 1920
--height 1080
--fps 60
--camera-start-delay-ms 2000
--camera-start-retries 3
--camera-retry-delay-ms 3000
--canvas-width 1920
--canvas-height 1080
--sets 1000
--motion-threshold 24
--min-motion-area 400
--record analyzed_sync.mp4
```

`Esc` または `Q` で終了します。

## 複数カメラ起動について

IC4 の `streamSetup` 中は、カメラから `PayloadSize` などの情報を読み出します。1台目が高帯域でstreaming中に2台目を直ちに初期化すると、USB controllerやcamera transportの状態によっては次のようなtimeoutが発生することがあります。

```text
Failed to query payload size from device
PayloadSize read failed (...: Timeout)
```

このサンプルは次の対策を行います。

- カメラを1台ずつ順番に起動
- 起動間に既定2秒の待機
- 起動失敗時に既定3回まで再試行
- device indexとattempt番号をログ出力
- 既定入力formatを1 byte/pixelの `BayerRG8` に設定

同じtimeoutが続く場合は、最初に次を試してください。

```bat
--camera-start-delay-ms 5000 --camera-start-retries 3 --camera-retry-delay-ms 5000 --format BayerRG8 --width 1280 --height 720 --fps 30
```

それでも失敗する場合は、2台を別のUSB controllerへ接続する、USB hubを避ける、またはカメラが対応するraw/Mono formatを指定してください。

## 処理経路

```text
IC4 camera frames
  -> D3D12CameraCaptureThread
  -> D3D12FrameSyncThread
  -> synchronized frame set
  -> D3D12FrameReadback
  -> OpenCV motion/luminance analysis
  -> analysis overlay
  -> grid composition
  -> imshow
  -> optional VideoWriter
```

表示と録画には同じ解析済み `cv::Mat` を使用するため、録画にも矩形と数値がそのまま含まれます。
