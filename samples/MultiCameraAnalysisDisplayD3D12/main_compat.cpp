#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
}

bool HasFlag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

std::vector<int> ParseDevices(const std::string& text)
{
    std::vector<int> result;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(',', begin);
        const auto token = text.substr(
            begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (!token.empty()) result.push_back(std::stoi(token));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

enum class TriggerMode
{
    None,
    Hardware,
    Software,
};

TriggerMode ParseTriggerMode(const std::string& value)
{
    if (value == "none") return TriggerMode::None;
    if (value == "hardware") return TriggerMode::Hardware;
    if (value == "software") return TriggerMode::Software;
    throw std::runtime_error("--trigger-mode must be none, hardware, or software");
}

const char* TriggerModeName(TriggerMode mode)
{
    switch (mode) {
    case TriggerMode::None: return "none";
    case TriggerMode::Hardware: return "hardware";
    case TriggerMode::Software: return "software";
    default: return "unknown";
    }
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& value)
{
    IC4Ext::CameraPixelFormat format{};
    if (!IC4Ext::ParseCameraPixelFormat(value, format)) {
        throw std::runtime_error("Unsupported --format value: " + value);
    }
    return format;
}

std::filesystem::path ResolveDefaultGamma1Path(const char* executablePath)
{
    const auto sourceTreePath = std::filesystem::path("samples") /
                                "MultiCameraAnalysisDisplayD3D12" /
                                "config" /
                                "gamma1.json";
    if (std::filesystem::exists(sourceTreePath)) return sourceTreePath;

    if (executablePath && *executablePath) {
        std::error_code error;
        const auto executable = std::filesystem::absolute(executablePath, error);
        if (!error) {
            const auto copiedPath = executable.parent_path() / "config" / "gamma1.json";
            if (std::filesystem::exists(copiedPath)) return copiedPath;
        }
    }
    return sourceTreePath;
}

struct Options
{
    std::vector<int> devices{0, 1};
    TriggerMode triggerMode = TriggerMode::None;
    std::string triggerSource = "Line1";
    std::uint64_t maxTimestampDiffNs = 10'000'000;

    int width = 0;
    int height = 0;
    double fps = 0.0;
    IC4Ext::CameraPixelFormat cameraFormat = IC4Ext::CameraPixelFormat::BayerRG8;
    bool formatSpecified = false;

    std::filesystem::path ic4JsonPath;
    std::size_t ic4JsonDeviceIndex = 0;
    int offsetX = 236;
    int offsetY = 0;

    int cameraSetupDelayMs = 1000;
    int cameraOpenRetries = 3;
    int cameraRetryDelayMs = 3000;

    int canvasWidth = 1600;
    int canvasHeight = 900;
    int maxSets = 0;
    int motionThreshold = 24;
    int minMotionArea = 400;
    std::filesystem::path recordPath;
    double recordFps = 0.0;
};

Options ParseOptions(int argc, char** argv)
{
    Options options;
    options.ic4JsonPath = ResolveDefaultGamma1Path(argc > 0 ? argv[0] : nullptr);

    if (const char* value = ArgValue(argc, argv, "--devices")) {
        options.devices = ParseDevices(value);
    }
    if (const char* value = ArgValue(argc, argv, "--trigger-mode")) {
        options.triggerMode = ParseTriggerMode(value);
    }
    if (const char* value = ArgValue(argc, argv, "--trigger-source")) {
        options.triggerSource = value;
    }
    if (const char* value = ArgValue(argc, argv, "--sync-policy")) {
        if (std::string(value) != "timestamp") {
            throw std::runtime_error(
                "This sample uses HostReceived timestamp synchronization. "
                "Use --sync-policy timestamp or omit the option.");
        }
    }
    if (const char* value = ArgValue(argc, argv, "--max-timestamp-diff-ns")) {
        options.maxTimestampDiffNs = std::strtoull(value, nullptr, 10);
    }

    if (const char* value = ArgValue(argc, argv, "--ic4-json")) {
        options.ic4JsonPath = value;
    }
    if (const char* value = ArgValue(argc, argv, "--ic4-json-device-index")) {
        options.ic4JsonDeviceIndex =
            static_cast<std::size_t>(std::strtoull(value, nullptr, 10));
    }
    if (HasFlag(argc, argv, "--no-ic4-json")) options.ic4JsonPath.clear();

    if (const char* value = ArgValue(argc, argv, "--offset-x")) {
        options.offsetX = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--offset-y")) {
        options.offsetY = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--width")) options.width = std::atoi(value);
    if (const char* value = ArgValue(argc, argv, "--height")) options.height = std::atoi(value);
    if (const char* value = ArgValue(argc, argv, "--fps")) options.fps = std::atof(value);
    if (const char* value = ArgValue(argc, argv, "--format")) {
        options.cameraFormat = ParseCameraFormat(value);
        options.formatSpecified = true;
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
    if (const char* value = ArgValue(argc, argv, "--camera-start-delay-ms")) {
        options.cameraSetupDelayMs = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--camera-start-retries")) {
        options.cameraOpenRetries = std::atoi(value);
    }

    if (const char* value = ArgValue(argc, argv, "--canvas-width")) {
        options.canvasWidth = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--canvas-height")) {
        options.canvasHeight = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--sets")) options.maxSets = std::atoi(value);
    if (const char* value = ArgValue(argc, argv, "--motion-threshold")) {
        options.motionThreshold = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--min-motion-area")) {
        options.minMotionArea = std::atoi(value);
    }
    if (const char* value = ArgValue(argc, argv, "--record")) options.recordPath = value;
    if (const char* value = ArgValue(argc, argv, "--record-fps")) {
        options.recordFps = std::atof(value);
    }

    if (options.devices.size() < 2) {
        throw std::runtime_error("--devices must contain at least two indices");
    }
    if (options.maxTimestampDiffNs == 0) {
        throw std::runtime_error("--max-timestamp-diff-ns must be > 0");
    }
    if (options.offsetX < 0 || options.offsetY < 0) {
        throw std::runtime_error("Offsets must be >= 0");
    }
    if (options.cameraSetupDelayMs < 0 || options.cameraRetryDelayMs < 0 ||
        options.cameraOpenRetries < 1) {
        throw std::runtime_error("Invalid camera setup retry options");
    }
    if (options.canvasWidth <= 0 || options.canvasHeight <= 0) {
        throw std::runtime_error("Canvas size must be positive");
    }
    if (!options.ic4JsonPath.empty() && !std::filesystem::exists(options.ic4JsonPath)) {
        throw std::runtime_error(
            "IC4 JSON state file was not found: " + options.ic4JsonPath.string());
    }
    return options;
}

IC4Ext::CameraCaptureConfig MakeCameraConfig(const Options& options)
{
    IC4Ext::CameraCaptureConfig config;
    if (!options.ic4JsonPath.empty()) {
        config.ic4StateJson.path = options.ic4JsonPath;
        config.ic4StateJson.deviceIndex = options.ic4JsonDeviceIndex;
        config.ic4StateJson.strict = false;
        config.ic4StateJson.applyNestedSelectorStates = true;
    }

    config.streamRequest.width = options.width;
    config.streamRequest.height = options.height;
    config.streamRequest.fps = options.fps;
    config.streamRequest.requestedFormat = options.cameraFormat;
    config.streamRequest.forceRequestedFormat = options.formatSpecified;
    config.streamRequest.offsetX = options.offsetX;
    config.streamRequest.offsetY = options.offsetY;
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config.maxPendingBuffers = 32;
    config.shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / "d3d12";

    // The validated compatibility lifecycle uses normal stream setup followed by
    // the typed stopAcquisition() call before the next camera is prepared.
    config.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;

    if (options.triggerMode == TriggerMode::Hardware) {
        IC4Ext::ConfigureHardwareTriggerSync(config, options.triggerSource);
    } else if (options.triggerMode == TriggerMode::Software) {
        IC4Ext::ConfigureSoftwareTriggerSync(config);
    } else {
        auto gate = IC4Ext::MakeSoftwareTriggerSyncConfig();
        gate.setExposureAutoOff = false;
        IC4Ext::ConfigureCameraSync(config, gate);
    }
    return config;
}

bool OpenAndPauseCapture(IC4Ext::D3D12CameraCapture& capture,
                         const IC4Ext::IC4DeviceSelector& selector,
                         const IC4Ext::CameraCaptureConfig& config,
                         const IC4Ext::D3D12BackendContext& backend,
                         std::size_t cameraSlot,
                         int deviceIndex,
                         int maxAttempts,
                         int retryDelayMs)
{
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        std::cout << "Preparing camera slot=" << cameraSlot
                  << " deviceIndex=" << deviceIndex
                  << " attempt=" << attempt << "/" << maxAttempts << std::endl;

        if (capture.open(selector, config, backend)) {
            if (capture.stopAcquisition()) {
                std::cout << "Prepared camera slot=" << cameraSlot
                          << " deviceIndex=" << deviceIndex
                          << " (stream configured, acquisition paused)" << std::endl;
                return true;
            }
            const auto error = capture.lastError();
            std::cerr << "stopAcquisition failed slot=" << cameraSlot
                      << " deviceIndex=" << deviceIndex << ": "
                      << error.where << ": " << error.message << std::endl;
            capture.close();
        } else {
            const auto error = capture.lastError();
            std::cerr << "Camera prepare failed slot=" << cameraSlot
                      << " deviceIndex=" << deviceIndex
                      << " attempt=" << attempt << "/" << maxAttempts << ": "
                      << error.where << ": " << error.message << std::endl;
        }

        if (attempt < maxAttempts && retryDelayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }
    return false;
}

struct MotionState
{
    cv::Mat previousGray;
};

void AnalyzeAndAnnotate(cv::Mat& bgr,
                        MotionState& state,
                        int physicalDeviceIndex,
                        int thresholdValue,
                        int minArea)
{
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    const double meanLuminance = cv::mean(gray)[0];

    double changedRatio = 0.0;
    std::size_t regionCount = 0;
    if (!state.previousGray.empty() && state.previousGray.size() == gray.size()) {
        cv::Mat difference;
        cv::absdiff(gray, state.previousGray, difference);
        cv::threshold(difference, difference, thresholdValue, 255, cv::THRESH_BINARY);
        cv::morphologyEx(
            difference,
            difference,
            cv::MORPH_OPEN,
            cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
        cv::dilate(
            difference,
            difference,
            cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));

        changedRatio = static_cast<double>(cv::countNonZero(difference)) /
                       static_cast<double>(difference.total());

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(difference, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (const auto& contour : contours) {
            const cv::Rect rectangle = cv::boundingRect(contour);
            if (rectangle.area() >= minArea) {
                ++regionCount;
                cv::rectangle(bgr, rectangle, cv::Scalar(0, 255, 255), 2);
            }
        }
    }
    gray.copyTo(state.previousGray);

    std::ostringstream line1;
    line1 << "Device " << physicalDeviceIndex;
    std::ostringstream line2;
    line2 << std::fixed << std::setprecision(1)
          << "Luma: " << meanLuminance
          << "  Motion: " << (changedRatio * 100.0) << "%"
          << "  Regions: " << regionCount;

    cv::rectangle(bgr, cv::Rect(0, 0, bgr.cols, 58), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(bgr, line1.str(), cv::Point(12, 23), cv::FONT_HERSHEY_SIMPLEX,
                0.65, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    cv::putText(bgr, line2.str(), cv::Point(12, 49), cv::FONT_HERSHEY_SIMPLEX,
                0.55, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
}

cv::Mat CpuFrameToBgr(const IC4Ext::CpuFrame& frame)
{
    if (frame.format != IC4Ext::CpuFrameFormat::RGBA8) {
        throw std::runtime_error("Sample expects RGBA8 readback");
    }
    cv::Mat rgba(static_cast<int>(frame.height),
                 static_cast<int>(frame.width),
                 CV_8UC4,
                 const_cast<std::uint8_t*>(frame.data.data()),
                 frame.rowPitch);
    cv::Mat bgr;
    cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
    return bgr;
}

struct Grid
{
    int columns = 1;
    int rows = 1;
    int gap = 8;
};

Grid MakeGrid(std::size_t count)
{
    Grid grid;
    grid.columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    grid.rows = static_cast<int>((count + static_cast<std::size_t>(grid.columns) - 1) /
                                 static_cast<std::size_t>(grid.columns));
    if (count == 2) {
        grid.columns = 2;
        grid.rows = 1;
    }
    return grid;
}

void PlaceLetterboxed(const cv::Mat& source, cv::Mat& canvas, const cv::Rect& cell)
{
    const double scale = std::min(
        static_cast<double>(cell.width) / source.cols,
        static_cast<double>(cell.height) / source.rows);
    const int width = std::max(1, static_cast<int>(std::lround(source.cols * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(source.rows * scale)));

    cv::Mat resized;
    cv::resize(source, resized, cv::Size(width, height), 0.0, 0.0, cv::INTER_LINEAR);
    const int x = cell.x + (cell.width - width) / 2;
    const int y = cell.y + (cell.height - height) / 2;
    resized.copyTo(canvas(cv::Rect(x, y, width, height)));
}

void PrintPipelineStats(
    const std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>>& cameras,
    const IC4Ext::D3D12FrameSyncThread& syncThread,
    int displayedSets)
{
    const auto syncStats = syncThread.stats();
    std::cout << "sets=" << displayedSets
              << " syncInput=" << syncStats.inputFrames
              << " syncEmitted=" << syncStats.emittedSets
              << " syncDropped=" << syncStats.droppedFrames
              << " syncIgnored=" << syncStats.ignoredFrames;
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        const auto stats = cameras[index]->stats();
        std::cout << " camera" << index
                  << "{read=" << stats.readFrames
                  << ",pushed=" << stats.pushedFrames
                  << ",errors=" << stats.readErrors << "}";
    }
    std::cout << std::endl;
}

void StopPipeline(
    std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>>& cameras,
    IC4Ext::D3D12FrameSyncThread& syncThread)
{
    for (auto& camera : cameras) {
        if (camera) camera->stopAcquisition();
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

        std::cout << "Multi-camera setup:"
                  << " lifecycle=compatibility-prepare-stop"
                  << " ic4Json="
                  << (options.ic4JsonPath.empty() ? "<disabled>" : options.ic4JsonPath.string())
                  << " jsonDeviceIndex=" << options.ic4JsonDeviceIndex
                  << " offsetX=" << options.offsetX
                  << " offsetY=" << options.offsetY
                  << " formatOverride="
                  << (options.formatSpecified ? IC4Ext::ToString(options.cameraFormat) : "<json>")
                  << " widthOverride=" << options.width
                  << " heightOverride=" << options.height
                  << " fpsOverride=" << options.fps
                  << " triggerMode=" << TriggerModeName(options.triggerMode)
                  << " timestampSource=host-received"
                  << " maxTimestampDiffNs=" << options.maxTimestampDiffNs
                  << std::endl;

        const auto cameraConfig = MakeCameraConfig(options);
        std::vector<IC4Ext::D3D12CameraCapture> preparedCaptures;
        preparedCaptures.reserve(options.devices.size());

        for (std::size_t index = 0; index < options.devices.size(); ++index) {
            IC4Ext::IC4DeviceSelector selector;
            selector.deviceIndex = options.devices[index];

            IC4Ext::D3D12CameraCapture capture;
            if (!OpenAndPauseCapture(
                    capture,
                    selector,
                    cameraConfig,
                    backend,
                    index,
                    options.devices[index],
                    options.cameraOpenRetries,
                    options.cameraRetryDelayMs)) {
                throw std::runtime_error(
                    "Camera prepare permanently failed slot=" + std::to_string(index) +
                    " deviceIndex=" + std::to_string(options.devices[index]) + ": " +
                    capture.lastError().where + ": " + capture.lastError().message);
            }
            preparedCaptures.push_back(std::move(capture));

            if (index + 1 < options.devices.size() && options.cameraSetupDelayMs > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options.cameraSetupDelayMs));
            }
        }

        ThreadKit::Queues::QueueOptions inputOptions;
        inputOptions.maxSize = 128;
        auto inputQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);

        ThreadKit::Queues::QueueOptions outputOptions;
        outputOptions.maxSize = 8;
        auto outputQueue = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

        IC4Ext::FrameSyncOptions syncOptions;
        syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
        syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
        syncOptions.maxTimestampDiffNs = options.maxTimestampDiffNs;
        syncOptions.maxBufferedFramesPerCamera = 32;
        for (std::size_t index = 0; index < options.devices.size(); ++index) {
            syncOptions.cameraIndices.push_back(static_cast<std::uint32_t>(index));
        }

        IC4Ext::D3D12FrameSyncThread syncThread(inputQueue, outputQueue, syncOptions);
        if (!syncThread.start()) throw std::runtime_error(syncThread.lastError().message);

        IC4Ext::CameraThreadOptions threadOptions;
        threadOptions.readTimeoutMs = 1000;
        threadOptions.copyPerOutputQueue = false;
        threadOptions.stopOnReadError = false;

        std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>> cameras;
        cameras.reserve(preparedCaptures.size());
        for (std::size_t index = 0; index < preparedCaptures.size(); ++index) {
            auto camera = std::make_unique<IC4Ext::D3D12CameraCaptureThread>(
                std::move(preparedCaptures[index]), backend, threadOptions);
            camera->addOutputQueue(static_cast<std::uint32_t>(index), inputQueue);
            if (!camera->start()) {
                throw std::runtime_error(
                    "Camera worker start failed slot=" + std::to_string(index) + ": " +
                    camera->lastError().where + ": " + camera->lastError().message);
            }
            cameras.push_back(std::move(camera));
        }

        if (options.triggerMode == TriggerMode::None) {
            for (std::size_t index = 0; index < cameras.size(); ++index) {
                if (!cameras[index]->setIC4Property("TriggerMode", std::string("Off"))) {
                    StopPipeline(cameras, syncThread);
                    throw std::runtime_error("Failed to release free-run trigger gate");
                }
            }
        }

        for (std::size_t index = 0; index < cameras.size(); ++index) {
            if (!cameras[index]->startAcquisition()) {
                StopPipeline(cameras, syncThread);
                throw std::runtime_error(
                    "startAcquisition failed slot=" + std::to_string(index) + ": " +
                    cameras[index]->lastError().where + ": " +
                    cameras[index]->lastError().message);
            }
            std::cout << "Acquisition started slot=" << index
                      << " deviceIndex=" << options.devices[index] << std::endl;
        }

        std::vector<IC4Ext::D3D12FrameReadback> readbacks(cameras.size());
        for (auto& readback : readbacks) {
            if (!readback.initialize(backend)) {
                StopPipeline(cameras, syncThread);
                throw std::runtime_error(readback.lastError().message);
            }
        }

        const Grid grid = MakeGrid(cameras.size());
        const int cellWidth =
            (options.canvasWidth - grid.gap * (grid.columns + 1)) / grid.columns;
        const int cellHeight =
            (options.canvasHeight - grid.gap * (grid.rows + 1)) / grid.rows;
        cv::Mat canvas(
            options.canvasHeight,
            options.canvasWidth,
            CV_8UC3,
            cv::Scalar(16, 16, 16));
        std::vector<MotionState> motionStates(cameras.size());

        cv::VideoWriter writer;
        if (!options.recordPath.empty()) {
            const double fps = options.recordFps > 0.0
                                   ? options.recordFps
                                   : (options.fps > 0.0 ? options.fps : 160.0);
            writer.open(
                options.recordPath.string(),
                cv::VideoWriter::fourcc('a', 'v', 'c', '1'),
                fps,
                canvas.size(),
                true);
            if (!writer.isOpened()) {
                writer.open(
                    options.recordPath.string(),
                    cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                    fps,
                    canvas.size(),
                    true);
            }
            if (!writer.isOpened()) {
                StopPipeline(cameras, syncThread);
                throw std::runtime_error("Failed to open VideoWriter");
            }
        }

        cv::namedWindow("IC4Ext Multi-Camera Analysis", cv::WINDOW_NORMAL);
        int emittedSets = 0;
        bool running = true;
        auto lastStatsPrint = std::chrono::steady_clock::now();

        while (running && (options.maxSets <= 0 || emittedSets < options.maxSets)) {
            if (options.triggerMode == TriggerMode::Software) {
                for (auto& camera : cameras) {
                    if (!camera->softwareTrigger()) {
                        StopPipeline(cameras, syncThread);
                        throw std::runtime_error("softwareTrigger failed");
                    }
                }
            }

            auto set = outputQueue->waitPopFor(std::chrono::milliseconds(100));
            if (!set) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastStatsPrint >= std::chrono::seconds(1)) {
                    PrintPipelineStats(cameras, syncThread, emittedSets);
                    lastStatsPrint = now;
                }
                const int key = cv::waitKey(1);
                running = key != 27 && key != 'q' && key != 'Q';
                continue;
            }

            canvas.setTo(cv::Scalar(16, 16, 16));
            for (const auto& indexed : set->frames) {
                if (indexed.cameraIndex >= readbacks.size()) continue;

                IC4Ext::CpuFrame cpuFrame;
                auto& readback = readbacks[indexed.cameraIndex];
                if (!readback.readback(
                        indexed.frame,
                        IC4Ext::CpuFrameFormat::RGBA8,
                        cpuFrame,
                        5000)) {
                    StopPipeline(cameras, syncThread);
                    throw std::runtime_error(
                        "readback failed: " + readback.lastError().message);
                }

                cv::Mat bgr = CpuFrameToBgr(cpuFrame);
                AnalyzeAndAnnotate(
                    bgr,
                    motionStates[indexed.cameraIndex],
                    options.devices[indexed.cameraIndex],
                    options.motionThreshold,
                    options.minMotionArea);

                const int column = static_cast<int>(indexed.cameraIndex) % grid.columns;
                const int row = static_cast<int>(indexed.cameraIndex) / grid.columns;
                const cv::Rect cell(
                    grid.gap + column * (cellWidth + grid.gap),
                    grid.gap + row * (cellHeight + grid.gap),
                    cellWidth,
                    cellHeight);
                PlaceLetterboxed(bgr, canvas, cell);
            }

            cv::imshow("IC4Ext Multi-Camera Analysis", canvas);
            if (writer.isOpened()) writer.write(canvas);

            ++emittedSets;
            if ((emittedSets % 30) == 0) {
                PrintPipelineStats(cameras, syncThread, emittedSets);
                lastStatsPrint = std::chrono::steady_clock::now();
            }

            const int key = cv::waitKey(1);
            running = key != 27 && key != 'q' && key != 'Q';
        }

        StopPipeline(cameras, syncThread);
        writer.release();
        cv::destroyAllWindows();
        core->WaitIdle();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "MultiCameraAnalysisDisplayD3D12 failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
