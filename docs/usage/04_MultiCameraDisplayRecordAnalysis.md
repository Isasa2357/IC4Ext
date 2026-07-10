# 複数カメラの同期表示・録画・解析

IC4Ext v1.0.1では、D3D12 backendで複数カメラを準備し、対応するフレームを同期setとして取り出せます。

```text
複数IC4 camera
  -> D3D12CameraCaptureThread
  -> D3D12IndexedFrameQueue
  -> D3D12FrameSyncThread
  -> D3D12SyncedFrameSet
  -> 表示 / 録画 / 解析
```

## Acquisition lifecycle

単一カメラとの互換性を維持するため、既定値は即時開始です。

```cpp
IC4Ext::CameraCaptureConfig config;
config.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;
```

複数カメラでは`Deferred`を使用します。

```cpp
IC4Ext::CameraCaptureConfig config;
config.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Deferred;
```

この場合、`open()`はIC4の次の経路を使用します。

```cpp
streamSetup(sink, ic4::StreamSetupOption::DeferAcquisitionStart);
```

全カメラのstreamを準備してから、worker threadと同期threadを開始し、最後に取得を開始します。

```cpp
capture0.open(selector0, config0, backend);
capture1.open(selector1, config1, backend);

IC4Ext::D3D12CameraCaptureThread camera0(std::move(capture0), backend);
IC4Ext::D3D12CameraCaptureThread camera1(std::move(capture1), backend);

camera0.start();
camera1.start();
syncThread.start();

camera0.startAcquisition();
camera1.startAcquisition();
```

終了時は次の順です。

```cpp
camera0.stopAcquisition();
camera1.stopAcquisition();
camera0.stopAndJoin();
camera1.stopAndJoin();
syncThread.stopAndJoin();
```

状態確認には次を使用できます。

```cpp
capture.isStreaming();
capture.isAcquisitionActive();
```

## Trigger設定

### free-run

```cpp
IC4Ext::ConfigureNoSync(config);
```

### HW trigger

```cpp
IC4Ext::ConfigureHardwareTriggerSync(
    config,
    "Line1",
    "FrameStart",
    "RisingEdge");
```

外部trigger信号は、全カメラの`startAcquisition()`成功後に入力します。

### SW trigger

```cpp
IC4Ext::ConfigureSoftwareTriggerSync(config);
cameraThread.softwareTrigger();
```

SW triggerは各カメラへcommandを順番に送る現在の実装では、送信遅延とjitterを十分に抑えられていません。v1.0.1では実験的機能です。同期精度が必要な用途ではHW triggerを使用します。

## Frame synchronization

```cpp
IC4Ext::FrameSyncOptions syncOptions;
syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
syncOptions.cameraIndices = {0, 1};
syncOptions.maxTimestampDiffNs = 10'000'000;
syncOptions.maxBufferedFramesPerCamera = 32;
```

timestamp source:

```text
Auto         : hostReceivedTimeを優先し、なければdevice timestamp
HostReceived : 同一プロセスのsteady_clock時刻
Device       : cameraのdeviceTimestampNs
```

独立したUSBカメラではdevice timestampの原点が異なる場合があるため、free-runでは`HostReceived`を使用します。

`FrameNumberExact`は、全カメラのframe counterが同じ起点に揃っていることを保証できる場合だけ使用します。

## 実機確認済み条件

```text
Camera model         : DFK 33UX252 x2
Resolution           : 1536 x 1536
PixelFormat          : BayerRG8
Requested frame rate : 160 fps
OffsetX / OffsetY    : 236 / 0
Trigger mode         : Hardware
Trigger source       : Line1
Frame matching       : TimestampNearest
Timestamp source     : HostReceived
Tolerance            : 4,000,000 ns
```

この条件で8.5万組以上を生成し、両カメラのread errorは0でした。同期dropは起動時の位置合わせ後、ほぼ一定でした。

```text
free-run + HostReceived : 10 msで確認
HW trigger + Line1      : 4 msで確認・推奨
SW trigger              : 実験的・将来改善対象
```

4 msは`HostReceived`の対応付け許容差であり、露光開始差を直接保証する値ではありません。

## gamma1標準設定

`MultiCameraAnalysisDisplayD3D12`は次を標準読込します。

```text
samples/MultiCameraAnalysisDisplayD3D12/config/gamma1.json
```

主要値:

```text
Width                = 1536
Height               = 1536
AcquisitionFrameRate = 160
PixelFormat           = BayerRG8
ExposureTime          = 2000 us
Gain                  = 12
Gamma                 = 1.0
OffsetX               = 236
OffsetY               = 0
```

JSON適用後、offsetは明示値で上書きされます。

## 解析サンプル

OpenCVを使用するサンプルを有効化します。

```bat
-DIC4EXT_BUILD_OPENCV_ANALYSIS_SAMPLE:BOOL=ON
```

HW trigger実行例:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --max-timestamp-diff-ns 4000000 --fps 160
```

録画:

```bat
MultiCameraAnalysisDisplayD3D12.exe --devices 0,1 --trigger-mode hardware --trigger-source Line1 --max-timestamp-diff-ns 4000000 --fps 160 --canvas-width 1920 --canvas-height 1080 --record analyzed_hw_sync.mp4 --record-fps 160
```

解析内容:

```text
平均輝度
直前フレームとの差分率
動領域の矩形
動領域数
```

## 2カメラintegration test

`test_camera2plus_frame_sync_smoke`は、2台を`Deferred`でopenしてからworkerとacquisitionを開始します。

free-run:

```bat
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=10000000"
set "IC4EXT_TEST_SYNC_SETS=100"
ctest --test-dir out\build\default -C Debug -L camera2plus --output-on-failure
```

HW trigger:

```bat
set "IC4EXT_TEST_HW_TRIGGER=1"
set "IC4EXT_TEST_TRIGGER_SOURCE=Line1"
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=4000000"
set "IC4EXT_TEST_SYNC_SETS=1000"
set "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS=60"
ctest --test-dir out\build\default -C Debug -L camera2plus --output-on-failure
```

テストは各setのtexture、camera index、HostReceived差、read error、正常停止を検証します。

## 性能とUSB帯域

`1536x1536 / 160 fps / BayerRG8`を2台で使用すると要求帯域が大きくなります。

```text
PayloadSize timeout
frame drop
実効fps低下
```

が発生する場合は、別USB host controllerへ分けるか、fpsまたは解像度を下げます。解析サンプルはGPU textureをCPUへreadbackしてOpenCVで処理するため、表示set数はcamera取得set数より少なくなる場合があります。
