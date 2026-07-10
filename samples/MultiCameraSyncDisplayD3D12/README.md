# MultiCameraSyncDisplayD3D12

複数のIC4カメラをD3D12 backendで取得し、`D3D12FrameSyncThread`が出力した同期frame setを1枚のcanvasへ並べて表示するサンプルです。

- 2台は左右並び
- 3台以上は正方形に近いgrid
- アスペクト比を維持してletterbox配置
- `--record`指定時は表示と同じcanvasをH.264 MP4へ記録
- `none` / `hardware` / `software` trigger mode
- `timestamp` / `frame-number` sync policy

## Deferred acquisition

全カメラを次の設定で先にopenします。

```cpp
config.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Deferred;
```

処理順:

```text
全camera: streamSetup(DeferAcquisitionStart)
D3D12FrameSyncThreadを開始
全camera worker threadを開始
全camera: startAcquisition()
```

終了時:

```text
全camera: stopAcquisition()
全camera worker: stopAndJoin()
sync thread: stopAndJoin()
```

これにより、1台目のframe転送中に2台目の`PayloadSize`を問い合わせる経路を避けます。

## 実行例

free-run、2台表示:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode none --sync-policy timestamp --max-timestamp-diff-ns 10000000 --format BayerRG8
```

外部HW trigger:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy timestamp --max-timestamp-diff-ns 4000000 --format BayerRG8 --fps 160
```

SW triggerは実験的です。

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode software --sync-policy timestamp --max-timestamp-diff-ns 10000000
```

表示と同じ映像を録画:

```bat
MultiCameraSyncDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --sync-policy timestamp --max-timestamp-diff-ns 4000000 --fps 160 --record synchronized.mp4
```

## 引数

```text
--devices 0,1[,2,...]
--trigger-mode none|hardware|software
--trigger-source Line1
--sync-policy timestamp|frame-number
--max-timestamp-diff-ns 10000000
--width 1536
--height 1536
--fps 160
--format BayerRG8
--camera-setup-delay-ms 1000
--camera-open-retries 3
--camera-retry-delay-ms 3000
--canvas-width 1600
--canvas-height 900
--sets 0
--record output.mp4
--record-bitrate 16000000
```

独立カメラではframe numberの起点が異なる場合があります。`frame-number`はcounter基準が一致することを保証できる場合だけ使用してください。通常は`timestamp + HostReceived`を使用します。

## 録画backend

```bat
-DIC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING:BOOL=ON
```

録画が不要な場合:

```bat
-DIC4EXT_MULTICAMERA_SAMPLE_ENABLE_RECORDING:BOOL=OFF
```

native D3D12 Video EncodeはGPUとdriverの対応状況に依存します。

## 実装上の注意

同期frameはGPU textureから`RGBA8` CPU frameへreadbackし、CPU上でcanvasへ合成します。表示はWin32/GDI、録画時は合成canvasをD3D12 RGBA8 textureへuploadしてD3DVideoEncoderへ渡します。

この経路は表示内容と録画内容の一致を優先しています。高解像度・高fpsでCPU負荷が問題になる場合は、D3D12ProcessingによるGPU grid compositeへ置き換えます。
