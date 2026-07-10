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

現在のDFK 33UX252とIC4 1.6.0.894では、`DeferAcquisitionStart`経路で2台目が安定してフレームを出さない場合がありました。そのため、複数カメラの実運用では次のprepare-stop方式を使用します。

```text
camera 0: Immediateでopen
camera 0: AcquisitionStop command
camera 1: Immediateでopen
camera 1: AcquisitionStop command
...
sync threadを開始
全camera worker threadを開始
全camera: AcquisitionStart command
```

実装では型付きAPIを使用します。

```cpp
IC4Ext::CameraCaptureConfig config0;
IC4Ext::CameraCaptureConfig config1;
config0.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;
config1.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;

capture0.open(selector0, config0, backend);
capture0.setIC4Property("AcquisitionStop", std::string("execute"));

capture1.open(selector1, config1, backend);
capture1.setIC4Property("AcquisitionStop", std::string("execute"));

IC4Ext::D3D12CameraCaptureThread camera0(std::move(capture0), backend);
IC4Ext::D3D12CameraCaptureThread camera1(std::move(capture1), backend);

syncThread.start();
camera0.start();
camera1.start();

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

IC4Ext自身が所有するcaptureでは、workerのblocking readが`startAcquisition()`や`stopAcquisition()`を妨げないようにしています。外部から渡された`ID3D*Camera`実装だけは、互換性のため制御とreadを直列化します。

`AcquisitionStartMode::Deferred`は実験的APIとして残していますが、この実機構成では推奨しません。

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

複数カメラの準備中にfree-run転送を止める場合は、一時的なsoftware-trigger gateを使用し、全worker準備後に`TriggerMode=Off`へ戻します。

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

最終integration test:

```text
Synchronized sets     : 1,000
Maximum observed diff : 438,100 ns
syncInput              : 2,004
syncEmitted            : 1,001
syncDropped            : 1
camera0 read           : 1,002
camera1 read           : 1,002
camera0/1 timeouts     : 0 / 0
camera0/1 errors       : 0 / 0
```

別の長時間試験では8.5万組以上を生成し、両カメラのread errorは0でした。

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

JSON適用後、offsetは明示値で上書きされます。同一の`devices[0].state`を両カメラへ適用できます。

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

`test_camera2plus_frame_sync_smoke`は、検証済みprepare-stop方式で2台を準備し、同期setを取得開始直後から消費します。各GPU frameのready fenceも待機します。

HW trigger:

```bat
set "IC4EXT_TEST_IC4_JSON=%CD%\samples\MultiCameraAnalysisDisplayD3D12\config\gamma1.json"
set "IC4EXT_TEST_IC4_JSON_DEVICE_INDEX=0"
set "IC4EXT_TEST_FPS=160"
set "IC4EXT_TEST_OFFSET_X=236"
set "IC4EXT_TEST_OFFSET_Y=0"
set "IC4EXT_TEST_HW_TRIGGER=1"
set "IC4EXT_TEST_TRIGGER_SOURCE=Line1"
set "IC4EXT_TEST_SYNC_TOLERANCE_NS=4000000"
set "IC4EXT_TEST_SYNC_SETS=1000"
set "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS=60"
set "IC4EXT_TEST_INTER_CAMERA_DELAY_MS=1000"
set "IC4EXT_TEST_GPU_READY_TIMEOUT_MS=5000"
ctest --test-dir out\build\default -C Debug -R "^test_camera2plus_frame_sync_smoke$" -V
```

テストは次を検証します。

```text
2台のGPU textureが存在する
GPU ready fenceが完了する
camera index 0/1が含まれる
HostReceived差が許容値以内
read errorが0
AcquisitionStopが成功する
workerとsync threadが正常終了する
```

## 性能とUSB帯域

`1536x1536 / 160 fps / BayerRG8`を2台で使用すると要求帯域が大きくなります。

```text
PayloadSize timeout
frame drop
実効fps低下
```

が発生する場合は、別USB host controllerへ分けるか、fpsまたは解像度を下げます。解析サンプルはGPU textureをCPUへreadbackしてOpenCVで処理するため、表示set数はcamera取得set数より少なくなる場合があります。
