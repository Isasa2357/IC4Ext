# IC4DeviceDiagnostics

IC4で列挙されたカメラの識別情報とtransport情報を表示し、指定したカメラを`deviceOpen`して主要なGenICam propertyを直接読み出す診断サンプルです。

このサンプルは、カメラが列挙されているにもかかわらず`streamSetup`で次のようなエラーになる場合の切り分けに使用します。

```text
Failed to query payload size from device
PayloadSize read failed (...: Timeout)
```

## ビルド

```bat
cmake --build out\build\default --config Debug --target IC4DeviceDiagnostics --parallel
```

## 全カメラの列挙

```bat
IC4DeviceDiagnostics.exe
```

次を表示します。

```text
index
model
serial
uniqueName
firmware/device version
userID
interface display name
transport layer name/version/type
```

## device indexでprobe

```bat
IC4DeviceDiagnostics.exe --device-index 1
```

## serialでprobe

`device index`は再接続後に変わる可能性があるため、実運用ではserial指定を推奨します。

```bat
IC4DeviceDiagnostics.exe --serial 12345678
```

probeでは次を直接読みます。

```text
Width
Height
PixelFormat
AcquisitionFrameRate
PayloadSize
DeviceStreamChannelCount
DeviceStreamChannelPacketSize
DeviceLinkThroughputLimit
DeviceLinkThroughputLimitMode
DeviceTLType
```

`deviceOpen: success`の後に`PayloadSize`だけがtimeoutする場合、カメラはOS/IC4には列挙されていますが、stream構築に必要なGenICam register readへ応答できていません。この場合はIC4ExtのD3D処理やFrameSyncThreadより前の問題です。

確認対象は次です。

```text
意図したserialが選択されているか
別プロセスがカメラを使用していないか
USB cable/port/controllerを入れ替えたとき問題がcameraとportのどちらへ追従するか
カメラの電源再投入後にPayloadSizeが読めるか
IC Capture 4でも同じカメラだけlive開始に失敗するか
firmwareとIC4 transport layer versionが2台で一致しているか
```
