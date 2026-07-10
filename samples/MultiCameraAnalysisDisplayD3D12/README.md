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
-DOpenCV_DIR:PATH=C:\path\to\opencv\build
```

OpenCV はこのサンプルだけで使用します。IC4Ext 本体は OpenCV に依存しません。

## 実行例

同期なし、カメラ0と1を表示します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp
```

HW trigger を使います。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --fps 60
```

SW trigger を使います。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode software --sync-policy timestamp
```

解析済みの合成映像を MP4 に保存します。

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --fps 60 --canvas-width 1920 --canvas-height 1080 --record analyzed_sync.mp4
```

## 引数

```text
--devices 0,1[,2,...]
--trigger-mode none|hardware|software
--trigger-source Line1
--sync-policy timestamp|frame-number
--max-timestamp-diff-ns 1000000
--width 1920
--height 1080
--fps 60
--canvas-width 1920
--canvas-height 1080
--sets 1000
--motion-threshold 24
--min-motion-area 400
--record analyzed_sync.mp4
```

`Esc` または `Q` で終了します。

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
