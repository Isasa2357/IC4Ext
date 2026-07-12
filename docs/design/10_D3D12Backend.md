# 10. D3D12 backend

この文書は、IC4Ext 2.0.0のD3D12Helper統合backendを説明する。D3D12の正式なcamera pipelineは`IC4Ext::D3D12` ReadOnly pipelineである。

## 1. Initialization rule

D3D12 backendは`D3D12CoreLib::D3D12Core`を前提とする。

```cpp
auto core = D3D12CoreLib::D3D12Core::CreateShared();
auto backend = IC4Ext::D3D12BackendContext::FromCore(core);

if (!backend.resolve()) {
    // error
}
```

raw `ID3D12Device*` / `ID3D12CommandQueue*`だけを渡す初期化は、helper-integrated backendの正式APIではない。

## 2. D3D12BackendContext

`D3D12BackendContext`は次を保持する。

```text
shared_ptr<D3D12Core> core
D3D12Core* corePtr
D3D12Queue* queue
ID3D12Device* device
ID3D12CommandQueue* commandQueue
```

`resolve()`は、`core` / `corePtr` / `queue`から不足fieldを解決する。

既定はDirect queueである。用途に応じてcompute-capable backendを選べるが、resource stateとcommand list typeの制約を満たす必要がある。

## 3. Public D3D12 entry point

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D12;
```

主なclass:

```text
D3D12BackendContext
D3D12ReadyToken
D3D12FenceManager
D3D12FrameConverter
D3D12FrameReadback

IC4Ext::D3D12::ReadOnlyFrame
IC4Ext::D3D12::ReadOnlyFrameSet
IC4Ext::D3D12::FramePool
IC4Ext::D3D12::FrameWriter
IC4Ext::D3D12::PooledFrameConverter
IC4Ext::D3D12::CameraCapture
IC4Ext::D3D12::CameraCaptureThread
IC4Ext::D3D12::FrameSyncThread
IC4Ext::D3D12::ReadOnlyFrameLifetimeTracker
```

旧`D3D12FrameCopier`、旧D3D12 capture threadのphysical-copy fan-out、旧D3D12 frame sync APIは正式build pathから外している。

## 4. Producer conversion path

```text
IC4 ImageBuffer
    -> CPU frame view
    -> slot UploadRing allocation
    -> slot reusable default-heap input buffer
    -> compute shader conversion
    -> CameraCapture-owned FramePool output Texture2D
    -> UAV barrier / published-state transition
    -> queue signal
    -> ReadOnlyFrame publish
```

### 4.1 UploadRing

CPU camera bytesを一時的に保持し、`CopyBufferRegion`でdefault heap input bufferへ転送する。

### 4.2 Reusable input buffer

`PooledFrameConverter`はcommand slotごとにdefault heap input bufferを保持する。

```text
capacityが十分:
  shader-read -> COPY_DEST -> copy -> shader-read

capacity不足:
  より大きいbufferを再確保
```

これにより、毎frameのinput buffer resource生成を避ける。

### 4.3 FramePool output

完成Texture2Dは`CameraCapture`所有の`FramePool`から取得する。consumerへ公開した後はReadOnlyであり、最後の共有参照がなくなるまで再利用しない。

## 5. Supported conversion

```text
Mono8    -> R8
Mono8    -> RGBA8
Bayer*8  -> RGBA8
BGR8     -> RGBA8
BGRa8    -> RGBA8
```

現在のGPU output format:

```text
DXGI_FORMAT_R8_UNORM
DXGI_FORMAT_R8G8B8A8_UNORM
```

## 6. ReadOnly resource state

camera frame poolの公開stateは`D3D12_RESOURCE_STATE_GENERIC_READ`である。

consumerは次の用途で元Textureを読む。

```text
non-pixel shader SRV
pixel shader SRV
COPY_SOURCE
```

consumerは元Textureへ書き込まない。専用output resourceを確保する。

## 7. Producer/consumer synchronization

### Producer ready

`ReadOnlyFrame::readyToken()`はproducer queueが書き込みを完了する時点を表す。

```cpp
Pipe::WaitForReadOnlyFrameReadyOnQueue(consumerQueue, frame);
```

このAPIはconsumer queueへGPU-side waitを登録する。

### Consumer completion

consumerがcommandをsubmitした後、CPU側frame handleを即解放してはならない。

```cpp
auto done = SubmitAndSignal();
lifetimeTracker.retainUntil(frame, done);
```

consumer completionまで保持しないと、FramePoolがTextureを再利用し、GPU read/write競合が起こる。

## 8. CameraCapture

`IC4Ext::D3D12::CameraCapture`は次を所有する。

```text
IC4 grabber / QueueSink
D3D12 converter core
PooledFrameConverter
producer fence manager
FramePool
pending IC4 buffers
statistics and metadata state
```

FramePoolは最初のnegotiated frame shapeに合わせてlazy initializeする。width、height、output formatが変わった場合は新しいpoolへ切り替える。

## 9. CameraCaptureThread

`CameraCaptureThread`は`ReadMode::NextFrame`で連続取得し、中央`IndexedReadOnlyFrameQueue`へ1回だけpushする。

```text
CameraCaptureThread N
    -> { CameraId N, ReadOnlyFrame }
    -> central ingress queue
```

このclassはoutput consumerを列挙せず、GPU copy fan-outを行わない。

## 10. FrameSyncThread

`FrameSyncThread`はCPU側のqueue/buffer操作だけを行う。D3D12 command listをrecordしない。

役割:

```text
timestamp-nearest matching
complete set creation
output snapshot load
priority order
FPS gate
required-camera selection
non-blocking queue push
statistics
```

GPU ready tokenとReadOnly ownershipはそのまま各output setへ引き継ぐ。

## 11. Readback

`D3D12FrameReadback`は2つのsource APIを持つ。

### Legacy D3D12CameraFrame

必要に応じて元resourceをCOPY_SOURCEへtransitionし、copy後に元stateへ戻す。

### ReadOnlyFrame

```cpp
IC4Ext::D3D12FrameReadback readback;
readback.initialize(consumerBackend);

IC4Ext::CpuFrame cpu;
readback.readback(
    frame,
    IC4Ext::CpuFrameFormat::BGR8,
    cpu,
    5000);
```

ReadOnly overloadは次を行う。

```text
published stateがCOPY_SOURCEを含むことを検証
consumer queueへproducer fence GpuWait
元resourceをtransitionしない
CopyTextureRegion
consumer queue signal/wait
CpuFrameへtight-packed変換
```

各並列CPU consumerは専用D3D12 queueと専用`D3D12FrameReadback`を持つ。

## 12. D3D12Helper usage

IC4Extが利用する主なhelper:

```text
D3D12Core
D3D12Queue
D3D12Fence
D3D12CommandContext
D3D12Resource
D3D12UploadRing
D3D12DescriptorAllocator / DescriptorHeap
D3D12ComputePipeline
D3D12ReadbackBuffer
D3D12ShaderCompiler
resource barrier helpers
```

D3D12Helperの単一state trackingは、複数queueから同じresourceを変更する用途には使わない。ReadOnly元resourceは公開後にstate変更しないことで競合を避ける。

## 13. DXC runtime

CMakeは`Microsoft.Direct3D.DXC`をNuGetから解決できる。

```text
IC4EXT_FETCH_DXC_RUNTIME=ON
IC4EXT_DXC_RUNTIME_DIR=<optional override>
IC4EXT_DXC_NUGET_VERSION=<optional fixed version>
```

`ic4ext_copy_dxc_runtime_to_target(target)`は、`dxcompiler.dll`と`dxil.dll`をexecutableと同じdirectoryへcopyする。

## 14. Performance observations

10-pipeline予備試験では、FramePool容量を16/64から128/256へ増やすと次が改善した。

```text
pool exhaustion: non-zero -> 0
capture timeouts: non-zero -> 0
sync drops: non-zero -> 0
sync rate: 約25 fps -> 約53 fps
```

これは、ReadOnly共有設計でもconsumer backlogに応じたpool sizingが必要であることを示す。

同じ試験でHLSL Sobelは約53 fpsへ追従した。OpenCV VideoWriterは約7-17 fpsであり、全フレーム保存にはhardware encodeが必要である。

## 15. Current limitations

- 一部実装本体が`include/IC4Ext/V2` / `src/V2`に残る。public APIではないが物理移動が未完了。
- D3D12-D3D11 shared resource interopは未実装。
- 10/12/16bit、packed Bayer、YUV/NV12等は未実装。
- hardware encoder経路は未統合。
- device removal / DRED failure pathの自動testは未実装。
- 2台160 fps long-run acceptanceは未達。現在の予備実測入力は約53 fps。

## 16. Related documents

```text
docs/d3d12/READONLY_PIPELINE.md
docs/d3d12/VALIDATION_AND_TUNING.md
docs/design/12_D3D12FrameSyncThread.md
docs/design/13_ReadbackAndCpuFrame.md
samples/MultiPipelineStressD3D12/README.md
```
