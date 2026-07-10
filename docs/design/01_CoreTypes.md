# 01. Core types

このファイルは IC4Ext の backend 非依存な Core 型を整理します。

## CameraPixelFormat

IC4 から受け取る入力 format です。現在は 8bit 系のみ対応しています。

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

## GpuFrameFormat

D3D11 / D3D12 texture として出力する format です。

```cpp
enum class GpuFrameFormat : std::uint32_t
{
    R8 = 1,
    RGBA8,
};
```

GPU 上では 3 channel texture は避け、カラー画像は `RGBA8` に寄せます。

## CpuFrameFormat

Readback 後の CPU 側 format です。

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

`BGR8` は OpenCV の `CV_8UC3` にそのまま渡しやすい形式です。

## FrameTiming

```cpp
struct FrameTiming
{
    std::uint64_t frameNumber = 0;
    std::uint64_t deviceTimestampNs = 0;
    std::chrono::steady_clock::time_point hostReceivedTime{};
};
```

意味:

```txt
frameNumber:
  IC4 metadata の device_frame_number。取得できない場合は 0。

deviceTimestampNs:
  IC4 metadata の device_timestamp_ns。取得できない場合は 0。

hostReceivedTime:
  PC 側で frame を受け取った時刻。
```

`FrameSyncThread` は `frameNumber` / `deviceTimestampNs` / `hostReceivedTime` を使って同期判断します。

## CpuFrame

```cpp
struct CpuFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CpuFrameFormat format = CpuFrameFormat::Unknown;
    std::uint32_t rowPitch = 0;
    std::vector<std::uint8_t> data;
    FrameTiming timing{};
};
```

`CpuFrame` は常に tight packed です。

```txt
Gray8: rowPitch = width
RGB8 / BGR8: rowPitch = width * 3
RGBA8: rowPitch = width * 4
```

D3D12 readback buffer の 256-byte aligned pitch は内部だけで扱い、`CpuFrame` に出す時点で詰め直します。

## FrameFormatMetadata

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

入力 format、実際の IC4 buffer format、GPU 出力 format、入力 pitch を保存します。

## Stats

主な stats 型:

```txt
CameraCaptureStats
CameraThreadStats
FrameSyncStats
```

これらは現在の debug / test / log 用の軽量統計です。
