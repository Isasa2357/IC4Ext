#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>
#include <D3DVideoEncoder/D3D12VideoEncoderDesc.hpp>
#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>
#endif

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
}

bool HasArg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

std::vector<int> ParseDeviceIndices(const std::string& text)
{
    std::vector<int> result;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(',', begin);
        const auto token = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!token.empty()) result.push_back(std::stoi(token));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& text)
{
    IC4Ext::CameraPixelFormat format{};
    if (!IC4Ext::ParseCameraPixelFormat(text, format)) {
        throw std::runtime_error("Unsupported camera format: " + text);
    }
    return format;
}

IC4Ext::FrameSyncPolicy ParseSyncPolicy(const std::string& text)
{
    if (text == "frame-number") return IC4Ext::FrameSyncPolicy::FrameNumberExact;
    if (text == "timestamp") return IC4Ext::FrameSyncPolicy::TimestampNearest;
    throw std::runtime_error("--sync-policy must be timestamp or frame-number");
}

enum class TriggerMode { None, Hardware, Software };

TriggerMode ParseTriggerMode(const std::string& text)
{
    if (text == "none") return TriggerMode::None;
    if (text == "hardware") return TriggerMode::Hardware;
    if (text == "software") return TriggerMode::Software;
    throw std::runtime_error("--trigger-mode must be none, hardware, or software");
}

struct Options
{
    std::vector<int> deviceIndices{0, 1};
    TriggerMode triggerMode = TriggerMode::None;
    IC4Ext::FrameSyncPolicy syncPolicy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    std::string triggerSource = "Line1";
    std::uint64_t maxTimestampDiffNs = 1'000'000;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int canvasWidth = 1600;
    int canvasHeight = 900;
    int maxSets = 0;
    IC4Ext::CameraPixelFormat cameraFormat = IC4Ext::CameraPixelFormat::BGR8;
    std::filesystem::path recordPath;
    int recordBitrate = 16'000'000;
};

Options ParseOptions(int argc, char** argv)
{
    Options o;
    if (const char* v = ArgValue(argc, argv, "--devices")) o.deviceIndices = ParseDeviceIndices(v);
    if (const char* v = ArgValue(argc, argv, "--trigger-mode")) o.triggerMode = ParseTriggerMode(v);
    if (const char* v = ArgValue(argc, argv, "--sync-policy")) o.syncPolicy = ParseSyncPolicy(v);
    if (const char* v = ArgValue(argc, argv, "--trigger-source")) o.triggerSource = v;
    if (const char* v = ArgValue(argc, argv, "--max-timestamp-diff-ns")) o.maxTimestampDiffNs = std::strtoull(v, nullptr, 10);
    if (const char* v = ArgValue(argc, argv, "--width")) o.width = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--height")) o.height = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--fps")) o.fps = std::atof(v);
    if (const char* v = ArgValue(argc, argv, "--canvas-width")) o.canvasWidth = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--canvas-height")) o.canvasHeight = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--sets")) o.maxSets = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--format")) o.cameraFormat = ParseCameraFormat(v);
    if (const char* v = ArgValue(argc, argv, "--record")) o.recordPath = v;
    if (const char* v = ArgValue(argc, argv, "--record-bitrate")) o.recordBitrate = std::atoi(v);

    if (o.deviceIndices.size() < 2) throw std::runtime_error("--devices must contain at least two indices, e.g. 0,1");
    if (o.canvasWidth <= 0 || o.canvasHeight <= 0) throw std::runtime_error("Canvas size must be positive");
    return o;
}

struct GridLayout
{
    int columns = 1;
    int rows = 1;
    int gap = 8;
};

GridLayout MakeGridLayout(std::size_t count)
{
    GridLayout layout;
    layout.columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    layout.rows = static_cast<int>((count + static_cast<std::size_t>(layout.columns) - 1) / static_cast<std::size_t>(layout.columns));
    if (count == 2) {
        layout.columns = 2;
        layout.rows = 1;
    }
    return layout;
}

void BlitLetterboxedRgba(const IC4Ext::CpuFrame& src,
                         std::vector<std::uint8_t>& dst,
                         int dstWidth,
                         int dstHeight,
                         int cellX,
                         int cellY,
                         int cellWidth,
                         int cellHeight)
{
    if (src.width == 0 || src.height == 0 || src.data.empty()) return;
    const double scale = std::min(static_cast<double>(cellWidth) / src.width,
                                  static_cast<double>(cellHeight) / src.height);
    const int drawWidth = std::max(1, static_cast<int>(std::lround(src.width * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::lround(src.height * scale)));
    const int drawX = cellX + (cellWidth - drawWidth) / 2;
    const int drawY = cellY + (cellHeight - drawHeight) / 2;

    for (int y = 0; y < drawHeight; ++y) {
        const std::uint32_t sy = std::min<std::uint32_t>(src.height - 1,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * src.height) / drawHeight));
        const auto* srcRow = src.data.data() + static_cast<std::size_t>(sy) * src.rowPitch;
        auto* dstRow = dst.data() + (static_cast<std::size_t>(drawY + y) * dstWidth + drawX) * 4u;
        for (int x = 0; x < drawWidth; ++x) {
            const std::uint32_t sx = std::min<std::uint32_t>(src.width - 1,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * src.width) / drawWidth));
            const auto* p = srcRow + static_cast<std::size_t>(sx) * 4u;
            auto* q = dstRow + static_cast<std::size_t>(x) * 4u;
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
            q[3] = 255;
        }
    }
}

class DisplayWindow
{
public:
    DisplayWindow(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4u, 0)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &DisplayWindow::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"IC4ExtMultiCameraSyncDisplay";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);

        RECT rect{0, 0, width_, height_};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"IC4Ext Multi-Camera Sync",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) throw std::runtime_error("CreateWindowExW failed");
    }

    bool pumpMessages()
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return !closed_;
    }

    void update(const std::vector<std::uint8_t>& pixels)
    {
        pixels_ = pixels;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        DisplayWindow* self = reinterpret_cast<DisplayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<DisplayWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_PAINT:
            self->paint();
            return 0;
        case WM_CLOSE:
            self->closed_ = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void paint()
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width_;
        bmi.bmiHeader.biHeight = -height_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, 0, 0, client.right, client.bottom,
                      0, 0, width_, height_, pixels_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd_, &ps);
    }

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool closed_ = false;
    std::vector<std::uint8_t> pixels_;
};

class D3D12CanvasUploader
{
public:
    void initialize(D3D12CoreLib::D3D12Core& core, UINT width, UINT height)
    {
        core_ = &core;
        width_ = width;
        height_ = height;
        auto* device = core.GetDevice();

        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&texture_)))) {
            throw std::runtime_error("Failed to create recording texture");
        }

        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint_, nullptr, nullptr, &uploadSize);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = uploadSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload_)))) {
            throw std::runtime_error("Failed to create recording upload buffer");
        }

        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                             IID_PPV_ARGS(&commandList_)))) {
            throw std::runtime_error("Failed to create upload command objects");
        }
        commandList_->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            throw std::runtime_error("Failed to create upload fence");
        }
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) throw std::runtime_error("Failed to create upload fence event");
    }

    ~D3D12CanvasUploader()
    {
        if (fenceEvent_) CloseHandle(fenceEvent_);
    }

    ID3D12Resource* upload(const std::vector<std::uint8_t>& bgra)
    {
        void* mapped = nullptr;
        D3D12_RANGE emptyRange{0, 0};
        if (FAILED(upload_->Map(0, &emptyRange, &mapped))) throw std::runtime_error("Upload Map failed");
        auto* dst = static_cast<std::uint8_t*>(mapped) + footprint_.Offset;
        const std::size_t srcPitch = static_cast<std::size_t>(width_) * 4u;
        for (UINT y = 0; y < height_; ++y) {
            const auto* srcRow = bgra.data() + static_cast<std::size_t>(y) * srcPitch;
            auto* dstRow = dst + static_cast<std::size_t>(y) * footprint_.Footprint.RowPitch;
            for (UINT x = 0; x < width_; ++x) {
                dstRow[x * 4u + 0u] = srcRow[x * 4u + 2u];
                dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                dstRow[x * 4u + 2u] = srcRow[x * 4u + 0u];
                dstRow[x * 4u + 3u] = 255;
            }
        }
        upload_->Unmap(0, nullptr);

        allocator_->Reset();
        commandList_->Reset(allocator_.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload_.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint_;
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = texture_.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commandList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &src, nullptr);
        commandList_->Close();
        ID3D12CommandList* lists[] = {commandList_.Get()};
        auto* queue = core_->GetDirectCommandQueue();
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fenceValue_;
        queue->Signal(fence_.Get(), value);
        if (fence_->GetCompletedValue() < value) {
            fence_->SetEventOnCompletion(value, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        return texture_.Get();
    }

private:
    D3D12CoreLib::D3D12Core* core_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    ComPtr<ID3D12Resource> texture_;
    ComPtr<ID3D12Resource> upload_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = ParseOptions(argc, argv);
        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
        if (!backend.resolve()) throw std::runtime_error("Failed to resolve D3D12 backend");

        ThreadKit::Queues::QueueOptions inputOptions;
        inputOptions.maxSize = 128;
        auto inputQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);
        ThreadKit::Queues::QueueOptions outputOptions;
        outputOptions.maxSize = 8;
        auto outputQueue = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = options.syncPolicy;
        syncOptions.maxTimestampDiffNs = options.maxTimestampDiffNs;
        syncOptions.maxBufferedFramesPerCamera = 32;
        for (std::size_t i = 0; i < options.deviceIndices.size(); ++i) {
            syncOptions.cameraIndices.push_back(static_cast<std::uint32_t>(i));
        }
        if (!syncOptions.cameraIndices.empty() && syncOptions.cameraIndices.front() == 0 && syncOptions.cameraIndices.size() > 1) {
            syncOptions.cameraIndices.erase(syncOptions.cameraIndices.begin());
            syncOptions.cameraIndices.insert(syncOptions.cameraIndices.begin(), 0);
        }

        IC4Ext::D3D12FrameSyncThread syncThread(inputQueue, outputQueue, syncOptions);
        if (!syncThread.start()) throw std::runtime_error(syncThread.lastError().message);

        std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>> cameras;
        cameras.reserve(options.deviceIndices.size());
        IC4Ext::CameraThreadOptions cameraThreadOptions;
        cameraThreadOptions.readTimeoutMs = 1000;
        cameraThreadOptions.copyPerOutputQueue = false;

        for (std::size_t i = 0; i < options.deviceIndices.size(); ++i) {
            IC4Ext::IC4DeviceSelector selector;
            selector.deviceIndex = options.deviceIndices[i];
            IC4Ext::CameraCaptureConfig config;
            config.streamRequest.width = options.width;
            config.streamRequest.height = options.height;
            config.streamRequest.fps = options.fps;
            config.streamRequest.requestedFormat = options.cameraFormat;
            config.streamRequest.forceRequestedFormat = true;
            config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
            config.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
            config.maxPendingBuffers = 32;
            config.shaderConfig.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d12";

            if (options.triggerMode == TriggerMode::Hardware) {
                IC4Ext::ConfigureHardwareTriggerSync(config, options.triggerSource);
            } else if (options.triggerMode == TriggerMode::Software) {
                IC4Ext::ConfigureSoftwareTriggerSync(config);
            } else {
                IC4Ext::ConfigureNoSync(config);
            }

            auto camera = std::make_unique<IC4Ext::D3D12CameraCaptureThread>(selector, config, backend, cameraThreadOptions);
            camera->addOutputQueue(static_cast<std::uint32_t>(i), inputQueue);
            if (!camera->start()) {
                throw std::runtime_error("Camera start failed: " + camera->lastError().where + ": " + camera->lastError().message);
            }
            cameras.push_back(std::move(camera));
        }

        std::vector<IC4Ext::D3D12FrameReadback> readbacks(cameras.size());
        for (auto& readback : readbacks) {
            if (!readback.initialize(backend)) throw std::runtime_error(readback.lastError().message);
        }

        DisplayWindow window(options.canvasWidth, options.canvasHeight);
        const GridLayout grid = MakeGridLayout(cameras.size());
        std::vector<std::uint8_t> canvas(static_cast<std::size_t>(options.canvasWidth) * options.canvasHeight * 4u, 0);

#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
        std::unique_ptr<D3DVideoEncoderLib::D3D12VideoEncoder> encoder;
        std::unique_ptr<D3D12CanvasUploader> uploader;
        if (!options.recordPath.empty()) {
            D3DVideoEncoderLib::D3D12VideoEncoderDesc desc;
            desc.outputPath = options.recordPath.wstring();
            desc.width = static_cast<std::uint32_t>(options.canvasWidth);
            desc.height = static_cast<std::uint32_t>(options.canvasHeight);
            desc.frameRateNum = options.fps > 0.0 ? static_cast<std::uint32_t>(std::lround(options.fps)) : 30u;
            desc.frameRateDen = 1;
            desc.backend = D3DVideoEncoderLib::D3DVideoEncoderBackendType::D3D12VideoEncode;
            desc.codec = D3DVideoEncoderLib::VideoCodec::H264;
            desc.internalFormat = D3DVideoEncoderLib::VideoPixelFormat::NV12;
            desc.bitrate = static_cast<std::uint32_t>(options.recordBitrate);
            desc.gopLength = desc.frameRateNum;
            desc.bFrameCount = 0;
            desc.input.core = core.get();
            desc.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.input.allowFormatConversion = true;
            desc.input.processingShaderDirectory = std::filesystem::current_path() / "shaders" / "D3D12Processing";
            desc.input.restoreStateAfterEncode = true;
            encoder = std::make_unique<D3DVideoEncoderLib::D3D12VideoEncoder>(desc);
            uploader = std::make_unique<D3D12CanvasUploader>();
            uploader->initialize(*core, static_cast<UINT>(options.canvasWidth), static_cast<UINT>(options.canvasHeight));
        }
#else
        if (!options.recordPath.empty()) {
            throw std::runtime_error("Recording was requested, but IC4Ext was built without D3DVideoEncoder support");
        }
#endif

        int emittedSets = 0;
        bool running = true;
        while (running && (options.maxSets <= 0 || emittedSets < options.maxSets)) {
            running = window.pumpMessages();
            if (!running) break;

            if (options.triggerMode == TriggerMode::Software) {
                for (auto& camera : cameras) {
                    if (!camera->softwareTrigger()) {
                        throw std::runtime_error("softwareTrigger failed: " + camera->lastError().message);
                    }
                }
            }

            auto set = outputQueue->waitPopFor(std::chrono::milliseconds(100));
            if (!set) continue;
            std::fill(canvas.begin(), canvas.end(), 0);

            const int cellWidth = (options.canvasWidth - grid.gap * (grid.columns + 1)) / grid.columns;
            const int cellHeight = (options.canvasHeight - grid.gap * (grid.rows + 1)) / grid.rows;
            for (const auto& indexed : set->frames) {
                if (indexed.cameraIndex >= readbacks.size()) continue;
                IC4Ext::CpuFrame cpu;
                if (!readbacks[indexed.cameraIndex].readback(indexed.frame, IC4Ext::CpuFrameFormat::RGBA8, cpu, 5000)) {
                    throw std::runtime_error("readback failed: " + readbacks[indexed.cameraIndex].lastError().message);
                }
                const int column = static_cast<int>(indexed.cameraIndex) % grid.columns;
                const int row = static_cast<int>(indexed.cameraIndex) / grid.columns;
                const int x = grid.gap + column * (cellWidth + grid.gap);
                const int y = grid.gap + row * (cellHeight + grid.gap);
                BlitLetterboxedRgba(cpu, canvas, options.canvasWidth, options.canvasHeight,
                                    x, y, cellWidth, cellHeight);
            }

            window.update(canvas);
#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
            if (encoder && uploader) {
                encoder->write(uploader->upload(canvas), D3D12_RESOURCE_STATE_COPY_DEST);
            }
#endif
            ++emittedSets;
            if ((emittedSets % 30) == 0) {
                const auto stats = syncThread.stats();
                std::cout << "sets=" << emittedSets
                          << " syncInput=" << stats.inputFrames
                          << " dropped=" << stats.droppedFrames
                          << " ignored=" << stats.ignoredFrames << '\n';
            }
        }

        for (auto& camera : cameras) camera->stopAndJoin();
        syncThread.stopAndJoin();
#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
        if (encoder) encoder->close();
#endif
        core->WaitIdle();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MultiCameraSyncDisplayD3D12 failed: " << e.what() << '\n';
        return 1;
    }
}
