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
