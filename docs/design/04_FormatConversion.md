# 04. Format conversion

IC4Extは、camera input format、GPU texture format、CPU readback formatを分けて扱う。

## 1. Camera input formats

```text
Mono8
BayerRG8 / BayerGR8 / BayerGB8 / BayerBG8
BGR8
BGRa8
```

## 2. GPU output formats

```text
R8
RGBA8
```

対応matrix:

| Input | R8 | RGBA8 |
|---|---:|---:|
| Mono8 | yes | yes |
| BayerRG8 | no | yes |
| BayerGR8 | no | yes |
| BayerGB8 | no | yes |
| BayerBG8 | no | yes |
| BGR8 | no | yes |
| BGRa8 | no | yes |

GPU側で3-channel BGR textureを作らず、color outputはRGBA8へ寄せる。

## 3. D3D11 conversion

D3D11 backendはformat別HLSLを使う。

```text
shaders/d3d11/IC4Ext_Convert_Mono8_To_R8.hlsl
shaders/d3d11/IC4Ext_Convert_Mono8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_Bayer8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_BGR8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_BGRa8_To_RGBA8.hlsl
```

## 4. D3D12 conversion

D3D12 2.0.0では、`IC4Ext::D3D12::PooledFrameConverter`がCameraCapture-owned `FramePool`へ直接書き込む。

```text
IC4 CPU bytes
    -> slot UploadRing
    -> slot reusable default-heap input buffer
    -> compute shader
    -> FramePool Texture2D UAV
    -> published state
    -> producer fence
    -> ReadOnlyFrame
```

shader source:

```text
shaders/d3d12/IC4Ext_D3D12_Convert_To_R8.hlsl
shaders/d3d12/IC4Ext_D3D12_Convert_To_RGBA8.hlsl
```

embedded shader sourceもfallbackとして持つ。

D3D12 converterは`D3D12BackendContext::FromCore(...)`で得たhelper-backed contextを前提とする。

## 5. D3D12 input buffer reuse

converter command slotごとにdefault-heap input bufferをcacheする。

```text
first use / larger input:
  allocate or grow buffer

same or smaller input:
  reuse existing buffer
```

resource state:

```text
NON_PIXEL_SHADER_RESOURCE
    -> COPY_DEST
    -> CopyBufferRegion
    -> NON_PIXEL_SHADER_RESOURCE
```

`PooledFrameConverterStats`でallocation/reuse/cache sizeを確認できる。

## 6. D3D12 output ownership

converterはconsumerごとにoutput textureを作らない。1回のcamera conversionで1つのFramePool textureを作り、`ReadOnlyFrame`として共有する。

```text
one converted Texture2D
    -> output A shared handle
    -> output B shared handle
    -> output C shared handle
```

書き込みconsumerは別resourceを確保する。

## 7. GPU published state

camera outputは現在`D3D12_RESOURCE_STATE_GENERIC_READ`で公開する。

これによりconsumerは元Textureを変更せず、次として利用できる。

```text
pixel/non-pixel shader SRV
COPY_SOURCE
```

## 8. CPU readback conversion

readback後は共通`CpuFrame`へ変換する。

```text
Gray8
RGBA8
RGB8
BGR8
```

| GPU source | Gray8 | RGBA8 | RGB8 | BGR8 |
|---|---:|---:|---:|---:|
| R8 | yes | yes | yes | yes |
| RGBA8 | yes | yes | yes | yes |

`RGBA8 -> Gray8`:

```text
Gray = round(0.299 R + 0.587 G + 0.114 B)
```

OpenCV向け`BGR8`はCPU readback出口で作る。

## 9. Metadata preservation

format conversion後も次を維持する。

```text
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
```

`FrameFormatMetadata::outputFormat`だけを実際のGPU outputへ更新する。

## 10. Shader loading

```text
CsoFile
HlslFile
Auto
embedded fallback
```

DXC runtimeが必要なtargetでは、CMakeが`dxcompiler.dll`と`dxil.dll`をexe横へ配置する。

## 11. Performance considerations

高fpsで避けるもの:

```text
毎frameのoutput Texture2D allocation
毎frameのdefault-heap input buffer allocation
output consumerごとのphysical texture copy
不要なCPU wait
```

現在のD3D12 ReadOnly pathは、output FramePoolとper-slot input buffer reuseを実装済みである。

## 12. Future formats

必要になった時点で追加する。

```text
Mono10 / Mono12 / Mono16
Bayer10 / Bayer12 / Bayer16
Bayer10p / Bayer12p
YUV / YCbCr
Polarized formats
MJPG / NV12
```

追加優先度:

1. unpack済み`Mono16` / `Bayer*16`
2. CPU reference test
3. shader reference test
4. packed format unpack
5. YUV/NV12とencoder連携
