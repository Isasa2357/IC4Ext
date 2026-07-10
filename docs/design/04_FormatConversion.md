# 04. Format conversion

IC4Ext の format 変換は、入力 camera format、GPU texture format、CPU readback format を分けて扱います。

## Current supported input formats

```txt
Mono8
BayerRG8 / BayerGR8 / BayerGB8 / BayerBG8
BGR8
BGRa8
```

## GPU output formats

```txt
R8
RGBA8
```

対応 matrix:

| Input | R8 | RGBA8 |
|---|---:|---:|
| Mono8 | yes | yes |
| BayerRG8 | no | yes |
| BayerGR8 | no | yes |
| BayerGB8 | no | yes |
| BayerBG8 | no | yes |
| BGR8 | no | yes |
| BGRa8 | no | yes |

## D3D11 conversion

D3D11 backend は format ごとの HLSL を使います。

```txt
shaders/d3d11/IC4Ext_Convert_Mono8_To_R8.hlsl
shaders/d3d11/IC4Ext_Convert_Mono8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_Bayer8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_BGR8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_BGRa8_To_RGBA8.hlsl
```

## D3D12 conversion

D3D12 backend は D3D12Helper 統合版 converter で raw IC4 frame bytes を upload buffer に載せ、compute shader で `R8` / `RGBA8` texture に変換します。

```txt
shaders/d3d12/IC4Ext_D3D12_Convert_To_R8.hlsl
shaders/d3d12/IC4Ext_D3D12_Convert_To_RGBA8.hlsl
```

D3D12 converter は `D3D12BackendContext::FromCore(...)` で得た helper-backed context を前提にします。

## CPU readback conversion

Readback 後は共通の `CpuFrame` に変換します。

```txt
Gray8
RGBA8
RGB8
BGR8
```

対応:

| GPU source | Gray8 | RGBA8 | RGB8 | BGR8 |
|---|---:|---:|---:|---:|
| R8 | yes | yes | yes | yes |
| RGBA8 | yes | yes | yes | yes |

`RGBA8 -> Gray8` は輝度変換を使います。

```txt
Gray = round(0.299 R + 0.587 G + 0.114 B)
```

## Why GPU output is not BGR8

D3D texture としては 3 channel format は扱いづらいため、GPU 側カラー出力は `RGBA8` に寄せます。OpenCV 向けの `BGR8` は `CpuFrame` readback の出口で作ります。

## Formats intentionally left for later

必要になった時点で追加します。

```txt
Mono10 / Mono12 / Mono16
Bayer10 / Bayer12 / Bayer16
Bayer10p / Bayer12p
YUV / YCbCr
Polarized formats
MJPG / NV12
```

16bit 展開済み format を追加する場合は、まず `Mono16` / `Bayer*16` を優先するのが安全です。packed format は bit unpack の CPU reference test と shader test を先に用意してから追加します。
