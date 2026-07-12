# 13. Readback and CpuFrame

IC4Extは、D3D11/D3D12 GPU frameをbackend非依存の`CpuFrame`へ変換するreadback APIを提供する。

IC4Ext 2.0.0のD3D12正式経路では、`IC4Ext::D3D12::ReadOnlyFrame`からのreadbackをサポートする。

## 1. Purpose

`CpuFrame`は通常GPU pipelineの中間形式ではない。主な用途:

```text
保存
テスト
checksum / pixel compare
OpenCV連携
デバッグ表示
CPU-only algorithm
```

通常処理ではReadOnly GPU textureのまま後段へ渡す。

## 2. CpuFrame

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
    FrameChunkMetadata chunkMetadata{};
};
```

`CpuFrame`は常にtight packedである。

```text
Gray8: rowPitch = width
RGB8:  rowPitch = width * 3
BGR8:  rowPitch = width * 3
RGBA8: rowPitch = width * 4
```

## 3. D3D11 API

```cpp
IC4Ext::D3D11FrameReadback readback;
readback.initialize(device, context);

IC4Ext::CpuFrame cpu;
readback.readback(frame, IC4Ext::CpuFrameFormat::BGR8, cpu);
```

D3D11 readbackはstaging textureをcacheする。

## 4. D3D12 legacy frame API

```cpp
IC4Ext::D3D12FrameReadback readback;
readback.initialize(backend);

IC4Ext::CpuFrame cpu;
readback.readback(
    legacyFrame,
    IC4Ext::CpuFrameFormat::BGR8,
    cpu,
    5000);
```

legacy `D3D12CameraFrame`経路では、必要に応じてsource resourceを`COPY_SOURCE`へtransitionし、copy後に元stateへ戻す。

この経路は既存helper/testとの互換用であり、新しいcamera pipelineのpublic frame typeではない。

## 5. D3D12 ReadOnlyFrame API

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

namespace Pipe = IC4Ext::D3D12;

IC4Ext::D3D12FrameReadback readback;
readback.initialize(consumerBackend);

IC4Ext::CpuFrame cpu;
const bool ok = readback.readback(
    readOnlyFrame,
    IC4Ext::CpuFrameFormat::BGR8,
    cpu,
    5000);
```

ReadOnly overloadは次を行う。

```text
1. sourceが有効なsingle-subresource Texture2Dか検証
2. formatがR8またはRGBA8か検証
3. published stateがCOPY_SOURCEを含むか検証
4. consumer queueへproducer-ready fenceのGpuWaitを登録
5. source resourceをtransitionせずCopyTextureRegion
6. consumer queueをsignal
7. copy完了をCPU wait
8. readback bufferをmap
9. destination CpuFrameFormatへtight-packed変換
10. FrameTiming / FrameChunkMetadataを引き継ぐ
```

## 6. Why ReadOnly readback does not transition the source

1つのReadOnly textureを複数consumerが同時に読むため、各consumerが独立に次をrecordしてはならない。

```text
GENERIC_READ -> COPY_SOURCE -> GENERIC_READ
```

複数queueが同じresource stateを変更すると、互いのbarrier sequenceを知らないためresource-state競合になる。

camera FramePoolのpublished stateは`D3D12_RESOURCE_STATE_GENERIC_READ`であり、COPY_SOURCE用途を含む。ReadOnly overloadはsource stateを変更せずcopyする。

## 7. Dedicated consumer queue

`D3D12FrameReadback`は内部に次を持つ。

```text
D3D12BackendContext
D3D12Queue pointer
D3D12CommandContext
D3D12ReadbackBuffer cache
statistics
last error
```

1つのinstanceを複数worker threadから同時に使わない。

並列CPU pipelineごとに次を作る。

```text
専用D3D12 queue
専用D3D12FrameReadback
専用readback buffer cache
専用CpuFrame
専用cv::Mat
```

`MultiPipelineStressD3D12`では、CPU/display/video pipelineごとに`DedicatedReadback`を作り、ペア処理はcameraごとにもinstanceを分ける。

## 8. GPU synchronization

### Producer completion

ReadOnly overloadはconsumer queue上でproducer fenceを待つ。

```text
consumerQueue.GpuWait(producerFence, producerValue)
    -> CopyTextureRegion
```

CPUがproducer readyを待ってからcopy commandをsubmitするのではなく、GPU queue間同期を使う。

### Readback completion

copy後、consumer queue自身のfenceをsignalし、CPUがその値を待つ。完了後にreadback bufferをmapする。

## 9. Resource lifetime

readback call中は引数の`ReadOnlyFrame`参照がsource resourceを保持する。synchronous readback APIはcopy completionまで戻らないため、call中にpoolへ返却されない。

非同期readbackを将来追加する場合は、copy completion fenceまでsource frameを保持するlifetime trackerが必要になる。

## 10. Supported source/destination matrix

| GPU source | Gray8 | RGBA8 | RGB8 | BGR8 |
|---|---:|---:|---:|---:|
| R8 | yes | yes | yes | yes |
| RGBA8 | yes | yes | yes | yes |

`RGBA8 -> Gray8`は概ね次の係数を使う。

```text
Gray = round(0.299 R + 0.587 G + 0.114 B)
```

## 11. OpenCV usage

`BGR8`はOpenCV `CV_8UC3`として扱いやすい。

```cpp
IC4Ext::CpuFrame cpu;
readback.readback(frame, IC4Ext::CpuFrameFormat::BGR8, cpu);

cv::Mat view(
    static_cast<int>(cpu.height),
    static_cast<int>(cpu.width),
    CV_8UC3,
    cpu.data.data(),
    cpu.rowPitch);
```

`CpuFrame`の寿命より長く`cv::Mat` viewを保持してはならない。独立所有が必要なら`clone()`する。

IC4Ext library本体はOpenCVに依存しない。

## 12. Readback cache

### D3D11

同じsize/format/subresource layoutのstaging textureを再利用する。

### D3D12

必要なcopy footprint以上のreadback bufferを保持し、同一または小さいframeで再利用する。

```cpp
const auto stats = readback.cacheStats();

stats.readbacks;
stats.cacheHits;
stats.cacheMisses;
stats.resourceRebuilds;
stats.bytesAllocated;
```

```cpp
readback.resetCache();
```

`resetCache()`は保持resourceとcache統計を初期化する。

## 13. Performance characteristics

readback costには次が含まれる。

```text
producer->consumer queue synchronization
Texture2D -> readback buffer copy
GPU fence wait
map/unmap
pixel format conversion
CpuFrame allocation/copy
OpenCV workload if any
```

10-pipeline予備試験の例:

```text
single display readback/compose      約28-33 ms
pair display readback/compose        約73 ms
OpenCV Canny                         約23 ms
OpenCV Sobel magnitude               約43 ms
OpenCV pair processed display        約105 ms
single OpenCV VideoWriter            約59-61 ms
pair OpenCV VideoWriter              約133 ms
```

これらは特定環境の予備値であり保証値ではないが、複数独立readbackがGPU/PCIe/CPU memory bandwidthを強く消費することを示す。

## 14. Video recording note

`MultiPipelineStressD3D12`のAVI保存は、ストレス負荷としてOpenCV `VideoWriter`を使う。

```text
ReadOnlyFrame
    -> independent readback
    -> BGR CpuFrame
    -> cv::VideoWriter
```

予備実測では、single recordingが約16-17 fps、pair recordingが約7-8 fpsだった。高fps全フレーム保存の最終方式には、GPU textureを直接入力するhardware encoderを使うべきである。

## 15. Tests

```text
test_cpu_frame
test_d3d11_frame_readback
test_d3d12_frame_readback
test_d3d12_pooled_converter_device
```

D3D12 deviceを作れない環境ではdevice testはreturn code 77でskipできる。

## 16. Samples

```text
SingleCameraReadOnlyReadbackD3D12
MultiPipelineStressD3D12
```

詳細:

```text
samples/MultiPipelineStressD3D12/README.md
docs/d3d12/VALIDATION_AND_TUNING.md
```

## 17. Future work

- asynchronous readback API
- readback ring / multiple in-flight copies
- format/size別の複数cache
- GPU-side resize/format conversion before readback
- hardware encoder integration
- readback latency p50/p95/p99 measurement
- bandwidth counters and GPU timestamp queries
