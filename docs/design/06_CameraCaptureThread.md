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
