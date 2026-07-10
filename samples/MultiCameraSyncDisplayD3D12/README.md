# MultiCameraSyncDisplayD3D12

複数の IC4 カメラを D3D12 backend で取得し、`D3D12FrameSyncThread` が出力した同期 frame set を1枚の canvasへ並べて表示するサンプルです。

- 2台は左右並び
- 3台以上は台数に応じた正方形に近い grid
- 各映像はアスペクト比を維持して letterbox 配置
- `--record` 指定時は、画面表示と同じ合成 canvas を H.264 MP4へ記録
- `none` / `hardware` / `software` trigger mode
- `timestamp` / `frame-number` sync policy

## 実行例

同期なし、2台表示:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp
```

外部 hardware trigger:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number
```

software trigger:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode software --sync-policy timestamp
```

表示と同じ横並び映像を録画:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy frame-number --fps 60 --record synchronized.mp4
```

3台を 1920x1080 canvasへ配置:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1,2 --canvas-width 1920 --canvas-height 1080
```

## 引数

```text
--devices 0,1[,2,...]             使用する IC4 device index
--trigger-mode none|hardware|software
--trigger-source Line1             hardware trigger input line
--sync-policy timestamp|frame-number
--max-timestamp-diff-ns 1000000    TimestampNearest の許容差
--width 1920
--height 1080
--fps 60
--format BGR8                      IC4 input pixel format
--canvas-width 1600
--canvas-height 900
--sets 0                           0 は windowを閉じるまで継続
--record output.mp4                指定した場合のみ録画
--record-bitrate 16000000
```

## 録画 backend

`IC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING=ON` の場合、CMake が `D3DVideoEncoder` を取得し、native D3D12 Video Encode backend を有効にします。

```bat
-DIC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING:BOOL=ON
```

録画が不要なら dependency と build時間を減らせます。

```bat
-DIC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING:BOOL=OFF
```

native D3D12 Video Encode は GPU / driver の対応状況に依存します。録画開始時に backend 初期化エラーになる場合は、表示だけで実機同期を確認するか、D3DVideoEncoder の capability sample で H.264 / NV12 support を確認してください。

## 実装上の注意

同期 frame は GPU texture から `RGBA8` CPU frameへ readbackし、CPU上で同じ canvasへ合成しています。表示は Win32/GDI、録画時は合成 canvas を D3D12 RGBA8 textureへ uploadして D3DVideoEncoderへ渡します。

この経路は「表示内容と録画内容を完全に一致させる」ことを優先しています。高解像度・高fpsでCPU負荷が問題になる場合は、次段階として D3D12Processing によるGPU grid compositeへ置き換えます。
