# 01. Core types

この文書はIC4Extのbackend非依存Core型を整理する。

## 1. CameraPixelFormat

IC4から受け取る入力format。現在は8bit系のみ対応する。

```cpp
enum class CameraPixelFormat : std::uint32_t
{
    Mono8 = 1,
    BayerRG8,
    BayerGR8,
    BayerGB8,
    BayerBG8,
    BGR8,
    BGRa8,
};
```

## 2. GpuFrameFormat

D3D11 / D3D12 texture出力format。

```cpp
enum class GpuFrameFormat : std::uint32_t
{
    R8 = 1,
    RGBA8,
};
```

GPU上では3-channel textureを避け、color outputは`RGBA8`へ寄せる。

## 3. CpuFrameFormat

readback後のCPU format。

```cpp
enum class CpuFrameFormat : std::uint32_t
{
    Unknown = 0,
    Gray8,
    RGBA8,
    RGB8,
    BGR8,
};
```

`BGR8`はOpenCV `CV_8UC3`へ渡しやすい。

## 4. FrameTiming

```cpp
struct FrameTiming
{
    std::uint64_t frameNumber = 0;
    std::uint64_t deviceTimestampNs = 0;
    std::chrono::steady_clock::time_point hostReceivedTime{};
};
```

意味:

```text
frameNumber
  IC4 metadataのdevice_frame_number。
  cameraごとにcounter domain/epochが異なる可能性がある。
  D3D12 ReadOnly FrameSyncThreadのmatchingには使用しない。

deviceTimestampNs
  IC4 metadataのdevice_timestamp_ns。
  camera間で同じclock domainの場合だけabsolute比較できる。

hostReceivedTime
  process-wide steady_clock上でframeを受け取った時刻。
  camera間で比較可能だがUSB転送やthread schedulingの影響を含む。
```

D3D12 ReadOnly synchronizationは`deviceTimestampNs`または`hostReceivedTime`によるtimestamp-nearestだけを使う。

D3D11 legacy synchronizationは別のpolicyを持つ場合があるため、backend別文書を参照する。

## 5. FrameFormatMetadata

```cpp
struct FrameFormatMetadata
{
    CameraPixelFormat requestedFormat;
    CameraPixelFormat actualInputFormat;
    GpuFrameFormat outputFormat;
    int width;
    int height;
    std::size_t inputRowPitchBytes;
};
```

保持内容:

```text
要求したcamera input format
実際のIC4 buffer format
GPU output format
frame width / height
input row pitch
```

## 6. FrameChunkMetadata

取得可能なIC4 chunk dataを`has*` flag付きで保持する。

```text
hasBlockId / blockId
hasExposureTime / exposureTimeUs
hasGain / gain
hasIMX174FrameId / imx174FrameId
hasIMX174FrameSet / imx174FrameSet
hasMultiFrameSetId / multiFrameSetId
hasMultiFrameSetFrameId / multiFrameSetFrameId
```

chunkが無効、propertyが存在しない、取得に失敗した場合は対応flagをfalseのままにする。

## 7. CpuFrame

```cpp
struct CpuFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CpuFrameFormat format = CpuFrameFormat::Unknown;
    std::uint32_t rowPitch = 0;
    std::vector<std::uint8_t> data;
    FrameTiming timing{};
    FrameChunkMetadata chunkMetadata{};
};
```

`CpuFrame`は常にtight packedである。

```text
Gray8:       rowPitch = width
RGB8/BGR8:   rowPitch = width * 3
RGBA8:       rowPitch = width * 4
```

D3D12 readback bufferのaligned row pitchは内部だけで扱い、`CpuFrame`へ出す時点で詰め直す。

## 8. CameraCaptureStats

主なcapture統計:

```text
receivedBuffers
droppedPendingBuffers
readFrames
readTimeouts
conversionFailures
```

D3D12 ReadOnly pipelineでは、FramePool固有統計を別に取得する。

```text
capacity
available
writing
published
acquisitions
dynamicAllocations
exhaustionDrops
waitTimeouts
```

## 9. CameraPerformanceSnapshot

```text
CameraCaptureStats
IC4StreamStatistics
CameraTimingPerformance
CameraTemperatureReading[]
```

`IC4StreamStatistics`はIC4 stream側のdeliver/underrun/errorを表す。

`CameraTimingPerformance`は最新frame timingからfps、interval、jitter、frame number gapを計算する。

## 10. D3D12 FrameSync types

D3D12 public namespace:

```cpp
namespace Pipe = IC4Ext::D3D12;
```

```text
CameraId
SyncGroupId
FrameSyncOutputId
FrameRateMode
FrameRateLimit
FrameSyncTimestampSource
FrameSyncConfig
FrameSyncStats
FrameSyncOutputConfig
FrameSyncOutputStats
```

D3D12 `FrameSyncStats`:

```text
inputFrames
completedSets
ignoredFrames
droppedFrames
incompleteSets
totalOutputSets
totalOutputQueueDrops
```

## 11. ErrorInfo

```cpp
struct ErrorInfo
{
    int code;
    std::string message;
    std::string where;
};
```

`where`には失敗したcomponent/API、`message`にはdriver/IC4/validation等の詳細を入れる。

## 12. Design rules

- timingとformat/chunk metadataはGPU/CPU変換後も維持する。
- frame numberをcamera間で無条件に比較しない。
- backend固有resource handleをCore型へ入れない。
- `CpuFrame`はreadback出口であり、通常GPU pipelineの中間表現ではない。
- statisticsはdebug/acceptance判定に使えるが、外部telemetry統合はapplication責務とする。
