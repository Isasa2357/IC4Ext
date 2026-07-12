# 02. IC4 device / stream design

この文書は、IC4 SDKを使ってcamera deviceを選択し、propertyを設定し、QueueSinkからimage bufferを取得するbackend共通設計を定義する。

D3D11とD3D12はIC4 device/stream setupを共有するが、GPU frame公開方式は異なる。

```text
D3D11 -> D3D11CameraFrame
D3D12 -> IC4Ext::D3D12::ReadOnlyFrame
```

## 1. Important assumptions

- `requestedFormat`はdeviceの`PixelFormat` propertyとして設定する。
- `QueueSink::Config::acceptedPixelFormats`にも同じformatを指定する。
- IC4 SDKによる意図しないformat conversionを避ける。
- IC4 callback threadではGPU conversionや長時間処理を行わない。
- callbackはimage bufferとmetadataをpending queueへ移し、`read()`側がGPU conversionする。
- pending queueと完成GPU frame poolは別のresourceである。

## 2. IC4 headers

原則としてC++ wrapperを使う。

```cpp
#include <ic4/ic4.h>
```

必要に応じて個別headerを使う。

```cpp
#include <ic4/DeviceEnum.h>
#include <ic4/Grabber.h>
#include <ic4/QueueSink.h>
#include <ic4/ImageBuffer.h>
#include <ic4/ImageType.h>
#include <ic4/Properties.h>
#include <ic4/PropertyConstants.h>
```

## 3. Library lifetime

IC4 library初期化/終了はprocess内で整合したlifetimeを持つ。

camera instanceごとに独立initializationを繰り返すのではなく、RAII contextまたはprocess-wide管理を使う。

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

## 4. Device selection

`IC4DeviceSelector`の解決順:

```text
1. serial
2. uniqueName
3. deviceIndex
4. selectorが空ならfirst device
```

失敗条件:

```text
device listが空
serial/uniqueNameが見つからない
deviceIndexが範囲外
IC4 API error
```

model nameは複数台で重複し得るため、主selectorにしない。

## 5. PixelFormat mapping

```text
Mono8    -> ic4::PixelFormat::Mono8
BayerRG8 -> ic4::PixelFormat::BayerRG8
BayerGR8 -> ic4::PixelFormat::BayerGR8
BayerGB8 -> ic4::PixelFormat::BayerGB8
BayerBG8 -> ic4::PixelFormat::BayerBG8
BGR8     -> ic4::PixelFormat::BGR8
BGRa8    -> ic4::PixelFormat::BGRa8
```

未対応formatはopen/configuration errorにする。

## 6. CameraCaptureConfig

代表field:

```text
ic4StateJson
streamRequest
outputSpec
queuePolicy
maxPendingBuffers
shaderConfig
acquisitionStartMode
propertyOverrides
```

### Stream request

```text
width
height
fps
requestedFormat
forceRequestedFormat
offsetX
offsetY
```

### Output spec

```text
R8 / RGBA8
createSrv
createUav
```

D3D12 ReadOnly pipelineではconsumerがSRVとして読むため、camera公開frameはSRV-capableである必要がある。

## 7. Configuration order

推奨順:

```text
1. device open
2. IC4 JSON state
3. explicit PixelFormat / Width / Height / FPS / ROI overrides
4. trigger/property overrides
5. QueueSink creation
6. stream setup
7. negotiated image type validation
8. backend converter initialization
```

JSONとexplicit optionの優先順位はAPI/sampleごとに明示する。

## 8. IC4 JSON state

IC Capture 4からexportしたJSONは次を読む。

```text
devices[deviceIndex].state
```

```cpp
config.ic4StateJson.path = "camera_state.json";
config.ic4StateJson.deviceIndex = 0;
config.ic4StateJson.strict = false;
config.ic4StateJson.applyNestedSelectorStates = true;
```

nested selector propertyはselectorを切り替えながらscalar valueを適用する。array/register blob等、直接適用できない値はskipできる。

JSON内の次は実効capture rateへ影響する。

```text
PixelFormat
Width / Height
AcquisitionFrameRate
ExposureTime
TriggerMode / TriggerSource
```

## 9. Device property settings

必須または主要property:

```text
PixelFormat
Width
Height
AcquisitionFrameRate
OffsetX
OffsetY
ExposureAuto
ExposureTime
GainAuto
Gain
Gamma
TriggerSelector
TriggerMode
TriggerSource
TriggerActivation
```

deviceがpropertyを持たない、locked、range外の場合は`ErrorInfo`へ記録する。

strict JSON modeでは最初の適用失敗をopen failureにできる。non-strict modeではunsupported/locked propertyをskipできる。

## 10. Trigger configuration

helper:

```text
ConfigureNoSync
ConfigureHardwareTriggerSync
ConfigureSoftwareTriggerSync
```

hardware trigger例:

```text
TriggerSelector = FrameStart
TriggerMode = On
TriggerSource = Line1
TriggerActivation = RisingEdge
ExposureAuto = Off
```

IC4Extは外部trigger pulseを生成しない。160 fpsでhardware trigger captureするには、Line1へ実際に約160 Hzのpulseが必要である。

## 11. QueueSink configuration

```cpp
ic4::QueueSink::Config sinkConfig;
sinkConfig.acceptedPixelFormats.push_back(
    ToIC4PixelFormat(requestedFormat));
```

`acceptedPixelFormats`を空にしない。空だと意図しないIC4側変換を受け入れる可能性がある。

`maxOutputBuffers`とIC4Ext pending queueは別である。

## 12. QueueSink listener

callback:

```cpp
class InternalQueueSinkListener : public ic4::QueueSinkListener
{
public:
    bool sinkConnected(
        ic4::QueueSink&,
        const ic4::ImageType&,
        size_t minBuffersRequired) override;

    void sinkDisconnected(ic4::QueueSink&) override;
    void framesQueued(ic4::QueueSink&) override;
};
```

### sinkConnected

検証:

```text
negotiated pixel format
width / height
row pitch / image size
minimum buffers
```

### framesQueued

実行内容:

```text
popOutputBuffer
hostReceivedTime取得
FrameTiming/format/chunk metadata取得
PendingIC4Frameを作成
mutex付きpending queueへpush
condition variable notify
```

禁止事項:

```text
GPU upload/compute
readback
OpenCV
file write
long wait
streamStop/close
```

## 13. PendingIC4Frame

概念型:

```cpp
struct PendingIC4Frame
{
    std::shared_ptr<ic4::ImageBuffer> buffer;
    FrameTiming timing;
    FrameFormatMetadata format;
    FrameChunkMetadata chunkMetadata;
};
```

`ImageBuffer`を保持している間、IC4側bufferはfree queueへ戻らない。pending数は必ず制限する。

## 14. Queue policies

### LatestOnly

```text
new frame arrival
  -> old pending framesを破棄
  -> latest frameのみ保持
```

preview/低遅延直接read向け。

### PreserveFrames

```text
new frame arrival
  -> FIFO末尾へ追加
  -> maxPendingBuffers超過時に古いframeをdrop
```

`CameraCaptureThread`の`ReadMode::NextFrame`向け。

## 15. read()

### LatestFrame

```text
pending queueが空ならtimeoutまで待つ
latest 1 frameを取得
古いpending frameを破棄
GPU conversion
```

### NextFrame

```text
pending queueが空ならtimeoutまで待つ
oldest 1 frameを取得
残りを維持
GPU conversion
```

```cpp
struct CameraReadOptions
{
    ReadMode mode = ReadMode::LatestFrame;
    std::uint32_t timeoutMs = 1000;
};
```

## 16. D3D12 conversion and pool interaction

D3D12 `CameraCapture::read()`はpending IC4 bufferを取り出した後、次を行う。

```text
FramePool shape確認 / lazy initialization
FrameWriter acquire
PooledFrameConverter convert
producer queue signal
ReadOnlyFrame publish
```

FramePool acquireに失敗した場合、camera hardwareが停止していなくてもconsumerへframeを公開できない。`FramePoolStats::exhaustionDrops`とcapture timeoutを確認する。

10-output予備試験では、pool 16/64で枯渇し、128/256で解消した。

## 17. Stream lifecycle

基本順:

```text
open
streamSetup
acquisition start
read loop
acquisition stop
stream stop
close
```

multi-camera hardware trigger setupでは、cameraごとのopen/start timingと外部trigger開始時点をapplication側で管理する。

`AcquisitionStartMode::Deferred`はcamera/driver組合せによって動作差があるため、実機検証済みsequenceを優先する。

## 18. Runtime setters

open後に利用可能な代表API:

```text
setFrameRate
setExposureAuto
setExposureTime
setGainAuto
setGain
setGamma
setOffset
setRoi
setPixelFormat
setIC4Property
softwareTrigger
```

stream中にlockedされるpropertyはsetterがfalseを返す。必要ならacquisitionを停止してから変更する。

## 19. Metadata

最低限:

```text
host received time
width / height
actual input format
input row pitch
```

利用可能なら:

```text
device frame number
device timestamp
chunk block id
exposure
gain
camera-specific chunk fields
```

D3D12 frame synchronizationはframe numberを使わずtimestamp-nearestを使う。

## 20. Statistics

```text
receivedBuffers
droppedPendingBuffers
readFrames
readTimeouts
conversionFailures
```

D3D12では加えてFramePool statisticsを確認する。

```text
capacity
available
writing
published
exhaustionDrops
waitTimeouts
```

実効camera rateを判断するときは、`CameraPerformanceSnapshot`のIC4 stream statisticsとtimingも併用する。

## 21. Thread safety

```text
IC4 callback thread -> pending queue push
capture/read thread  -> pending queue pop + GPU conversion
control thread       -> property/lifecycle operations
```

`read()`の複数consumer同時呼び出しは想定しない。1つの`CameraCapture`に対して1つのread ownerを持つ。

callbackとclose/stream stopの競合に注意する。callback内からcloseしない。

## 22. Related documents

```text
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
docs/design/10_D3D12Backend.md
```
