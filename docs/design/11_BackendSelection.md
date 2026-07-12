# 11. Backend selection

IC4ExtはCMake optionとpublic compile definitionでD3D11/D3D12 backendを選択する。

## 1. Public macros

```cpp
IC4EXT_ENABLE_D3D11
IC4EXT_ENABLE_D3D12
```

CMake target `IC4Ext::IC4Ext`は、configure optionに応じて両macroをpublic definitionとして設定する。

## 2. CMake options

```cmake
option(IC4EXT_ENABLE_D3D11 "Build and expose the D3D11 backend" ON)
option(IC4EXT_ENABLE_D3D12 "Build and expose the D3D12 backend" ON)
```

これらは次を制御する。

```text
backend source registration
helper dependency fetch
include directories
target linking
system libraries
sample registration
test registration
public preprocessor definitions
```

少なくとも利用するbackendを1つ有効にする。

## 3. D3D12 public include

D3D12 2.0.0の正式なpublic entryは次である。

```cpp
#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
namespace Pipe = IC4Ext::D3D12;
```

D3D12だけをbuildする例:

```bat
cmake -S . -B out\build\v2_d3d12 ^
  -DIC4EXT_ENABLE_D3D11=OFF ^
  -DIC4EXT_ENABLE_D3D12=ON
```

旧D3D12 camera/thread/sync/copier headerを直接includeしない。

## 4. Aggregate header

```cpp
#include <IC4Ext/IC4Ext.hpp>
```

aggregate headerはCMake definitionに応じてbackend headerをincludeする。ただしD3D12の意図を明確にするため、新規コードでは`ReadOnlyPipeline.hpp`の直接includeを推奨する。

## 5. Backend asymmetry

D3D11とD3D12は同じCore型を共有するが、camera pipelineのpublic modelは現在異なる。

```text
D3D11
  existing mutable/copy-based frame pipeline

D3D12
  ReadOnlyFrame + CameraCapture-owned FramePool
  one central timestamp FrameSyncThread
  shared immutable fan-out
```

applicationはbackend固有classを明示して扱う。無理に1つの共通virtual APIへ統合しない。

## 6. Common Core definitions

次はbackend非依存である。

```text
CameraCaptureConfig
CameraReadOptions
CameraSyncConfig
FrameTiming
FrameFormatMetadata
FrameChunkMetadata
CpuFrame
ErrorInfo
CameraPerformanceSnapshot
```

## 7. D3D12 backend context

D3D12は`D3D12BackendContext`を使う。

```cpp
auto core = D3D12CoreLib::D3D12Core::CreateShared();
auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
```

raw device/queue pointerだけを渡す初期化は正式経路ではない。

## 8. Dependency selection

```text
D3D11 enabled -> D3D11Helper v1.12.1
D3D12 enabled -> D3D12Helper v1.12.1
both          -> both helpers
```

ThreadKitとnlohmann/jsonは共通依存である。

## 9. Sample/test selection

D3D12のみの場合:

```text
IC4DeviceDiagnostics
SingleCameraReadOnlyReadbackD3D12
MultiCameraReadOnlySyncD3D12
MultiPipelineStressD3D12
D3D12 no-camera/device tests
```

OpenCVはstress sampleだけが要求する。IC4Ext library本体の依存ではない。

## 10. Related documents

```text
docs/d3d12/READONLY_PIPELINE.md
docs/design/10_D3D12Backend.md
docs/design/14_CurrentStatusAndRoadmap.md
```
