# 02. IC4 device / stream 設計 v1.3

## 1. 目的

この文書は IC4 SDK を使って camera device を開き、device property を設定し、QueueSink から image buffer を取得する設計を定義する。

重要な前提:

- `requestedFormat` は device の `PixelFormat` property として設定する。
- `QueueSink::Config::acceptedPixelFormats` にも同じ format を指定する。
- IC4 SDK による暗黙の format conversion を避ける。
- `D3D11CameraCapture` は自前の worker thread を持たないが、IC4 QueueSink callback は使う。

## 2. 使用する IC4 header

実装では原則として C++ wrapper API を使う。

```cpp
#include <ic4/ic4.h>
```

必要に応じて個別 header を include してもよい。

```cpp
#include <ic4/DeviceEnum.h>
#include <ic4/Grabber.h>
#include <ic4/QueueSink.h>
#include <ic4/ImageBuffer.h>
#include <ic4/ImageType.h>
#include <ic4/Properties.h>
#include <ic4/PropertyConstants.h>
```

## 3. IC4 library lifetime

IC4 SDK の初期化 / 終了が必要な場合、IC4Ext は RAII wrapper を用意する。

```cpp
class IC4LibraryContext
{
public:
    IC4LibraryContext();
    ~IC4LibraryContext();

    IC4LibraryContext(const IC4LibraryContext&) = delete;
    IC4LibraryContext& operator=(const IC4LibraryContext&) = delete;
};
```

`D3D11CameraCapture` は内部に `std::shared_ptr<IC4LibraryContext>` を持つか、process-wide singleton として管理してよい。

## 4. Device selection

`IC4DeviceSelector` の解決は次の手順で行う。

```cpp
std::optional<ic4::DeviceInfo> resolveDevice(const IC4DeviceSelector& selector);
```

手順:

1. `ic4::DeviceEnum::enumDevices()` で device list を取得する。
2. `selector.serial` が非空なら、`DeviceInfo::serial()` と完全一致する device を探す。
3. `selector.uniqueName` が非空なら、`DeviceInfo::uniqueName()` と完全一致する device を探す。
4. `selector.deviceIndex >= 0` なら、その index の device を返す。
5. selector が空なら、list の先頭を返す。

失敗条件:

- device list が空。
- serial / uniqueName が見つからない。
- deviceIndex が範囲外。
- IC4 API が error を返した。

`modelName` は selector に含めない。model name は複数台で重複し得るため、初期実装では選択条件にしない。

## 5. PixelFormat mapping

`CameraPixelFormat` から `ic4::PixelFormat` への変換関数を実装する。

```cpp
ic4::PixelFormat toIC4PixelFormat(CameraPixelFormat fmt);
```

対応表:

```txt
Mono8    -> ic4::PixelFormat::Mono8
BayerRG8 -> ic4::PixelFormat::BayerRG8
BayerGR8 -> ic4::PixelFormat::BayerGR8
BayerGB8 -> ic4::PixelFormat::BayerGB8
BayerBG8 -> ic4::PixelFormat::BayerBG8
BGR8     -> ic4::PixelFormat::BGR8
BGRa8    -> ic4::PixelFormat::BGRa8
```

未対応 format はこの関数で error にする。

## 6. Device property 設定

`D3D11CameraCapture::open()` は device open 後、stream setup 前に property を設定する。

必須:

```txt
PixelFormat = requestedFormat
```

任意指定:

```txt
Width  = config.streamRequest.width  if width  > 0
Height = config.streamRequest.height if height > 0
FPS    = config.streamRequest.fps    if fps    > 0
```

IC4 SDK の property 名は SDK の定数を使う。

```cpp
ic4::PropId::PixelFormat
```

`Width` / `Height` / `AcquisitionFrameRate` などは IC4 SDK の property wrapper / PropId を確認して使う。device が該当 property を持たない、または設定できない場合は open 失敗にする。

`PixelFormat` 設定例の方針:

```cpp
ic4::PropertyMap props = grabber.devicePropertyMap(err);
props.setValue(ic4::PropId::PixelFormat, ic4PixelFormat, err);
```

実際の API 名は IC4 SDK header に合わせること。設計上の要件は「QueueSink ではなく device property を設定する」ことである。

## 7. QueueSink 設定

`QueueSink::Config` は次のように設定する。

```cpp
ic4::QueueSink::Config sinkConfig;
sinkConfig.acceptedPixelFormats.push_back(toIC4PixelFormat(requestedFormat));
```

`acceptedPixelFormats` を空にしない。空にすると全 format を受け入れてしまい、device format と sink format のずれを検出しにくくなる。

`maxOutputBuffers` は IC4 SDK 内部 queue の上限である。IC4Ext は callback で速やかに `popOutputBuffer()` し、IC4Ext 内部 pending queue へ移すため、初期実装では次の方針にする。

```txt
FrameQueuePolicy::LatestOnly     -> sinkConfig.maxOutputBuffers = 2
FrameQueuePolicy::PreserveFrames -> sinkConfig.maxOutputBuffers = 0 または十分大きい値
```

`LatestOnly` で 2 にする理由は、callback 処理中に次 frame が入っても過度に詰まりにくくするためである。IC4Ext 内部 pending queue では最新 1 枚だけを保持する。

## 8. QueueSinkListener 実装

`D3D11CameraCapture` は内部 listener を持つ。

```cpp
class InternalQueueSinkListener : public ic4::QueueSinkListener
{
public:
    bool sinkConnected(ic4::QueueSink& sink,
                       const ic4::ImageType& imageType,
                       size_t minBuffersRequired) override;

    void sinkDisconnected(ic4::QueueSink& sink) override;

    void framesQueued(ic4::QueueSink& sink) override;
};
```

### 8.1 sinkConnected

`sinkConnected()` では negotiated image type を検証する。

検証内容:

1. `imageType.pixel_format()` が `requestedFormat` と一致すること。
2. width / height が config と矛盾しないこと。
3. row pitch や image size を取得できる場合は metadata に保存すること。

`imageType.pixel_format()` が `requestedFormat` と一致しない場合、原則 `false` を返して stream setup を失敗させる。

### 8.2 framesQueued

`framesQueued()` は IC4 SDK の dedicated thread から呼ばれる。ここで重い GPU 処理をしてはならない。

行う処理:

1. `sink.isCancelRequested()` を適宜確認する。
2. `sink.popOutputBuffer()` を呼び、取得できる image buffer を取り出す。
3. buffer と受信時刻を `PendingIC4Frame` に包む。
4. `D3D11CameraCapture` 内部の mutex 付き pending queue へ push する。
5. condition variable で `read()` を起こす。

禁止事項:

- callback 内で D3D11 upload / compute shader dispatch を行わない。
- callback 内で長時間 block しない。
- callback 内で `streamStop()` を待つような処理を行わない。

### 8.3 pending queue

内部 pending frame は次のように表す。

```cpp
struct PendingIC4Frame
{
    std::shared_ptr<ic4::ImageBuffer> buffer;
    FrameTiming timing;
    FrameFormatMetadata format;
};
```

`std::shared_ptr<ic4::ImageBuffer>` を保持している間、IC4 buffer は sink の free queue に戻らない。したがって pending queue のサイズは必ず管理する。

`LatestOnly` の場合:

```txt
新 frame 到着時:
  古い pending frame をすべて破棄
  新 frame だけ保持
```

`PreserveFrames` の場合:

```txt
新 frame 到着時:
  pending queue の末尾へ追加
  maxPendingBuffers を超えた場合は古い frame を drop し、drop count を増やす
```

## 9. read() から見る pending queue

`read(ReadMode::LatestFrame)`:

```txt
pending queue が空なら timeout まで待つ
複数ある場合は最後の frame だけ取り出す
それ以前の frame は破棄する
```

`read(ReadMode::NextFrame)`:

```txt
pending queue が空なら timeout まで待つ
最古 frame を 1 枚取り出す
他の frame は残す
```

`read()` の timeout は `CameraReadOptions` または config で指定できるようにする。

```cpp
struct CameraReadOptions
{
    ReadMode mode = ReadMode::LatestFrame;
    std::uint32_t timeoutMs = 1000;
};
```

簡易 API として次も提供してよい。

```cpp
ReadResult read(ReadMode mode = ReadMode::LatestFrame);
ReadResult read(const CameraReadOptions& options);
```

## 10. stream setup / start / stop

`open()` の順序:

```txt
1. IC4 library initialize
2. DeviceEnum::enumDevices
3. selector resolve
4. Grabber create
5. deviceOpen(DeviceInfo)
6. device property 設定: PixelFormat, Width, Height, FPS
7. QueueSink listener create
8. QueueSink::create(listener, sinkConfig)
9. grabber.streamSetup(queueSink, AcquisitionStart)
10. negotiated format 検証
11. D3D11 converter 初期化
```

`close()` の順序:

```txt
1. acquisitionStop 可能なら実行
2. streamStop 可能なら実行
3. pending queue を clear
4. QueueSink / listener / Grabber を release
5. D3D11 resources を release
```

`streamStop()` は `framesQueued()` callback の終了を待つ可能性があるため、callback 内から `close()` を呼んではならない。

## 11. frame metadata の取得

IC4 image buffer から取得できる情報は可能な限り `FrameTiming` / `FrameFormatMetadata` に入れる。

最低限:

- host received time
- width
- height
- actual input format
- input row pitch bytes

frame number / device timestamp が IC4 API から取得できる場合は入れる。取得できない場合は 0 とする。

## 12. 統計情報

`D3D11CameraCapture` は最低限次の stats を持つ。

```cpp
struct CameraCaptureStats
{
    std::uint64_t receivedBuffers = 0;
    std::uint64_t droppedPendingBuffers = 0;
    std::uint64_t readFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t conversionFailures = 0;
};
```

stats は thread-safe に取得できること。
