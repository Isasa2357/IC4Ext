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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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

std::vector<int> ParseDeviceIndices(const std::string& text)
{
    std::vector<int> result;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(',', begin);
        const auto token = text.substr(begin, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - begin);
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

enum class TriggerMode
{
    None,
    Hardware,
    Software,
};

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
    std::uint64_t maxTimestampDiffNs = 10'000'000;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int canvasWidth = 1600;
    int canvasHeight = 900;
    int maxSets = 0;
    int cameraSetupDelayMs = 1000;
    int cameraOpenRetries = 3;
    int cameraRetryDelayMs = 3000;
    IC4Ext::CameraPixelFormat cameraFormat = IC4Ext::CameraPixelFormat::BGR8;
    std::filesystem::path recordPath;
    int recordBitrate = 16'000'000;
};

Options ParseOptions(int argc, char** argv)
{
    Options options;
    if (const char* value = ArgValue(argc, argv, "--devices")) {
        options.deviceIndices = ParseDeviceIndices(value);
    }
    if (const char* value = ArgValue(argc, argv, "--trigger-mode")) {
        options.triggerMode = ParseTriggerMode(value);
    }
    if (const char* value = ArgValue(argc, argv, "--sync-policy")) {
        options.syncPolicy = ParseSyncPolicy(value);
    }
    if (const char* value = ArgValue(argc, argv, "--trigger-source")) {
        options.triggerSource = value;
    }
    if (const char* value = ArgValue(argc, argv, "--max-timestamp-diff-ns")) {
        options.maxTimestampDiffNs = std::strtoull(value, nullptr, 10);
    }
    if (const char* value = ArgValue(argc, argv, "--width")) options.width = std::atoi(value);
    if (const char* value = ArgValue(argc, argv, "--height")) options.height = std::atoi(value);
    if (const char* value = ArgValue(argc, argv, "--fps")) options.fps = std::atof(value);
    if (const char* value = ArgValue(argc, argv, "--canvas-width")) {
        options.canvasWidth = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--canvas-height")) {
        options.canvasHeight = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--sets")) options.maxSets = std::atoi(value);
    if (const char* value = ArgValue(argc, argv, "--format")) {
        options.cameraFormat = ParseCameraFormat(value);
    }
    if (const char* value = ArgValue(argc, argv, "--camera-setup-delay-ms")) {
        options.cameraSetupDelayMs = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--camera-open-retries")) {
        options.cameraOpenRetries = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--camera-retry-delay-ms")) {
        options.cameraRetryDelayMs = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--record")) options.recordPath = value;
    if (const char* value = ArgValue(argc, argv, "--record-bitrate")) {
        options.recordBitrate = std::atoi(value);
    }

    if (options.deviceIndices.size() < 2) {
        throw std::runtime_error("--devices must contain at least two indices, e.g. 0,1");
    }
    if (options.canvasWidth <= 0 || options.canvasHeight <= 0) {
        throw std::runtime_error("Canvas size must be positive");
    }
    if (options.cameraOpenRetries < 1 || options.cameraSetupDelayMs < 0 ||
        options.cameraRetryDelayMs < 0) {
        throw std::runtime_error("Invalid camera setup/retry option");
    }
    return options;
}

IC4Ext::CameraCaptureConfig MakeCameraConfig(const Options& options)
{
    IC4Ext::CameraCaptureConfig config;
    config.streamRequest.width = options.width;
    config.streamRequest.height = options.height;
    config.streamRequest.fps = options.fps;
    config.streamRequest.requestedFormat = options.cameraFormat;
    config.streamRequest.forceRequestedFormat = true;
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config.maxPendingBuffers = 32;
    config.shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / "d3d12";
    config.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Deferred;

    switch (options.triggerMode) {
    case TriggerMode::Hardware:
        IC4Ext::ConfigureHardwareTriggerSync(config, options.triggerSource);
        break;
    case TriggerMode::Software:
        IC4Ext::ConfigureSoftwareTriggerSync(config);
        break;
    case TriggerMode::None:
    default:
        IC4Ext::ConfigureNoSync(config);
        break;
    }
    return config;
}

bool OpenDeferredCapture(IC4Ext::D3D12CameraCapture& capture,
                         const IC4Ext::IC4DeviceSelector& selector,
                         const IC4Ext::CameraCaptureConfig& config,
                         const IC4Ext::D3D12BackendContext& backend,
                         std::size_t slot,
                         int deviceIndex,
                         int attempts,
                         int retryDelayMs)
{
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        std::cout << "Preparing camera slot=" << slot
                  << " deviceIndex=" << deviceIndex
                  << " attempt=" << attempt << "/" << attempts << std::endl;
        if (capture.open(selector, config, backend)) {
            if (capture.isStreaming() && !capture.isAcquisitionActive()) {
                std::cout << "Prepared camera slot=" << slot
                          << " deviceIndex=" << deviceIndex
                          << " (stream configured, acquisition deferred)" << std::endl;
                return true;
            }
            std::cerr << "Deferred camera entered an invalid lifecycle state\n";
            capture.close();
        } else {
            const auto error = capture.lastError();
            std::cerr << "Camera prepare failed: " << error.where
                      << ": " << error.message << std::endl;
        }

        if (attempt < attempts && retryDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }
    return false;
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
    layout.rows = static_cast<int>((count + static_cast<std::size_t>(layout.columns) - 1) /
                                   static_cast<std::size_t>(layout.columns));
    if (count == 2) {
        layout.columns = 2;
        layout.rows = 1;
    }
    return layout;
}

void BlitLetterboxedRgba(const IC4Ext::CpuFrame& source,
                         std::vector<std::uint8_t>& destination,
                         int destinationWidth,
                         int destinationHeight,
                         int cellX,
                         int cellY,
                         int cellWidth,
                         int cellHeight)
{
    (void)destinationHeight;
    if (source.width == 0 || source.height == 0 || source.data.empty()) return;

    const double scale = std::min(static_cast<double>(cellWidth) / source.width,
                                  static_cast<double>(cellHeight) / source.height);
    const int drawWidth = std::max(1, static_cast<int>(std::lround(source.width * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::lround(source.height * scale)));
    const int drawX = cellX + (cellWidth - drawWidth) / 2;
    const int drawY = cellY + (cellHeight - drawHeight) / 2;

    for (int y = 0; y < drawHeight; ++y) {
        const std::uint32_t sourceY = std::min<std::uint32_t>(
            source.height - 1,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height) /
                                       drawHeight));
        const auto* sourceRow =
            source.data.data() + static_cast<std::size_t>(sourceY) * source.rowPitch;
        auto* destinationRow =
            destination.data() +
            (static_cast<std::size_t>(drawY + y) * destinationWidth + drawX) * 4u;

        for (int x = 0; x < drawWidth; ++x) {
            const std::uint32_t sourceX = std::min<std::uint32_t>(
                source.width - 1,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * source.width) /
                                           drawWidth));
            const auto* pixel = sourceRow + static_cast<std::size_t>(sourceX) * 4u;
            auto* output = destinationRow + static_cast<std::size_t>(x) * 4u;
            output[0] = pixel[2];
            output[1] = pixel[1];
            output[2] = pixel[0];
            output[3] = 255;
        }
    }
}

class DisplayWindow
{
public:
    DisplayWindow(int width, int height)
        : width_(width),
          height_(height),
          pixels_(static_cast<std::size_t>(width) * height * 4u, 0)
    {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &DisplayWindow::WndProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"IC4ExtMultiCameraSyncDisplay";
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&windowClass);

        RECT rectangle{0, 0, width_, height_};
        AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0,
                                windowClass.lpszClassName,
                                L"IC4Ext Multi-Camera Sync",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                rectangle.right - rectangle.left,
                                rectangle.bottom - rectangle.top,
                                nullptr,
                                nullptr,
                                windowClass.hInstance,
                                this);
        if (!hwnd_) throw std::runtime_error("CreateWindowExW failed");
    }

    bool pumpMessages()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
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
    static LRESULT CALLBACK WndProc(HWND hwnd,
                                    UINT message,
                                    WPARAM wParam,
                                    LPARAM lParam)
    {
        auto* self = reinterpret_cast<DisplayWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<DisplayWindow*>(create->lpCreateParams);
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
        PAINTSTRUCT paintStructure{};
        HDC deviceContext = BeginPaint(hwnd_, &paintStructure);
        RECT client{};
        GetClientRect(hwnd_, &client);

        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = width_;
        bitmap.bmiHeader.biHeight = -height_;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(deviceContext,
                      0,
                      0,
                      client.right,
                      client.bottom,
                      0,
                      0,
                      width_,
                      height_,
                      pixels_.data(),
                      &bitmap,
                      DIB_RGB_COLORS,
                      SRCCOPY);
        EndPaint(hwnd_, &paintStructure);
    }

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool closed_ = false;
    std::vector<std::uint8_t> pixels_;
};

#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
class D3D12CanvasUploader
{
public:
    void initialize(D3D12CoreLib::D3D12Core& core, UINT width, UINT height)
    {
        core_ = &core;
        width_ = width;
        height_ = height;
        auto* device = core.GetDevice();

        D3D12_RESOURCE_DESC textureDescription{};
        textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDescription.Width = width;
        textureDescription.Height = height;
        textureDescription.DepthOrArraySize = 1;
        textureDescription.MipLevels = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &textureDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&texture_)))) {
            throw std::runtime_error("Failed to create recording texture");
        }

        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(
            &textureDescription, 0, 1, 0, &footprint_, nullptr, nullptr, &uploadSize);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDescription{};
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = uploadSize;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload_)))) {
            throw std::runtime_error("Failed to create recording upload buffer");
        }

        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
            FAILED(device->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocator_.Get(),
                                             nullptr,
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
        if (FAILED(upload_->Map(0, &emptyRange, &mapped))) {
            throw std::runtime_error("Upload Map failed");
        }
        auto* destination = static_cast<std::uint8_t*>(mapped) + footprint_.Offset;
        const std::size_t sourcePitch = static_cast<std::size_t>(width_) * 4u;
        for (UINT y = 0; y < height_; ++y) {
            const auto* sourceRow = bgra.data() + static_cast<std::size_t>(y) * sourcePitch;
            auto* destinationRow =
                destination + static_cast<std::size_t>(y) * footprint_.Footprint.RowPitch;
            for (UINT x = 0; x < width_; ++x) {
                destinationRow[x * 4u + 0u] = sourceRow[x * 4u + 2u];
                destinationRow[x * 4u + 1u] = sourceRow[x * 4u + 1u];
                destinationRow[x * 4u + 2u] = sourceRow[x * 4u + 0u];
                destinationRow[x * 4u + 3u] = 255;
            }
        }
        upload_->Unmap(0, nullptr);

        allocator_->Reset();
        commandList_->Reset(allocator_.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload_.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint_;
        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = texture_.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commandList_->CopyTextureRegion(&destinationLocation, 0, 0, 0, &source, nullptr);
        commandList_->Close();

        ID3D12CommandList* commandLists[] = {commandList_.Get()};
        auto* queue = core_->GetDirectCommandQueue();
        queue->ExecuteCommandLists(1, commandLists);
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
#endif

void StopPipeline(
    std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>>& cameras,
    IC4Ext::D3D12FrameSyncThread& syncThread)
{
    for (auto& camera : cameras) {
        if (camera && camera->isAcquisitionActive()) camera->stopAcquisition();
    }
    for (auto& camera : cameras) {
        if (camera) camera->stopAndJoin();
    }
    syncThread.stopAndJoin();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = ParseOptions(argc, argv);
        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
        if (!backend.resolve()) throw std::runtime_error("Failed to resolve D3D12 backend");

        const auto cameraConfig = MakeCameraConfig(options);
        std::vector<IC4Ext::D3D12CameraCapture> preparedCaptures;
        preparedCaptures.reserve(options.deviceIndices.size());

        for (std::size_t i = 0; i < options.deviceIndices.size(); ++i) {
            IC4Ext::IC4DeviceSelector selector;
            selector.deviceIndex = options.deviceIndices[i];
            IC4Ext::D3D12CameraCapture capture;
            if (!OpenDeferredCapture(capture,
                                     selector,
                                     cameraConfig,
                                     backend,
                                     i,
                                     options.deviceIndices[i],
                                     options.cameraOpenRetries,
                                     options.cameraRetryDelayMs)) {
                throw std::runtime_error("Failed to prepare all cameras");
            }
            preparedCaptures.push_back(std::move(capture));
            if (i + 1 < options.deviceIndices.size() && options.cameraSetupDelayMs > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options.cameraSetupDelayMs));
            }
        }

        ThreadKit::Queues::QueueOptions inputOptions;
        inputOptions.maxSize = 128;
        auto inputQueue =
            std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);
        ThreadKit::Queues::QueueOptions outputOptions;
        outputOptions.maxSize = 8;
        auto outputQueue =
            std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = options.syncPolicy;
        syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
        syncOptions.maxTimestampDiffNs = options.maxTimestampDiffNs;
        syncOptions.maxBufferedFramesPerCamera = 32;
        for (std::size_t i = 0; i < options.deviceIndices.size(); ++i) {
            syncOptions.cameraIndices.push_back(static_cast<std::uint32_t>(i));
        }

        IC4Ext::D3D12FrameSyncThread syncThread(inputQueue, outputQueue, syncOptions);
        if (!syncThread.start()) throw std::runtime_error(syncThread.lastError().message);

        std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>> cameras;
        cameras.reserve(preparedCaptures.size());
        IC4Ext::CameraThreadOptions cameraThreadOptions;
        cameraThreadOptions.readTimeoutMs = 1000;
        cameraThreadOptions.copyPerOutputQueue = false;

        for (std::size_t i = 0; i < preparedCaptures.size(); ++i) {
            auto camera = std::make_unique<IC4Ext::D3D12CameraCaptureThread>(
                std::move(preparedCaptures[i]), backend, cameraThreadOptions);
            camera->addOutputQueue(static_cast<std::uint32_t>(i), inputQueue);
            if (!camera->start()) {
                throw std::runtime_error("Camera worker start failed: " +
                                         camera->lastError().where + ": " +
                                         camera->lastError().message);
            }
            cameras.push_back(std::move(camera));
        }

        for (std::size_t i = 0; i < cameras.size(); ++i) {
            if (!cameras[i]->startAcquisition()) {
                StopPipeline(cameras, syncThread);
                throw std::runtime_error("startAcquisition failed: " +
                                         cameras[i]->lastError().message);
            }
            std::cout << "Acquisition started slot=" << i
                      << " deviceIndex=" << options.deviceIndices[i] << std::endl;
        }

        std::vector<IC4Ext::D3D12FrameReadback> readbacks(cameras.size());
        for (auto& readback : readbacks) {
            if (!readback.initialize(backend)) {
                StopPipeline(cameras, syncThread);
                throw std::runtime_error(readback.lastError().message);
            }
        }

        DisplayWindow window(options.canvasWidth, options.canvasHeight);
        const GridLayout grid = MakeGridLayout(cameras.size());
        std::vector<std::uint8_t> canvas(
            static_cast<std::size_t>(options.canvasWidth) * options.canvasHeight * 4u,
            0);

#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
        std::unique_ptr<D3DVideoEncoderLib::D3D12VideoEncoder> encoder;
        std::unique_ptr<D3D12CanvasUploader> uploader;
        if (!options.recordPath.empty()) {
            D3DVideoEncoderLib::D3D12VideoEncoderDesc description;
            description.outputPath = options.recordPath.wstring();
            description.width = static_cast<std::uint32_t>(options.canvasWidth);
            description.height = static_cast<std::uint32_t>(options.canvasHeight);
            description.frameRateNum = options.fps > 0.0
                                           ? static_cast<std::uint32_t>(std::lround(options.fps))
                                           : 30u;
            description.frameRateDen = 1;
            description.backend =
                D3DVideoEncoderLib::D3D12VideoEncoderBackendType::D3D12VideoEncode;
            description.codec = D3DVideoEncoderLib::VideoCodec::H264;
            description.internalFormat = D3DVideoEncoderLib::VideoPixelFormat::NV12;
            description.bitrate = static_cast<std::uint32_t>(options.recordBitrate);
            description.gopLength = description.frameRateNum;
            description.bFrameCount = 0;
            description.input.core = core.get();
            description.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.input.allowFormatConversion = true;
            description.input.processingShaderDirectory =
                std::filesystem::current_path() / "shaders" / "D3D12Processing";
            description.input.restoreStateAfterEncode = true;
            encoder = std::make_unique<D3DVideoEncoderLib::D3D12VideoEncoder>(description);
            uploader = std::make_unique<D3D12CanvasUploader>();
            uploader->initialize(*core,
                                 static_cast<UINT>(options.canvasWidth),
                                 static_cast<UINT>(options.canvasHeight));
        }
#else
        if (!options.recordPath.empty()) {
            StopPipeline(cameras, syncThread);
            throw std::runtime_error(
                "Recording was requested, but IC4Ext was built without D3DVideoEncoder support");
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
                        StopPipeline(cameras, syncThread);
                        throw std::runtime_error("softwareTrigger failed: " +
                                                 camera->lastError().message);
                    }
                }
            }

            auto set = outputQueue->waitPopFor(std::chrono::milliseconds(100));
            if (!set) continue;
            std::fill(canvas.begin(), canvas.end(), 0);

            const int cellWidth =
                (options.canvasWidth - grid.gap * (grid.columns + 1)) / grid.columns;
            const int cellHeight =
                (options.canvasHeight - grid.gap * (grid.rows + 1)) / grid.rows;
            for (const auto& indexed : set->frames) {
                if (indexed.cameraIndex >= readbacks.size()) continue;
                IC4Ext::CpuFrame cpu;
                if (!readbacks[indexed.cameraIndex].readback(
                        indexed.frame, IC4Ext::CpuFrameFormat::RGBA8, cpu, 5000)) {
                    StopPipeline(cameras, syncThread);
                    throw std::runtime_error(
                        "readback failed: " + readbacks[indexed.cameraIndex].lastError().message);
                }
                const int column = static_cast<int>(indexed.cameraIndex) % grid.columns;
                const int row = static_cast<int>(indexed.cameraIndex) / grid.columns;
                const int x = grid.gap + column * (cellWidth + grid.gap);
                const int y = grid.gap + row * (cellHeight + grid.gap);
                BlitLetterboxedRgba(cpu,
                                    canvas,
                                    options.canvasWidth,
                                    options.canvasHeight,
                                    x,
                                    y,
                                    cellWidth,
                                    cellHeight);
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

        StopPipeline(cameras, syncThread);
#if IC4EXT_SAMPLE_HAS_VIDEO_ENCODER
        if (encoder) encoder->close();
#endif
        core->WaitIdle();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "MultiCameraSyncDisplayD3D12 failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
