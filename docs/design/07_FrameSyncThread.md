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
