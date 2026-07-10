# 13. Readback and CpuFrame

v1.3 では、D3D11 / D3D12 GPU frame を backend 非依存の `CpuFrame` として取り出す readback API を追加しました。

## Purpose

`CpuFrame` は通常 pipeline の中間形式ではありません。目的は以下です。

```txt
保存
テスト
checksum / pixel compare
OpenCV 連携
デバッグ表示
```

通常処理では GPU texture のまま後段へ渡します。

## CpuFrame

```cpp
enum class CpuFrameFormat : std::uint32_t
{
    Unknown = 0,
    Gray8,
    RGBA8,
    RGB8,
    BGR8,
};

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
RGB8:  rowPitch = width * 3
BGR8:  rowPitch = width * 3
RGBA8: rowPitch = width * 4
```

## D3D11 API

```cpp
IC4Ext::D3D11FrameReadback readback;
readback.initialize(device, context);

IC4Ext::CpuFrame cpu;
readback.readback(frame, IC4Ext::CpuFrameFormat::BGR8, cpu);
```

## D3D12 API

```cpp
IC4Ext::D3D12FrameReadback readback;
readback.initialize(backend);

IC4Ext::CpuFrame cpu;
readback.readback(frame, IC4Ext::CpuFrameFormat::BGR8, cpu);
```

D3D12 backend は D3D12Helper-backed `D3D12BackendContext` を必要とします。

## Supported source/destination matrix

| GPU source | Gray8 | RGBA8 | RGB8 | BGR8 |
|---|---:|---:|---:|---:|
| R8 | yes | yes | yes | yes |
| RGBA8 | yes | yes | yes | yes |

`RGBA8 -> Gray8` は以下です。

```txt
Gray = round(0.299 R + 0.587 G + 0.114 B)
```

## OpenCV usage

`BGR8` を指定すると、OpenCV の `CV_8UC3` としてそのまま扱いやすい byte layout になります。

```cpp
IC4Ext::CpuFrame cpu;
readback.readback(frame, IC4Ext::CpuFrameFormat::BGR8, cpu);

// cv::Mat image(cpu.height, cpu.width, CV_8UC3, cpu.data.data(), cpu.rowPitch);
```

IC4Ext 本体は OpenCV に依存しません。

## Samples

```bat
SingleCameraReadback.exe --device-index 0 --output RGBA8 --cpu-format BGR8 --out frame.ppm
SingleCameraReadbackD3D12.exe --device-index 0 --output RGBA8 --cpu-format BGR8 --out frame_d3d12.ppm
```

Gray output は PGM、RGB/BGR/RGBA output は PPM に保存します。

## Tests

```txt
test_cpu_frame
test_d3d11_frame_readback
test_d3d12_frame_readback
```

GPU/device が利用できない場合、backend readback test は skip return code `77` を返します。

## Current performance note

v1.3 の readback は正しさ確認・保存・デバッグ用途を優先しています。毎 frame 高 fps readback を行う用途では、今後以下の最適化を追加する余地があります。

```txt
D3D11 staging texture cache
D3D12 readback buffer reuse
readback ring
format/size ごとの resource cache
```
