# IC4Ext 自己完結設計書 v1.3 FULL

この文書は分割版 docs を結合した全文版である。すべて UTF-8。


---

# 00. IC4Ext 全体設計書 v1.3

## 1. 目的

IC4Ext は、The Imaging Source IC4 SDK から取得したカメラフレームを、Direct3D 11 の GPU texture として扱いやすくするための C++17 ライブラリである。

初期実装では D3D11 backend のみを対象にする。D3D12 backend、D3D12 用ディレクトリ、D3D12 API skeleton は作らない。

このライブラリは次を目的とする。

1. IC4 カメラを列挙・選択・open する。
2. カメラ device の `PixelFormat` property を明示設定する。
3. IC4 SDK から受け取った image buffer を GPU に upload する。
4. 必要に応じて D3D11 compute shader で format conversion を行う。
5. `read()` により、OpenCV `VideoCapture::read()` に近い感覚で D3D11 GPU frame を取得できるようにする。
6. `ReadMode::LatestFrame` と `ReadMode::NextFrame` を選択可能にする。
7. 連続取得用途向けに `D3D11CameraCaptureThread` を提供する。
8. thread 間配送では `cameraIndex` 付き frame を流し、将来の複数カメラ同期に備える。

## 2. 非目的

IC4Ext は以下を直接の目的にしない。

- OpenCV `Mat` を中心にした CPU 画像処理ライブラリにすること。
- D3D11 を完全に隠蔽した抽象 graphics layer にすること。
- D3D12 backend を同時に実装すること。
- D3D12 用の空 skeleton を作ること。
- `ic4gui` を使った GUI 表示を含めること。
- window / swapchain 表示 sample を初期実装に含めること。
- 10/12bit packed Bayer の unpack / demosaic を初期実装に含めること。
- 複数カメラの実機テストを初期実装に含めること。

## 3. 外部依存

### 3.1 IC4 SDK

IC4 SDK はカメラ列挙、device open、stream 設定、image buffer 取得に使う。

初期実装では GUI を使わないため、link は `ic4core.lib` のみとする。

想定 SDK root 例:

```txt
C:/Users/user/AppData/Local/Programs/The Imaging Source Europe GmbH/IC Imaging Control 4
```

想定ファイル例:

```txt
bin/x64/ic4core.dll
lib/x64/ic4core.lib
include/ic4/ic4.h
```

`ic4gui.lib` / `ic4gui.dll` は使わない。

### 3.2 D3D11Helper

D3D11Helper は D3D11 device/context、texture、SRV/UAV、compute pipeline、shader 読み込み、fence 周辺の定型処理を薄く包む helper として使う。

IC4Ext は D3D11Helper を前提にしてよい。ただし、D3D11Helper に存在しない小さな処理は IC4Ext 内で直接 D3D11 API を呼んでよい。

### 3.3 ThreadKit

ThreadKit は thread 間 queue、停止 token、worker thread に使う。

想定する主な機能:

```cpp
ThreadKit::Queues::BlockingQueue<T>
ThreadKit::Queues::QueueOptions
ThreadKit::Queues::QueueStats
ThreadKit::StopToken
ThreadKit::Threads::WorkerThread
```

IC4Ext 側で独自の blocking queue や stop token を再実装しない。

## 4. 主要設計原則

### 4.1 D3D11 のみを実装対象にする

初期実装で公開する主要クラスは次である。

```cpp
IC4Ext::D3D11CameraCapture
IC4Ext::D3D11CameraCaptureThread
IC4Ext::D3D11FrameConverter
IC4Ext::D3D11FrameCopier
IC4Ext::D3D11FrameSyncThread
```

D3D12 向けの namespace、directory、dummy header、macro branch は作らない。

### 4.2 CameraCapture は自前の worker thread を持たない

`D3D11CameraCapture` は OpenCV の `VideoCapture` に近い同期的な取得クラスである。

ただし、IC4 `QueueSinkListener::framesQueued()` は IC4 SDK 側の dedicated thread から呼ばれる。この callback は `D3D11CameraCapture` 内部の pending buffer queue へ image buffer を移すために使ってよい。

`D3D11CameraCapture` 自体は `ThreadKit::WorkerThread` を持たない。

### 4.3 requestedFormat は device PixelFormat として扱う

`CameraStreamRequest::requestedFormat` は、IC4 `QueueSink` の受け入れ形式だけではなく、カメラ device の `PixelFormat` property として設定する。

実装者は次を行うこと。

1. device open 後、IC4 property map から `PixelFormat` を設定する。
2. `QueueSink::Config::acceptedPixelFormats` にも同じ pixel format を入れる。
3. sink format が device format と異なることによる IC4 SDK 内部変換を避ける。

`requestedFormat` が device で選択できない場合、IC4Ext は `open()` を失敗させる。

### 4.4 outputFormat は利用者が受け取る GPU texture format

`FrameOutputSpec::outputFormat` は `read()` が返す D3D11 texture の形式である。

例:

```cpp
CameraCaptureConfig config;
config.streamRequest.requestedFormat = CameraPixelFormat::BayerRG8;
config.outputSpec.outputFormat       = GpuFrameFormat::RGBA8;
```

この場合、IC4 device には `BayerRG8` を要求し、`read()` が返す GPU texture は `DXGI_FORMAT_R8G8B8A8_UNORM` になる。

### 4.5 read mode は LatestFrame と NextFrame を選べる

`D3D11CameraCapture::read()` は次を受け取る。

```cpp
enum class ReadMode
{
    LatestFrame,
    NextFrame,
};
```

デフォルトは `LatestFrame` である。

- `LatestFrame`: pending buffer queue に複数 frame がある場合、古い frame を捨て、最新 frame を GPU へ upload / convert して返す。表示・preview 向け。
- `NextFrame`: pending buffer queue の oldest frame を 1 枚取り出して GPU へ upload / convert して返す。録画・解析・capture thread 向け。

### 4.6 CameraCaptureThread は NextFrame を使う

`D3D11CameraCaptureThread` は内部で `D3D11CameraCapture::read(ReadMode::NextFrame)` を繰り返し呼ぶ。

`CameraCaptureThread` の目的は、連続取得された frame を ThreadKit queue へ配送することである。表示だけを目的にする `LatestFrame` は使わない。

### 4.7 queue 登録には必ず cameraIndex を要求する

thread から出力される frame は必ず次の形にする。

```cpp
struct D3D11IndexedCameraFrame
{
    std::uint32_t cameraIndex;
    D3D11CameraFrame frame;
};
```

`D3D11CameraCaptureThread::addOutputQueue()` は必ず `cameraIndex` を引数に取る。

```cpp
void addOutputQueue(
    std::uint32_t cameraIndex,
    std::shared_ptr<D3D11IndexedFrameQueue> queue);
```

1 台カメラでも `cameraIndex = 0` を明示する。

### 4.8 CameraCaptureThread は CameraCapture を外部共有しない

推奨 constructor は `CameraCapture` 自体を受け取らず、`CameraCapture` の初期化引数を受け取る。

```cpp
D3D11CameraCaptureThread(
    IC4DeviceSelector selector,
    CameraCaptureConfig config,
    D3D11CoreLib::D3D11Core* core,
    CameraThreadOptions options = {});
```

この constructor は thread 内部で `D3D11CameraCapture` を生成・open する。

既存 `D3D11CameraCapture` を渡す overload を作る場合は、必ず move 専用にする。

```cpp
explicit D3D11CameraCaptureThread(
    D3D11CameraCapture&& capture,
    CameraThreadOptions options = {});
```

copy 受け取りや参照受け取りは禁止する。外部から同じ capture に触れてはならない。

### 4.9 D3D11 fence が使える前提にする

D3D11 GPU 作業の完了通知は `ID3D11Fence` を使う。

必要 interface:

```cpp
ID3D11Device5
ID3D11DeviceContext4
ID3D11Fence
```

初期化時に `QueryInterface` または `CreateFence` が失敗した場合、fallback せず backend 初期化失敗にする。

`ID3D11Query(D3D11_QUERY_EVENT)` fallback は実装しない。

### 4.10 shader は .hlsl と .cso の両方を読める

format conversion shader は次の両方をサポートする。

- `.hlsl`: 実行時 compile または D3D11Helper の shader compiler 経由で読み込む。
- `.cso`: 事前 compile 済み bytecode として読み込む。

利用者は config で shader directory と読み込み方式を指定できる。

## 5. 基本データフロー

### 5.1 単体 read 利用

```txt
IC4 camera
  -> IC4 QueueSink callback
  -> CameraCapture internal pending buffer queue
  -> read(LatestFrame or NextFrame)
  -> raw byte upload buffer
  -> D3D11 compute shader conversion
  -> D3D11CameraFrame
```

### 5.2 CameraCaptureThread 利用

```txt
D3D11CameraCaptureThread
  owns D3D11CameraCapture
  loop:
    capture.read(ReadMode::NextFrame)
    copy frame for N output queues if needed
    push D3D11IndexedCameraFrame(cameraIndex, frame)
```

### 5.3 1 台カメラ初期構成

初期 sample/test は 1 台カメラを前提にする。

```txt
camera0
  -> D3D11CameraCaptureThread
  -> output queue with cameraIndex = 0
  -> consumer logs frame number / timestamp / texture size / format
```

## 6. パッケージ構成

実装成果物は次の構成を推奨する。

```txt
IC4Ext/
  CMakeLists.txt
  README.md
  include/
    IC4Ext/
      IC4Ext.hpp
      Config.hpp
      Core/
        CoreTypes.hpp
        Error.hpp
        IC4DeviceSelector.hpp
      D3D11/
        D3D11CameraFrame.hpp
        D3D11CameraCapture.hpp
        D3D11CameraCaptureThread.hpp
        D3D11FrameConverter.hpp
        D3D11FrameCopier.hpp
        D3D11FrameSyncThread.hpp
  src/
    Core/
      Error.cpp
      IC4DeviceSelector.cpp
    D3D11/
      D3D11CameraCapture.cpp
      D3D11CameraCaptureThread.cpp
      D3D11FrameConverter.cpp
      D3D11FrameCopier.cpp
      D3D11FrameSyncThread.cpp
  shaders/
    d3d11/
      IC4Ext_Convert_Bayer8_To_RGBA8.hlsl
      IC4Ext_Convert_BGR8_To_RGBA8.hlsl
      IC4Ext_Convert_BGRa8_To_RGBA8.hlsl
      IC4Ext_Convert_Mono8_To_R8.hlsl
      IC4Ext_Convert_Mono8_To_RGBA8.hlsl
  samples/
    SingleCameraLog/
      main.cpp
      CMakeLists.txt
  tests/
    test_core.cpp
    test_single_camera_open_read.cpp
    test_single_camera_capture_thread.cpp
```

D3D12 directory は作らない。

## 7. 最低限の実装順序

1. Core 型群と error 型を実装する。
2. IC4 device selector と stream setup を実装する。
3. D3D11 backend 初期化と fence ready token を実装する。
4. raw byte upload と frame converter を実装する。
5. `D3D11CameraCapture::open/read/close` を実装する。
6. 1 台カメラ sample を実装する。
7. `D3D11CameraCaptureThread` を実装する。
8. 1 台カメラ前提 test を実装する。
9. `D3D11FrameSyncThread` は 1 台入力で動く最小形を実装し、複数台同期は simulation test に留める。


---

# 01. Core 型群設計 v1.3

## 1. namespace

全 API は原則として `IC4Ext` namespace に置く。

```cpp
namespace IC4Ext
{
}
```

D3D11 固有クラスは `IC4Ext` 直下または `IC4Ext::D3D11` に置いてよいが、公開 API では名前に `D3D11` を含める。

例:

```cpp
IC4Ext::D3D11CameraCapture
IC4Ext::D3D11CameraFrame
```

## 2. エラー方針

初期実装では例外必須にしない。`open()` は `bool` を返し、詳細は `lastError()` で取得できるようにする。

```cpp
struct ErrorInfo
{
    int code = 0;
    std::string message;
    std::string where;
};
```

推奨 API:

```cpp
const ErrorInfo& lastError() const noexcept;
```

重大なプログラマエラー、例えば null `D3D11Core*`、未 open 状態での `read()` は、`false` / empty result を返し `lastError` を更新する。

## 3. IC4DeviceSelector

カメラ選択には次の struct を使う。

```cpp
struct IC4DeviceSelector
{
    std::string serial;
    std::string uniqueName;
    int deviceIndex = -1;
};
```

選択順序は固定である。

1. `serial` が非空なら serial 完全一致。
2. `uniqueName` が非空なら unique name 完全一致。
3. `deviceIndex >= 0` なら `DeviceEnum::enumDevices()` の index。
4. すべて未指定なら先頭 device。

複数条件が同時に指定された場合、上位条件のみを見る。例えば `serial` と `deviceIndex` の両方が指定されている場合、`serial` を優先し、`deviceIndex` は無視する。

`serial` / `uniqueName` が見つからない、または `deviceIndex` が範囲外の場合、open は失敗する。

## 4. CameraPixelFormat

`CameraPixelFormat` は IC4 device に要求する pixel format を表す。

初期実装で対応する値:

```cpp
enum class CameraPixelFormat
{
    Mono8,

    BayerRG8,
    BayerGR8,
    BayerGB8,
    BayerBG8,

    BGR8,
    BGRa8,
};
```

IC4 SDK との対応:

```txt
CameraPixelFormat::Mono8    -> ic4::PixelFormat::Mono8
CameraPixelFormat::BayerRG8 -> ic4::PixelFormat::BayerRG8
CameraPixelFormat::BayerGR8 -> ic4::PixelFormat::BayerGR8
CameraPixelFormat::BayerGB8 -> ic4::PixelFormat::BayerGB8
CameraPixelFormat::BayerBG8 -> ic4::PixelFormat::BayerBG8
CameraPixelFormat::BGR8     -> ic4::PixelFormat::BGR8
CameraPixelFormat::BGRa8    -> ic4::PixelFormat::BGRa8
```

初期実装で対応しない値は enum に入れないか、入れる場合は `Unsupported` として明示的に扱う。

未対応例:

```txt
BayerRG10p
BayerRG12p
BayerRG12Packed
BayerRG16
BGRa16
Polarized 系
YUV 系
```

## 5. GpuFrameFormat

`GpuFrameFormat` は `read()` が返す D3D11 texture の format を表す。

```cpp
enum class GpuFrameFormat
{
    R8,
    RGBA8,
};
```

D3D11 / DXGI との対応:

```txt
GpuFrameFormat::R8    -> DXGI_FORMAT_R8_UNORM
GpuFrameFormat::RGBA8 -> DXGI_FORMAT_R8G8B8A8_UNORM
```

初期実装では `RGBA8` を主出力とする。`Mono8 -> R8` も対応する。

## 6. ReadMode

`read()` の frame 取得方針を表す。

```cpp
enum class ReadMode
{
    LatestFrame,
    NextFrame,
};
```

- `LatestFrame`: pending queue 内の古い frame を捨て、最新 frame を処理する。
- `NextFrame`: pending queue 内の最古 frame を 1 枚処理する。

`D3D11CameraCapture::read()` の default は `LatestFrame` とする。

```cpp
ReadResult read(ReadMode mode = ReadMode::LatestFrame);
```

`D3D11CameraCaptureThread` は必ず `ReadMode::NextFrame` を使う。

## 7. FrameQueuePolicy

IC4 SDK の QueueSink callback から内部 pending queue へ buffer を移すときの保持方針を表す。

```cpp
enum class FrameQueuePolicy
{
    LatestOnly,
    PreserveFrames,
};
```

- `LatestOnly`: 内部 pending queue は最新 frame のみ保持する。新 frame 到着時に古い pending frame は破棄する。preview 用 default。
- `PreserveFrames`: 内部 pending queue に frame を順に保持する。解析・録画・capture thread 用。

`CameraCaptureConfig` の default は `LatestOnly` とする。

`D3D11CameraCaptureThread` が内部で `D3D11CameraCapture` を作る場合、config の policy を `PreserveFrames` に補正してよい。少なくとも thread loop では `ReadMode::NextFrame` を使う。

## 8. CameraStreamRequest

```cpp
struct CameraStreamRequest
{
    int width = 0;
    int height = 0;
    double fps = 0.0;
    CameraPixelFormat requestedFormat = CameraPixelFormat::BayerRG8;
};
```

意味:

- `width > 0` の場合、device の width property に設定を試みる。
- `height > 0` の場合、device の height property に設定を試みる。
- `fps > 0` の場合、device の frame rate property に設定を試みる。
- `requestedFormat` は device の `PixelFormat` property に設定する。

property 設定に失敗した場合は open 失敗にする。実装者が graceful fallback を入れる場合でも、選択された実 format を metadata に必ず残す。

## 9. FrameOutputSpec

```cpp
struct FrameOutputSpec
{
    GpuFrameFormat outputFormat = GpuFrameFormat::RGBA8;
    bool createSrv = true;
    bool createUav = true;
};
```

- `outputFormat` は返却 texture の format。
- `createSrv` が true なら SRV を作る。
- `createUav` が true なら converter が UAV 書き込みできるようにする。

`createUav == false` の場合でも、compute shader conversion が必要な path では内部 UAV texture を作り、最後に output texture へ copy してよい。

## 10. ShaderInputKind / ShaderLoadConfig

shader は `.hlsl` と `.cso` の両方を読める必要がある。

```cpp
enum class ShaderInputKind
{
    Auto,
    HlslFile,
    CsoFile,
};

struct ShaderLoadConfig
{
    ShaderInputKind inputKind = ShaderInputKind::Auto;
    std::filesystem::path shaderDirectory;
    std::string entryPoint = "main";
    std::string target = "cs_5_0";
    bool preferCsoWhenBothExist = true;
};
```

`Auto` の推奨動作:

1. `preferCsoWhenBothExist == true` なら `.cso` を先に探す。
2. `.cso` がなければ `.hlsl` を探して compile する。
3. どちらもなければ open 失敗または converter 初期化失敗にする。

## 11. CameraCaptureConfig

```cpp
struct CameraCaptureConfig
{
    CameraStreamRequest streamRequest;
    FrameOutputSpec outputSpec;
    FrameQueuePolicy queuePolicy = FrameQueuePolicy::LatestOnly;
    std::size_t maxPendingBuffers = 1;
    ShaderLoadConfig shaderConfig;
};
```

`maxPendingBuffers` の意味:

- `queuePolicy == LatestOnly` の場合、実装は常に最新 1 枚だけ保持してよい。`maxPendingBuffers` は最低 1 とみなす。
- `queuePolicy == PreserveFrames` の場合、`maxPendingBuffers == 0` なら無制限、`>0` なら上限到達時に policy に従って drop または read 停止を設計する。

初期実装では、`PreserveFrames` で上限到達した場合は古い frame を drop し、drop count を stats に記録する方針でよい。

## 12. FrameTiming

```cpp
struct FrameTiming
{
    std::uint64_t frameNumber = 0;
    std::uint64_t deviceTimestampNs = 0;
    std::chrono::steady_clock::time_point hostReceivedTime;
};
```

IC4 image buffer から frame number / timestamp を取得できる場合は設定する。取得できない場合は 0 とし、`hostReceivedTime` は callback で buffer を受け取った時刻に設定する。

## 13. FrameFormatMetadata

```cpp
struct FrameFormatMetadata
{
    CameraPixelFormat requestedFormat;
    CameraPixelFormat actualInputFormat;
    GpuFrameFormat outputFormat;
    int width = 0;
    int height = 0;
    std::size_t inputRowPitchBytes = 0;
};
```

`actualInputFormat` は IC4 sink 接続時に negotiated image type から取得する。`requestedFormat` と異なる場合は、原則として open 失敗にする。ただし実装者が許容する場合でも metadata に差分を残す。

## 14. D3D11ReadyToken

GPU work 完了を表す token。

```cpp
struct D3D11ReadyToken
{
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    std::uint64_t value = 0;

    bool isValid() const noexcept;
    bool isReady() const;
    bool wait(std::uint32_t timeoutMs = INFINITE) const;
};
```

`wait()` は `ID3D11Fence::GetCompletedValue()` を確認し、未完了なら `SetEventOnCompletion(value, event)` で待つ。

## 15. D3D11CameraFrame

```cpp
struct D3D11CameraFrame
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;

    D3D11ReadyToken ready;
    FrameTiming timing;
    FrameFormatMetadata format;
};
```

`texture` は `read()` が返す最終 GPU texture である。raw input buffer や変換中間 texture は含めない。

## 16. D3D11IndexedCameraFrame

```cpp
struct D3D11IndexedCameraFrame
{
    std::uint32_t cameraIndex = 0;
    D3D11CameraFrame frame;
};

using D3D11IndexedFrameQueue =
    ThreadKit::Queues::BlockingQueue<D3D11IndexedCameraFrame>;
```

CameraCaptureThread と FrameSyncThread の入力はこの型を使う。


---

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


---

# 03. D3D11 backend 設計 v1.3

## 1. 目的

この文書は IC4Ext の D3D11 backend に必要な device/context、resource、fence、shader 読み込み、copy の仕様を定義する。

初期実装では D3D11 backend のみを実装する。D3D12 backend は存在しない。

## 2. 外部から受け取る D3D11 core

`D3D11CameraCapture::open()` は D3D11Helper の core object を受け取る。

```cpp
bool open(const IC4DeviceSelector& selector,
          const CameraCaptureConfig& config,
          D3D11CoreLib::D3D11Core* core);
```

`core == nullptr` は open 失敗にする。

`core` から最低限取得できる必要があるもの:

```cpp
ID3D11Device* device;
ID3D11DeviceContext* immediateContext;
```

IC4Ext は `core` を所有しない。`D3D11CameraCapture` と `D3D11CameraCaptureThread` の寿命中、呼び出し側は `core` を生存させる。

## 3. D3D11.4 fence 前提

IC4Ext は D3D11 GPU work 完了通知に D3D11 fence を使う。

必要 interface:

```cpp
ID3D11Device5
ID3D11DeviceContext4
ID3D11Fence
```

backend 初期化時に次を行う。

```cpp
ComPtr<ID3D11Device5> device5;
ComPtr<ID3D11DeviceContext4> context4;
ComPtr<ID3D11Fence> fence;

HRESULT hr1 = device->QueryInterface(IID_PPV_ARGS(&device5));
HRESULT hr2 = context->QueryInterface(IID_PPV_ARGS(&context4));
HRESULT hr3 = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
```

いずれかが失敗した場合、backend 初期化は失敗する。

`ID3D11Query(D3D11_QUERY_EVENT)` fallback は実装しない。

## 4. D3D11FenceManager

fence value の採番と token 生成を管理する class を作る。

```cpp
class D3D11FenceManager
{
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    D3D11ReadyToken signal();
    bool wait(const D3D11ReadyToken& token, std::uint32_t timeoutMs);
    bool isReady(const D3D11ReadyToken& token) const;

private:
    Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    std::atomic<std::uint64_t> nextValue_{1};
};
```

`signal()` は、直前までに immediate context に投入された GPU command の後ろに fence signal を入れる。

```cpp
std::uint64_t value = nextValue_.fetch_add(1);
context4_->Signal(fence_.Get(), value);
return D3D11ReadyToken{ fence_, value };
```

## 5. D3D11ReadyToken

```cpp
struct D3D11ReadyToken
{
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    std::uint64_t value = 0;

    bool isValid() const noexcept
    {
        return fence != nullptr && value != 0;
    }

    bool isReady() const
    {
        return isValid() && fence->GetCompletedValue() >= value;
    }

    bool wait(std::uint32_t timeoutMs = INFINITE) const;
};
```

`wait()` の実装方針:

```txt
1. token が invalid なら false
2. GetCompletedValue() >= value なら true
3. CreateEvent
4. fence->SetEventOnCompletion(value, event)
5. WaitForSingleObject(event, timeoutMs)
6. WAIT_OBJECT_0 なら true、それ以外は false
```

## 6. raw upload buffer

IC4 image buffer は CPU memory である。D3D11 compute shader で扱うため、まず raw byte buffer として GPU へ upload する。

BGR8 は 24-bit であり、D3D11 texture format として直接扱いにくい。そのため、初期実装では input を texture としてではなく raw byte buffer として扱うことを推奨する。

推奨 resource:

```txt
ID3D11Buffer rawInputBuffer
  bind: D3D11_BIND_SHADER_RESOURCE
  usage: D3D11_USAGE_DEFAULT または D3D11_USAGE_DYNAMIC
  data: IC4 image buffer の row pitch を含む raw bytes
```

SRV は buffer SRV として作る。compute shader は byte offset を計算して input を読む。

実装の簡略化として、CPU 側で BGR8 を BGRA8 へ expand してから texture upload してもよいが、初期設計上の推奨は raw byte buffer + compute shader である。

## 7. output texture

`GpuFrameFormat` に応じて output texture を作る。

```txt
RGBA8 -> DXGI_FORMAT_R8G8B8A8_UNORM
R8    -> DXGI_FORMAT_R8_UNORM
```

基本 descriptor:

```cpp
D3D11_TEXTURE2D_DESC desc = {};
desc.Width = width;
desc.Height = height;
desc.MipLevels = 1;
desc.ArraySize = 1;
desc.Format = dxgiFormat;
desc.SampleDesc.Count = 1;
desc.Usage = D3D11_USAGE_DEFAULT;
desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
```

`FrameOutputSpec::createSrv` が true なら SRV を作る。compute shader が output へ書き込むため、通常は UAV も作る。

`DXGI_FORMAT_R8_UNORM` の UAV support は device feature に依存する可能性があるため、初期化時に `CheckFormatSupport` で検証する。未対応なら `Mono8 -> R8` を unsupported にするか、内部 RGBA8 出力に fallback する。ただし fallback した場合は metadata に実 output format を正しく残す。

## 8. shader 読み込み

shader は `.hlsl` と `.cso` の両方をサポートする。

推奨 class:

```cpp
class D3D11ShaderLoader
{
public:
    bool loadComputeShader(ID3D11Device* device,
                           const ShaderLoadConfig& config,
                           std::string_view baseName,
                           Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outShader,
                           std::vector<std::uint8_t>* outBytecode = nullptr);
};
```

`baseName` 例:

```txt
IC4Ext_Convert_Bayer8_To_RGBA8
IC4Ext_Convert_BGR8_To_RGBA8
IC4Ext_Convert_BGRa8_To_RGBA8
IC4Ext_Convert_Mono8_To_R8
IC4Ext_Convert_Mono8_To_RGBA8
```

読み込み規則:

```txt
ShaderInputKind::CsoFile:
  shaderDirectory / (baseName + ".cso") を読む

ShaderInputKind::HlslFile:
  shaderDirectory / (baseName + ".hlsl") を読む
  entryPoint / target で compile する

ShaderInputKind::Auto:
  preferCsoWhenBothExist が true なら .cso -> .hlsl の順
  false なら .hlsl -> .cso の順
```

`.hlsl` compile は D3D11Helper の機能を使ってよい。D3D11Helper 側に compile helper がない場合は、`D3DCompileFromFile` または DXC を IC4Ext 側で使ってよい。

## 9. constant buffer

compute shader へ渡す parameter は constant buffer にまとめる。

```cpp
struct ConvertConstants
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t inputRowPitchBytes;
    std::uint32_t inputPixelFormat; // CameraPixelFormat を uint32_t 化
};
```

Bayer pattern を shader に渡すには `inputPixelFormat` を使う。

## 10. compute dispatch

dispatch group size は shader と合わせる。

例:

```hlsl
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
```

dispatch count:

```cpp
UINT groupsX = (width  + 15) / 16;
UINT groupsY = (height + 15) / 16;
context->Dispatch(groupsX, groupsY, 1);
```

dispatch 後に UAV を unbind する。

```cpp
ID3D11UnorderedAccessView* nullUav = nullptr;
context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
```

その後、`D3D11FenceManager::signal()` を呼び、返却 frame の `ready` に token を入れる。

## 11. D3D11FrameCopier

複数 output queue へ配送する場合、consumer ごとに独立 texture を持たせる。

```cpp
class D3D11FrameCopier
{
public:
    bool copyFrame(const D3D11CameraFrame& src,
                   D3D11CameraFrame& dst);
};
```

copy 方針:

1. `src` と同じ size / format の texture を作る。
2. `ID3D11DeviceContext::CopyResource` または `CopySubresourceRegion` で copy する。
3. copy 後に fence signal し、`dst.ready` に token を入れる。
4. CPU wait はしない。

metadata と timing は src から dst へコピーする。

## 12. CPU wait 禁止の原則

`read()`、`FrameConverter`、`FrameCopier`、`CameraCaptureThread` は原則として GPU 完了を CPU wait しない。

返却 frame には `D3D11ReadyToken` を持たせる。consumer は texture を使う直前に必要なら `frame.ready.wait()` を呼ぶ。

ただし、test や sample で frame の完了を確認する目的では wait してよい。


---

# 04. format conversion 設計 v1.3

## 1. 目的

この文書は IC4 input format から D3D11 output texture への変換仕様を定義する。

初期実装では次のみ対応する。

```txt
Mono8
BayerRG8 / BayerGR8 / BayerGB8 / BayerBG8
BGR8
BGRa8
```

出力は次のみ対応する。

```txt
R8
RGBA8
```

## 2. 変換表

| requestedFormat | outputFormat | 対応 | 変換 |
|---|---:|---:|---|
| Mono8 | R8 | yes | 1 byte を R に書く |
| Mono8 | RGBA8 | yes | R=G=B=mono, A=255 |
| BayerRG8 | RGBA8 | yes | RGGB Bayer demosaic |
| BayerGR8 | RGBA8 | yes | GRBG Bayer demosaic |
| BayerGB8 | RGBA8 | yes | GBRG Bayer demosaic |
| BayerBG8 | RGBA8 | yes | BGGR Bayer demosaic |
| BGR8 | RGBA8 | yes | B,G,R byte を R,G,B に並べ替え、A=255 |
| BGRa8 | RGBA8 | yes | B,G,R,a byte を R,G,B,A に並べ替え |
| Bayer*8 | R8 | no | 初期実装では unsupported |
| BGR8 | R8 | no | 初期実装では unsupported |
| BGRa8 | R8 | no | 初期実装では unsupported |

10/12bit packed Bayer、16bit Bayer、YUV、Polarized 系は初期実装では unsupported とする。

## 3. raw memory layout

compute shader は IC4 image buffer を raw byte buffer として読む。

入力 offset は row pitch を考慮する。

```txt
offset = y * inputRowPitchBytes + x * bytesPerPixel
```

bytesPerPixel:

```txt
Mono8    -> 1
Bayer8   -> 1
BGR8     -> 3
BGRa8    -> 4
```

BGR8 は 24-bit であるため、D3D11 texture として直接扱わず、raw byte buffer として shader へ渡す。

## 4. Bayer8 -> RGBA8

初期実装は高品質 demosaic でなくてよい。まずは bilinear demosaic を実装する。

Bayer pattern:

```txt
BayerRG8: R G / G B
BayerGR8: G R / B G
BayerGB8: G B / R G
BayerBG8: B G / G R
```

edge pixel は clamp sampling でよい。

shader 関数例:

```hlsl
uint LoadByte(uint x, uint y)
{
    uint offset = y * inputRowPitchBytes + x;
    return RawInput.Load(offset) & 0xff;
}
```

実装者は pattern ごとに現在 pixel が R/G/B のどれかを判断し、不足 channel を上下左右または斜め近傍から補間する。

初期実装では速度優先でよい。後で高品質化できるように shader file を分ける。

## 5. BGR8 -> RGBA8

入力:

```txt
byte0 = B
byte1 = G
byte2 = R
```

出力:

```txt
RGBA = (R, G, B, 255)
```

row pitch を必ず使う。`width * 3` と仮定してはならない。

## 6. BGRa8 -> RGBA8

IC4 SDK の表記は `BGRa8` である。IC4Ext の enum も `BGRa8` とする。

入力:

```txt
byte0 = B
byte1 = G
byte2 = R
byte3 = A
```

出力:

```txt
RGBA = (R, G, B, A)
```

## 7. Mono8 -> R8

入力 1 byte を output texture の R channel に書く。

```txt
R = mono
```

output texture format は `DXGI_FORMAT_R8_UNORM`。

UAV support を `CheckFormatSupport` で確認する。未対応なら converter 初期化失敗にする。

## 8. Mono8 -> RGBA8

入力 1 byte を RGB へ複製し、A=255 にする。

```txt
RGBA = (mono, mono, mono, 255)
```

## 9. Shader file 名

推奨 shader file:

```txt
shaders/d3d11/IC4Ext_Convert_Bayer8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_BGR8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_BGRa8_To_RGBA8.hlsl
shaders/d3d11/IC4Ext_Convert_Mono8_To_R8.hlsl
shaders/d3d11/IC4Ext_Convert_Mono8_To_RGBA8.hlsl
```

同名 `.cso` もサポートする。

```txt
IC4Ext_Convert_Bayer8_To_RGBA8.cso
...
```

## 10. D3D11FrameConverter API

```cpp
class D3D11FrameConverter
{
public:
    bool initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    D3D11FenceManager* fenceManager,
                    const ShaderLoadConfig& shaderConfig);

    bool isSupported(CameraPixelFormat input,
                     GpuFrameFormat output) const;

    bool convert(const PendingIC4Frame& input,
                 const FrameOutputSpec& outputSpec,
                 D3D11CameraFrame& outFrame);

    const ErrorInfo& lastError() const noexcept;
};
```

`convert()` は次を行う。

```txt
1. input format / output format が supported か確認
2. raw byte buffer を作成または再利用
3. IC4 image buffer の raw bytes を raw byte buffer へ upload
4. output texture / SRV / UAV を作成
5. 対応 compute shader を bind
6. constant buffer を設定
7. Dispatch
8. UAV/SRV を unbind
9. fence signal
10. outFrame に texture, srv, uav, metadata, timing, ready token を設定
```

## 11. resource reuse

初期実装では毎 frame resource を作ってもよい。ただし、高 fps カメラでは負荷が大きくなるため、実装者は次を cache してよい。

- raw input buffer
- output texture pool
- constant buffer
- shader object
- SRV/UAV descriptor

resource reuse を行う場合でも、返却済み frame の texture を上書きしてはならない。consumer が所有する frame は独立 resource とする。

## 12. unsupported error

unsupported 変換を要求された場合、`open()` 時点で検出できるなら open 失敗にする。runtime にしか判定できない場合は `read()` が失敗 result を返す。

error message には少なくとも次を含める。

```txt
input format
output format
supported conversion list
```

## 13. テスト観点

実機カメラなしで可能な unit test:

- BGR8 raw buffer から RGBA8 の channel order が正しい。
- BGRa8 raw buffer から RGBA8 の alpha が維持される。
- Mono8 -> RGBA8 の RGB 複製が正しい。
- Mono8 -> R8 が正しい。
- Bayer pattern ごとに 2x2 / 4x4 の基本色配置が破綻しない。
- unsupported conversion が明示的に失敗する。

実機カメラあり test:

- requestedFormat と actualInputFormat が一致する。
- output texture の width / height / DXGI format が config と一致する。


---

# 05. D3D11CameraCapture 設計 v1.3

## 1. 目的

`D3D11CameraCapture` は、IC4 camera から取得した frame を D3D11 GPU texture として返す同期的な capture class である。

OpenCV `VideoCapture` に近い使用感を目指すが、返却されるのは CPU image ではなく `D3D11CameraFrame` である。

## 2. 公開 API

```cpp
class D3D11CameraCapture
{
public:
    D3D11CameraCapture();
    ~D3D11CameraCapture();

    D3D11CameraCapture(const D3D11CameraCapture&) = delete;
    D3D11CameraCapture& operator=(const D3D11CameraCapture&) = delete;

    D3D11CameraCapture(D3D11CameraCapture&&) noexcept;
    D3D11CameraCapture& operator=(D3D11CameraCapture&&) noexcept;

    bool open(const IC4DeviceSelector& selector,
              const CameraCaptureConfig& config,
              D3D11CoreLib::D3D11Core* core);

    void close() noexcept;
    bool isOpened() const noexcept;

    ReadResult read(ReadMode mode = ReadMode::LatestFrame);
    ReadResult read(const CameraReadOptions& options);

    CameraCaptureStats stats() const;
    const ErrorInfo& lastError() const noexcept;
};
```

`D3D11CameraCapture` は move-only とする。copy は禁止する。

## 3. ReadResult

```cpp
struct ReadResult
{
    bool ok = false;
    D3D11CameraFrame frame;
    ErrorInfo error;

    explicit operator bool() const noexcept { return ok; }
};
```

timeout は error ではなく `ok == false` とし、`error.code` に timeout を示す値を入れる。

## 4. CameraReadOptions

```cpp
struct CameraReadOptions
{
    ReadMode mode = ReadMode::LatestFrame;
    std::uint32_t timeoutMs = 1000;
};
```

`read(ReadMode)` は `timeoutMs = 1000` の簡易 overload として実装してよい。

## 5. 内部状態

```cpp
class D3D11CameraCapture
{
private:
    IC4DeviceSelector selector_;
    CameraCaptureConfig config_;

    D3D11CoreLib::D3D11Core* core_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    std::unique_ptr<D3D11FenceManager> fenceManager_;
    std::unique_ptr<D3D11FrameConverter> converter_;

    ic4::Grabber grabber_;
    std::shared_ptr<ic4::QueueSink> queueSink_;
    std::shared_ptr<InternalQueueSinkListener> listener_;

    mutable std::mutex pendingMutex_;
    std::condition_variable pendingCv_;
    std::deque<PendingIC4Frame> pendingFrames_;

    std::atomic<bool> opened_{false};
    CameraCaptureStats stats_;
    ErrorInfo lastError_;
};
```

実際には `ic4::Grabber` の default construction / invalid state を IC4 SDK の仕様に合わせて扱う。

## 6. open() 手順

`open()` は次の順に実装する。

```txt
1. すでに open 済みなら close()
2. core != nullptr を確認
3. core から ID3D11Device / ID3D11DeviceContext を取得
4. D3D11FenceManager::initialize()
   - ID3D11Device5 / ID3D11DeviceContext4 / ID3D11Fence が使えなければ失敗
5. converter 初期化
   - requestedFormat -> outputFormat が supported か確認
   - shader load config に従って .cso / .hlsl を読む
6. IC4 device selector resolve
7. grabber.deviceOpen(deviceInfo)
8. device property 設定
   - PixelFormat = requestedFormat
   - Width / Height / FPS if specified
9. QueueSink listener 作成
10. QueueSink::create(listener, sinkConfig)
11. grabber.streamSetup(queueSink, AcquisitionStart)
12. sinkConnected で negotiated format を検証
13. opened_ = true
```

失敗した場合は `close()` 相当の cleanup を行い、`opened_ = false` にする。

## 7. read() 手順

`read()` は次を行う。

```txt
1. opened_ を確認
2. pending queue から PendingIC4Frame を取り出す
   - LatestFrame: 最新だけ取り出し、古いものを破棄
   - NextFrame: 最古を 1 枚取り出す
3. converter.convert() で D3D11CameraFrame に変換
4. stats を更新
5. ReadResult{ ok=true, frame=... } を返す
```

pending queue が空の場合、`CameraReadOptions::timeoutMs` まで condition variable で待つ。

### 7.1 LatestFrame

```cpp
PendingIC4Frame takeLatest()
{
    // pendingFrames_ の最後だけ取得
    // それ以外は破棄し、droppedPendingBuffers を増やす
}
```

用途:

- preview
- 表示
- 低遅延優先

### 7.2 NextFrame

```cpp
PendingIC4Frame takeNext()
{
    // pendingFrames_ の先頭を取得
    // 残りは維持
}
```

用途:

- 録画
- 解析
- `D3D11CameraCaptureThread`

## 8. thread safety

`D3D11CameraCapture` は、複数 thread から同時に `read()` されることを想定しない。

許可する操作:

- IC4 callback thread が pending queue に push する。
- 利用者 thread が `read()` で pending queue から pop する。

禁止する操作:

- 複数 consumer が同じ `D3D11CameraCapture` に対して同時に `read()` する。
- `read()` と `close()` を外部から同時に呼ぶ。
- `D3D11CameraCaptureThread` に move した capture に外部から触る。

必要な同期:

- pending queue は mutex で保護する。
- stats は atomic または mutex で保護する。
- callback と close の競合に注意する。

## 9. 出力 frame の所有権

`read()` が返した `D3D11CameraFrame` の texture は consumer が所有する。

以後の `read()` が同じ texture を上書きしてはならない。

高 fps 対応のため resource pool を使う場合でも、未解放 frame を再利用してはならない。pool は参照が戻った resource のみ再利用する。

## 10. ready token の意味

`read()` は GPU conversion dispatch 後、CPU wait せずに frame を返す。

返却 frame の `ready` には fence token が入る。

consumer は texture を使う前に必要なら次を呼ぶ。

```cpp
frame.ready.wait();
```

sample/test では wait して完了確認してよい。

## 11. close()

`close()` は noexcept とし、可能な限り cleanup する。

順序:

```txt
1. opened_ = false
2. grabber.acquisitionStop() 可能なら実行
3. grabber.streamStop() 可能なら実行
4. pending queue clear
5. queueSink_ reset
6. listener_ reset
7. converter_ reset
8. fenceManager_ reset
9. D3D11 raw pointer を null
```

IC4 SDK の `streamStop()` は callback 終了を待つ可能性がある。callback 内から `close()` を呼ばない。

## 12. lastError

`lastError_` には失敗箇所をわかるように入れる。

例:

```txt
where = "D3D11CameraCapture::open / CreateFence"
message = "ID3D11Device5::CreateFence failed"
```

```txt
where = "D3D11CameraCapture::open / PixelFormat"
message = "Requested PixelFormat BayerRG12p is not supported in initial implementation"
```

## 13. 最小使用例

```cpp
D3D11CoreLib::D3D11Core core;
// core.initialize(...)

IC4Ext::IC4DeviceSelector selector;
selector.deviceIndex = 0;

IC4Ext::CameraCaptureConfig config;
config.streamRequest.width = 1920;
config.streamRequest.height = 1080;
config.streamRequest.fps = 60.0;
config.streamRequest.requestedFormat = IC4Ext::CameraPixelFormat::BGR8;
config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;

IC4Ext::D3D11CameraCapture cap;
if (!cap.open(selector, config, &core)) {
    std::cerr << cap.lastError().message << std::endl;
    return;
}

while (true) {
    auto result = cap.read(IC4Ext::ReadMode::LatestFrame);
    if (!result) {
        continue;
    }

    auto& frame = result.frame;
    frame.ready.wait();
    // frame.texture / frame.srv を使用
}
```


---

# 06. D3D11CameraCaptureThread 設計 v1.3

## 1. 目的

`D3D11CameraCaptureThread` は、`D3D11CameraCapture::read(ReadMode::NextFrame)` を別 thread で繰り返し呼び、取得した frame を ThreadKit queue へ配送する補助 class である。

この class は録画・解析など、frame を順に処理したい用途を主対象にする。低遅延 preview だけが目的なら、利用者が直接 `D3D11CameraCapture::read(ReadMode::LatestFrame)` を呼べばよい。

## 2. 基本方針

- `D3D11CameraCaptureThread` は内部に `D3D11CameraCapture` を所有する。
- 通常 constructor は `CameraCapture` の初期化引数を受け取り、内部で capture を open する。
- 既存 capture を受け取る場合は `D3D11CameraCapture&&` の move 専用にする。
- 参照や pointer で外部 capture を共有しない。
- worker loop では必ず `ReadMode::NextFrame` を使う。
- output queue 登録時は必ず `cameraIndex` を指定する。

## 3. options

```cpp
struct CameraThreadOptions
{
    std::uint32_t readTimeoutMs = 1000;
    bool copyPerOutputQueue = true;
    bool stopOnReadError = false;
};
```

- `readTimeoutMs`: 内部 `read()` の timeout。
- `copyPerOutputQueue`: 複数 queue に流す場合、consumer ごとに独立 texture を作るか。
- `stopOnReadError`: timeout 以外の read error で thread を止めるか。

## 4. 公開 API

```cpp
class D3D11CameraCaptureThread
{
public:
    D3D11CameraCaptureThread(IC4DeviceSelector selector,
                             CameraCaptureConfig config,
                             D3D11CoreLib::D3D11Core* core,
                             CameraThreadOptions options = {});

    explicit D3D11CameraCaptureThread(D3D11CameraCapture&& capture,
                                      CameraThreadOptions options = {});

    ~D3D11CameraCaptureThread();

    D3D11CameraCaptureThread(const D3D11CameraCaptureThread&) = delete;
    D3D11CameraCaptureThread& operator=(const D3D11CameraCaptureThread&) = delete;

    bool open();
    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    void addOutputQueue(std::uint32_t cameraIndex,
                        std::shared_ptr<D3D11IndexedFrameQueue> queue);

    CameraThreadStats stats() const;
    const ErrorInfo& lastError() const noexcept;
};
```

constructor ではまだ open せず、`open()` で内部 capture を開く設計でもよい。実装を単純化したい場合、constructor で必要情報を保存し、`start()` 時に未 open なら open する。

## 5. capture 初期化引数 constructor

推奨 constructor:

```cpp
D3D11CameraCaptureThread(IC4DeviceSelector selector,
                         CameraCaptureConfig config,
                         D3D11CoreLib::D3D11Core* core,
                         CameraThreadOptions options = {});
```

この constructor は `selector`、`config`、`core` を保存する。

内部 capture を open するとき、thread 用途に合わせて次を補正する。

```cpp
config.queuePolicy = FrameQueuePolicy::PreserveFrames;
if (config.maxPendingBuffers == 1) {
    config.maxPendingBuffers = 0; // または十分大きい値
}
```

理由: `CameraCaptureThread` は `ReadMode::NextFrame` で frame を順番に処理するため、default の `LatestOnly` では意図せず frame が落ちる。

## 6. move capture constructor

既存 capture を渡す場合は move 専用にする。

```cpp
explicit D3D11CameraCaptureThread(D3D11CameraCapture&& capture,
                                  CameraThreadOptions options = {});
```

この constructor を呼んだ後、元の capture object は moved-from 状態であり、利用者は触ってはならない。

実装者は、moved-from capture の `isOpened()` が false を返すようにしてよい。

禁止 API:

```cpp
D3D11CameraCaptureThread(D3D11CameraCapture& capture);        // 禁止
D3D11CameraCaptureThread(D3D11CameraCapture* capture);        // 禁止
D3D11CameraCaptureThread(std::shared_ptr<D3D11CameraCapture>); // 禁止
```

## 7. output queue 登録

出力 queue は必ず cameraIndex と一緒に登録する。

```cpp
void addOutputQueue(std::uint32_t cameraIndex,
                    std::shared_ptr<D3D11IndexedFrameQueue> queue);
```

`queue == nullptr` は error。

1 台カメラでも必ず `cameraIndex = 0` として登録する。

同じ `cameraIndex` で複数 queue を登録してよい。例えば表示用 queue と録画用 queue の両方に camera0 frame を流せる。

内部表現:

```cpp
struct OutputBinding
{
    std::uint32_t cameraIndex = 0;
    std::shared_ptr<D3D11IndexedFrameQueue> queue;
};

std::vector<OutputBinding> outputs_;
```

## 8. dispatch アルゴリズム

worker loop:

```txt
while !stopRequested:
  result = capture.read({ ReadMode::NextFrame, readTimeoutMs })
  if result timeout:
    stats.readTimeouts++
    continue
  if result error:
    stats.readErrors++
    if stopOnReadError: break
    continue

  frame = std::move(result.frame)
  dispatchFrameToOutputs(frame)
```

`read()` は `NextFrame` 固定である。

## 9. 複数 output queue への配送

output queue が N 個ある場合、原則として consumer ごとに独立 resource を渡す。

```txt
N == 0:
  frame を破棄し stats.noOutputDrops++

N == 1:
  frame を move して push

N >= 2:
  N-1 個 copy を作る
  最後の queue に元 frame を move
```

copy には `D3D11FrameCopier` を使う。

```cpp
for i in 0..N-2:
    D3D11CameraFrame copied;
    copier.copyFrame(frame, copied);
    outputs[i].queue->push({ outputs[i].cameraIndex, std::move(copied) });

outputs[N-1].queue->push({ outputs[N-1].cameraIndex, std::move(frame) });
```

copy 完了を CPU wait しない。copy frame には copy 後の fence token を入れる。

## 10. queue push 方針

ThreadKit queue の capacity / drop policy は利用者が queue 作成時に決める。

`D3D11CameraCaptureThread` は queue push が失敗した場合、stats に記録する。

想定される失敗:

- queue が closed
- capacity overflow
- stop requested

push が block する queue を使う場合、thread stop が遅れる可能性がある。ThreadKit 側に timeout push があるならそれを使う。

## 11. stats

```cpp
struct CameraThreadStats
{
    std::uint64_t readFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t pushedFrames = 0;
    std::uint64_t pushFailures = 0;
    std::uint64_t copiedFrames = 0;
    std::uint64_t copyFailures = 0;
    std::uint64_t noOutputDrops = 0;
};
```

## 12. thread 停止

`requestStop()` は worker thread に停止要求を出す。

`join()` は thread 終了を待つ。

`stopAndJoin()` は両方を行う。

Destructor は `stopAndJoin()` を呼ぶ。ただし destructor で長時間 block する可能性は README に明記する。

## 13. 最小使用例

```cpp
using Queue = IC4Ext::D3D11IndexedFrameQueue;

auto queue = std::make_shared<Queue>(/* ThreadKit options */);

IC4Ext::IC4DeviceSelector selector;
selector.deviceIndex = 0;

IC4Ext::CameraCaptureConfig config;
config.streamRequest.requestedFormat = IC4Ext::CameraPixelFormat::BGR8;
config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;

IC4Ext::D3D11CameraCaptureThread thread(selector, config, &core);
thread.addOutputQueue(0, queue);

if (!thread.open()) {
    std::cerr << thread.lastError().message << std::endl;
    return;
}

thread.start();

while (true) {
    IC4Ext::D3D11IndexedCameraFrame indexed;
    if (queue->pop(indexed)) {
        indexed.frame.ready.wait();
        std::cout << "camera=" << indexed.cameraIndex
                  << " frame=" << indexed.frame.timing.frameNumber
                  << std::endl;
    }
}
```


---

# 07. D3D11FrameSyncThread 設計 v1.3

## 1. 目的

`D3D11FrameSyncThread` は、`D3D11CameraCaptureThread` から出力された `D3D11IndexedCameraFrame` を受け取り、cameraIndex と timestamp / frame number に基づいて同期済み frame set を作る class である。

初期実装・初期テストは 1 台カメラ前提でよい。ただし queue 型は最初から `cameraIndex` 付きにし、将来の複数カメラ同期を壊さない。

## 2. 入力型

```cpp
struct D3D11IndexedCameraFrame
{
    std::uint32_t cameraIndex = 0;
    D3D11CameraFrame frame;
};

using D3D11IndexedFrameQueue =
    ThreadKit::Queues::BlockingQueue<D3D11IndexedCameraFrame>;
```

`D3D11FrameSyncThread` はこの queue を入力にする。

## 3. 出力型

同期済み frame set:

```cpp
struct D3D11SyncedFrameSet
{
    std::vector<D3D11IndexedCameraFrame> frames;
    std::uint64_t syncGroupId = 0;
    std::chrono::steady_clock::time_point emittedTime;
};

using D3D11SyncedFrameQueue =
    ThreadKit::Queues::BlockingQueue<D3D11SyncedFrameSet>;
```

1 台カメラの場合、`frames.size() == 1` の set を出力する。

## 4. Sync policy

```cpp
enum class FrameSyncPolicy
{
    PassThroughSingleCamera,
    TimestampNearest,
    FrameNumberExact,
};
```

初期実装で必須なのは `PassThroughSingleCamera` である。

- `PassThroughSingleCamera`: 1 台入力をそのまま set に包んで出力する。
- `TimestampNearest`: 複数 camera の timestamp が近い frame を組にする。将来拡張。
- `FrameNumberExact`: frame number が一致する frame を組にする。将来拡張。

## 5. options

```cpp
struct FrameSyncOptions
{
    FrameSyncPolicy policy = FrameSyncPolicy::PassThroughSingleCamera;
    std::vector<std::uint32_t> cameraIndices = {0};
    std::uint64_t maxTimestampDiffNs = 1'000'000; // 1 ms, multi-camera 用
    std::size_t maxBufferedFramesPerCamera = 8;
};
```

初期実装では `cameraIndices.size() == 1` のみ実機 test 対象にする。

## 6. API

```cpp
class D3D11FrameSyncThread
{
public:
    D3D11FrameSyncThread(std::shared_ptr<D3D11IndexedFrameQueue> inputQueue,
                         std::shared_ptr<D3D11SyncedFrameQueue> outputQueue,
                         FrameSyncOptions options = {});

    bool start();
    void requestStop();
    void join();
    void stopAndJoin();

    FrameSyncStats stats() const;
    const ErrorInfo& lastError() const noexcept;
};
```

入力 queue は aggregate queue を推奨する。つまり、複数 camera thread が同じ input queue に `D3D11IndexedCameraFrame` を push する。

```txt
camera0 thread -> aggregate indexed input queue
camera1 thread -> aggregate indexed input queue
aggregate queue -> FrameSyncThread
```

初期 sample/test では camera0 のみが aggregate queue に push する。

## 7. PassThroughSingleCamera

1 台カメラ用 policy。

処理:

```txt
while !stopRequested:
  inputQueue.pop(indexedFrame)
  if indexedFrame.cameraIndex が options.cameraIndices[0] と一致:
      set.frames = { indexedFrame }
      set.syncGroupId++
      outputQueue.push(set)
  else:
      stats.ignoredFrames++
```

この policy は GPU ready token を wait しない。frame の ready token はそのまま保持する。

## 8. TimestampNearest 将来設計

複数カメラの場合、cameraIndex ごとに小さな buffer を持つ。

```cpp
std::unordered_map<std::uint32_t, std::deque<D3D11IndexedCameraFrame>> buffers;
```

新 frame 到着時に、基準 frame の timestamp に最も近い frame を各 camera buffer から選ぶ。

同期成立条件:

```txt
すべての対象 camera に候補 frame がある
かつ max(timestamp) - min(timestamp) <= maxTimestampDiffNs
```

成立したら set を出力し、使った frame を buffer から削除する。

古すぎる frame は drop し、stats に記録する。

## 9. FrameNumberExact 将来設計

frame number が全 camera で一致する場合のみ set を出力する。

この policy は camera 間で共通 frame number が得られる場合にのみ有効である。IC4 device timestamp が camera ごとに独立している場合は `TimestampNearest` を使う。

## 10. ready token の扱い

`D3D11FrameSyncThread` は同期判断のために GPU 完了を待たない。

理由:

- 同期判断に必要なのは timestamp / frame number である。
- GPU wait は後段 consumer が texture を実際に使う直前に行えばよい。

したがって、`D3D11SyncedFrameSet` 内の各 frame は元の `D3D11ReadyToken` を保持する。

## 11. stats

```cpp
struct FrameSyncStats
{
    std::uint64_t inputFrames = 0;
    std::uint64_t emittedSets = 0;
    std::uint64_t ignoredFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t pushFailures = 0;
};
```

## 12. 初期テスト範囲

手元にカメラが 1 台しかない前提のため、実機 test は次に限定する。

- cameraIndex=0 の frame を input queue に流す。
- `PassThroughSingleCamera` で `frames.size() == 1` の set が出力される。
- `cameraIndex` が保持される。
- `D3D11ReadyToken` が保持される。

複数カメラ同期は実機 test ではなく、人工的な `D3D11IndexedCameraFrame` を使った unit test に留める。


---

# 08. Build / sample / test 設計 v1.3

## 1. ライブラリ形式

IC4Ext は header-only ではなく、通常の C++ library として実装する。

```txt
include/  public headers
src/      implementation files
shaders/  hlsl/cso
samples/  sample programs
tests/    tests
```

static library / shared library のどちらでもよいが、初期実装では static library を推奨する。

## 2. CMake options

D3D11 backend のみなので、D3D12 enable option は作らない。

```cmake
option(IC4EXT_BUILD_SAMPLES "Build IC4Ext samples" ON)
option(IC4EXT_BUILD_TESTS "Build IC4Ext tests" ON)
set(IC4_SDK_ROOT "" CACHE PATH "Path to IC Imaging Control 4 SDK root")
set(D3D11HELPER_ROOT "" CACHE PATH "Path to D3D11Helper")
set(THREADKIT_ROOT "" CACHE PATH "Path to ThreadKit")
```

`IC4_SDK_ROOT` 例:

```txt
C:/Users/user/AppData/Local/Programs/The Imaging Source Europe GmbH/IC Imaging Control 4
```

## 3. IC4 include / lib

CMake では次を設定する。

```cmake
target_include_directories(IC4Ext PUBLIC
    "${IC4_SDK_ROOT}/include"
)

target_link_directories(IC4Ext PUBLIC
    "${IC4_SDK_ROOT}/lib/x64"
)

target_link_libraries(IC4Ext PUBLIC
    ic4core
)
```

`ic4gui` は link しない。

## 4. runtime DLL

実行時には次が見える必要がある。

```txt
${IC4_SDK_ROOT}/bin/x64/ic4core.dll
```

実装者は sample/test 実行時に次のどちらかを行う。

1. `${IC4_SDK_ROOT}/bin/x64` を PATH に追加する。
2. post-build step で `ic4core.dll` を executable directory へ copy する。

plugin DLL は IC4 runtime が必要に応じて使う可能性があるため、PATH 方式を推奨する。

## 5. D3D11Helper / ThreadKit

D3D11Helper と ThreadKit は project に直接追加、または CMake subdirectory として追加してよい。

想定:

```cmake
add_subdirectory(${D3D11HELPER_ROOT} D3D11Helper_build)
add_subdirectory(${THREADKIT_ROOT} ThreadKit_build)

target_link_libraries(IC4Ext PUBLIC
    D3D11Helper
    ThreadKit
)
```

実際の target 名は外部 library の CMake に合わせること。

ThreadKit が header-only の場合は include directory のみでよい。

## 6. Windows / SDK 要件

D3D11 fence を使うため、ビルド環境には `ID3D11Device5` / `ID3D11DeviceContext4` / `ID3D11Fence` が見える Windows SDK が必要である。

include:

```cpp
#include <d3d11_4.h>
```

link:

```txt
d3d11.lib
dxgi.lib
d3dcompiler.lib  // .hlsl runtime compile を使う場合
```

`QueryInterface(ID3D11Device5)` などが実行時に失敗した場合、IC4Ext は fallback せず初期化失敗にする。

## 7. shader build

`.hlsl` と `.cso` の両対応にする。

開発時:

```txt
shaders/d3d11/*.hlsl を runtime compile
```

配布時:

```txt
shaders/d3d11/*.cso を同梱
```

CMake で `.hlsl` を `.cso` に compile する custom command を追加してよい。

例:

```cmake
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/IC4Ext_Convert_BGR8_To_RGBA8.cso
    COMMAND fxc /T cs_5_0 /E main /Fo ... IC4Ext_Convert_BGR8_To_RGBA8.hlsl
    DEPENDS IC4Ext_Convert_BGR8_To_RGBA8.hlsl
)
```

DXC を使う場合は target profile と bytecode compatibility を確認する。

## 8. sample: SingleCameraLog

初期 sample は window 表示を行わない。

目的:

- カメラ 1 台を open する。
- D3D11 texture として frame を取得する。
- frame metadata を console に出す。

コマンドライン例:

```txt
SingleCameraLog.exe --device-index 0 --width 1920 --height 1080 --fps 60 --format BGR8 --output RGBA8
```

出力例:

```txt
opened camera index=0
frame=1 timestampNs=... 1920x1080 RGBA8 ready=1
frame=2 timestampNs=... 1920x1080 RGBA8 ready=1
```

sample では frame を使う前に `frame.ready.wait()` を呼んでよい。

## 9. sample main の流れ

```txt
1. コマンドライン引数 parse
2. D3D11Helper で D3D11Core 初期化
3. IC4DeviceSelector 作成
4. CameraCaptureConfig 作成
5. D3D11CameraCapture open
6. loop:
   - read(LatestFrame)
   - ready.wait()
   - metadata print
7. Ctrl+C または指定 frame 数で終了
```

## 10. tests 方針

手元にカメラが 1 台しかない前提で test を設計する。

### 10.1 unit tests: カメラ不要

- enum mapping test
- unsupported format test
- selector priority test
- shader path resolution test
- `D3D11ReadyToken` invalid token test
- synthetic raw buffer conversion test

### 10.2 D3D11 device tests: カメラ不要

- D3D11Core 初期化
- `ID3D11Device5` / `ID3D11DeviceContext4` が取れること
- fence create / signal / wait
- output texture 作成
- shader compile / cso load

### 10.3 single camera tests: カメラ 1 台必要

- first device open
- deviceIndex=0 open
- requestedFormat 設定
- `read(LatestFrame)` が成功する
- `read(NextFrame)` が成功する
- 返却 frame の texture が null でない
- width / height / output format metadata が正しい
- `frame.ready.wait()` が成功する
- `D3D11CameraCaptureThread` が cameraIndex=0 付き frame を queue に push する
- `D3D11FrameSyncThread` の `PassThroughSingleCamera` が 1 frame set を出す

## 11. tests で避けること

初期 test では次を要求しない。

- 複数カメラ実機同期
- window / swapchain 表示
- D3D12 backend
- 10/12bit Bayer
- 長時間 soak test
- 厳密な画質評価

## 12. README に書くべき制約

実装 README には最低限次を書く。

```txt
- 初期版は D3D11 のみ対応。
- D3D12 backend は含まない。
- IC4 SDK の ic4core.lib / ic4core.dll が必要。
- D3D11 fence が使える Windows 環境が必要。
- 対応入力 format は Mono8 / Bayer8 / BGR8 / BGRa8。
- 10/12bit packed Bayer は未対応。
- sample/test は 1 台カメラ前提。
```
