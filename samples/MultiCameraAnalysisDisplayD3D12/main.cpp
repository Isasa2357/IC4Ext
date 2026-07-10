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

std::vector<int> ParseDevices(const std::string& text)
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

enum class TriggerMode { None, Hardware, Software };

TriggerMode ParseTriggerMode(const std::string& text)
{
    if (text == "none") return TriggerMode::None;
    if (text == "hardware") return TriggerMode::Hardware;
    if (text == "software") return TriggerMode::Software;
    throw std::runtime_error("--trigger-mode must be none, hardware, or software");
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& text)
{
    IC4Ext::CameraPixelFormat format{};
    if (!IC4Ext::ParseCameraPixelFormat(text, format)) {
        throw std::runtime_error("Unsupported --format value: " + text);
    }
    return format;
}

struct Options
{
    std::vector<int> devices{0, 1};
    TriggerMode triggerMode = TriggerMode::None;
    std::string triggerSource = "Line1";
    std::uint64_t maxTimestampDiffNs = 100'000'000;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    IC4Ext::CameraPixelFormat cameraFormat = IC4Ext::CameraPixelFormat::BayerRG8;
    int cameraSetupDelayMs = 1000;
    int cameraOpenRetries = 3;
    int cameraRetryDelayMs = 3000;
    int canvasWidth = 1600;
    int canvasHeight = 900;
    int maxSets = 0;
    int motionThreshold = 24;
    int minMotionArea = 400;
    std::filesystem::path recordPath;
};

Options ParseOptions(int argc, char** argv)
{
    Options o;
    if (const char* v = ArgValue(argc, argv, "--devices")) o.devices = ParseDevices(v);
    if (const char* v = ArgValue(argc, argv, "--trigger-mode")) o.triggerMode = ParseTriggerMode(v);
    if (const char* v = ArgValue(argc, argv, "--sync-policy")) {
        if (std::string(v) != "timestamp") {
            throw std::runtime_error(
                "MultiCameraAnalysisDisplayD3D12 uses host timer synchronization. "
                "--sync-policy frame-number is not supported because independent cameras can start with different frame-number offsets. "
                "Use --sync-policy timestamp or omit the option.");
        }
    }
    if (const char* v = ArgValue(argc, argv, "--trigger-source")) o.triggerSource = v;
    if (const char* v = ArgValue(argc, argv, "--max-timestamp-diff-ns")) {
        o.maxTimestampDiffNs = std::strtoull(v, nullptr, 10);
    }
    if (const char* v = ArgValue(argc, argv, "--width")) o.width = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--height")) o.height = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--fps")) o.fps = std::atof(v);
    if (const char* v = ArgValue(argc, argv, "--format")) o.cameraFormat = ParseCameraFormat(v);
    if (const char* v = ArgValue(argc, argv, "--camera-setup-delay-ms")) o.cameraSetupDelayMs = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--camera-open-retries")) o.cameraOpenRetries = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--camera-retry-delay-ms")) o.cameraRetryDelayMs = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--camera-start-delay-ms")) o.cameraSetupDelayMs = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--camera-start-retries")) o.cameraOpenRetries = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--canvas-width")) o.canvasWidth = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--canvas-height")) o.canvasHeight = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--sets")) o.maxSets = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--motion-threshold")) o.motionThreshold = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--min-motion-area")) o.minMotionArea = std::atoi(v);
    if (const char* v = ArgValue(argc, argv, "--record")) o.recordPath = v;

    if (o.devices.size() < 2) throw std::runtime_error("--devices must contain at least two indices");
    if (o.canvasWidth <= 0 || o.canvasHeight <= 0) throw std::runtime_error("Canvas size must be positive");
    if (o.cameraSetupDelayMs < 0) throw std::runtime_error("--camera-setup-delay-ms must be >= 0");
    if (o.cameraOpenRetries < 1) throw std::runtime_error("--camera-open-retries must be >= 1");
    if (o.cameraRetryDelayMs < 0) throw std::runtime_error("--camera-retry-delay-ms must be >= 0");
    if (o.maxTimestampDiffNs == 0) throw std::runtime_error("--max-timestamp-diff-ns must be > 0");
    return o;
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
    config.shaderConfig.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d12";

    if (options.triggerMode == TriggerMode::Hardware) {
        IC4Ext::ConfigureHardwareTriggerSync(config, options.triggerSource);
    } else if (options.triggerMode == TriggerMode::Software) {
        IC4Ext::ConfigureSoftwareTriggerSync(config);
    } else {
        IC4Ext::ConfigureNoSync(config);
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
            if (capture.setIC4Property("AcquisitionStop", std::string("execute"))) {
                std::cout << "Prepared camera slot=" << cameraSlot
                          << " deviceIndex=" << deviceIndex
                          << " (stream configured, acquisition paused)" << std::endl;
                return true;
            }

            const auto stopError = capture.lastError();
            std::cerr << "AcquisitionStop failed slot=" << cameraSlot
                      << " deviceIndex=" << deviceIndex
                      << ": " << stopError.where << ": " << stopError.message << std::endl;
            capture.close();
        } else {
            const auto openError = capture.lastError();
            std::cerr << "Camera prepare failed slot=" << cameraSlot
                      << " deviceIndex=" << deviceIndex
                      << " attempt=" << attempt << "/" << maxAttempts
                      << ": " << openError.where << ": " << openError.message << std::endl;
        }

        if (attempt < maxAttempts && retryDelayMs > 0) {
            std::cout << "Retrying camera deviceIndex=" << deviceIndex
                      << " after " << retryDelayMs << " ms" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }
    return false;
}

struct MotionState
{
    cv::Mat previousGray;
};

struct AnalysisResult
{
    double meanLuminance = 0.0;
    double changedRatio = 0.0;
    std::vector<cv::Rect> motionRegions;
};

AnalysisResult AnalyzeAndAnnotate(cv::Mat& bgr,
                                  MotionState& state,
                                  int physicalDeviceIndex,
                                  int thresholdValue,
                                  int minArea)
{
    AnalysisResult result;
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    result.meanLuminance = cv::mean(gray)[0];

    if (!state.previousGray.empty() && state.previousGray.size() == gray.size()) {
        cv::Mat diff;
        cv::absdiff(gray, state.previousGray, diff);
        cv::threshold(diff, diff, thresholdValue, 255, cv::THRESH_BINARY);
        cv::morphologyEx(diff, diff, cv::MORPH_OPEN,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
        cv::dilate(diff, diff,
                   cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));

        result.changedRatio = static_cast<double>(cv::countNonZero(diff)) /
                              static_cast<double>(diff.total());

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(diff, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (const auto& contour : contours) {
            const cv::Rect rect = cv::boundingRect(contour);
            if (rect.area() >= minArea) {
                result.motionRegions.push_back(rect);
                cv::rectangle(bgr, rect, cv::Scalar(0, 255, 255), 2);
            }
        }
    }

    gray.copyTo(state.previousGray);

    std::ostringstream line1;
    line1 << "Device " << physicalDeviceIndex;
    std::ostringstream line2;
    line2 << std::fixed << std::setprecision(1)
          << "Luma: " << result.meanLuminance
          << "  Motion: " << (result.changedRatio * 100.0) << "%"
          << "  Regions: " << result.motionRegions.size();

    cv::rectangle(bgr, cv::Rect(0, 0, bgr.cols, 58), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(bgr, line1.str(), cv::Point(12, 23), cv::FONT_HERSHEY_SIMPLEX,
                0.65, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    cv::putText(bgr, line2.str(), cv::Point(12, 49), cv::FONT_HERSHEY_SIMPLEX,
                0.55, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    return result;
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
    Grid g;
    g.columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    g.rows = static_cast<int>((count + static_cast<std::size_t>(g.columns) - 1) /
                              static_cast<std::size_t>(g.columns));
    if (count == 2) {
        g.columns = 2;
        g.rows = 1;
    }
    return g;
}

void PlaceLetterboxed(const cv::Mat& src, cv::Mat& canvas, const cv::Rect& cell)
{
    const double scale = std::min(static_cast<double>(cell.width) / src.cols,
                                  static_cast<double>(cell.height) / src.rows);
    const int width = std::max(1, static_cast<int>(std::lround(src.cols * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(src.rows * scale)));
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(width, height), 0.0, 0.0, cv::INTER_LINEAR);
    const int x = cell.x + (cell.width - width) / 2;
    const int y = cell.y + (cell.height - height) / 2;
    resized.copyTo(canvas(cv::Rect(x, y, width, height)));
}

void PrintPipelineStats(const std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>>& cameras,
                        const IC4Ext::D3D12FrameSyncThread& syncThread,
                        int displayedSets)
{
    const auto syncStats = syncThread.stats();
    std::cout << "sets=" << displayedSets
              << " syncInput=" << syncStats.inputFrames
              << " syncEmitted=" << syncStats.emittedSets
              << " syncDropped=" << syncStats.droppedFrames
              << " syncIgnored=" << syncStats.ignoredFrames;

    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const auto stats = cameras[i]->stats();
        std::cout << " camera" << i
                  << "{read=" << stats.readFrames
                  << ",pushed=" << stats.pushedFrames
                  << ",errors=" << stats.readErrors << "}";
    }
    std::cout << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = ParseOptions(argc, argv);
        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
        if (!backend.resolve()) throw std::runtime_error("Failed to resolve D3D12 backend");

        std::cout << "Multi-camera two-phase setup: format=" << IC4Ext::ToString(options.cameraFormat)
                  << " interCameraSetupDelayMs=" << options.cameraSetupDelayMs
                  << " retries=" << options.cameraOpenRetries
                  << " retryDelayMs=" << options.cameraRetryDelayMs
                  << " syncPolicy=timestamp-nearest"
                  << " timestampSource=host-received"
                  << " maxTimestampDiffNs=" << options.maxTimestampDiffNs << std::endl;

        std::vector<IC4Ext::D3D12CameraCapture> preparedCaptures;
        preparedCaptures.reserve(options.devices.size());
        for (std::size_t i = 0; i < options.devices.size(); ++i) {
            IC4Ext::IC4DeviceSelector selector;
            selector.deviceIndex = options.devices[i];
            IC4Ext::D3D12CameraCapture capture;
            const auto config = MakeCameraConfig(options);
            if (!OpenAndPauseCapture(capture, selector, config, backend, i, options.devices[i],
                                     options.cameraOpenRetries, options.cameraRetryDelayMs)) {
                throw std::runtime_error("Camera prepare permanently failed slot=" + std::to_string(i) +
                                         " deviceIndex=" + std::to_string(options.devices[i]) +
                                         ": " + capture.lastError().where + ": " + capture.lastError().message);
            }
            preparedCaptures.push_back(std::move(capture));
            if (i + 1 < options.devices.size() && options.cameraSetupDelayMs > 0) {
                std::cout << "Waiting " << options.cameraSetupDelayMs
                          << " ms before preparing the next camera" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(options.cameraSetupDelayMs));
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
        syncOptions.cameraIndices.clear();
        syncOptions.maxTimestampDiffNs = options.maxTimestampDiffNs;
        syncOptions.maxBufferedFramesPerCamera = 32;
        for (std::size_t i = 0; i < options.devices.size(); ++i) {
            syncOptions.cameraIndices.push_back(static_cast<std::uint32_t>(i));
        }

        IC4Ext::D3D12FrameSyncThread syncThread(inputQueue, outputQueue, syncOptions);
        if (!syncThread.start()) throw std::runtime_error(syncThread.lastError().message);

        std::vector<std::unique_ptr<IC4Ext::D3D12CameraCaptureThread>> cameras;
        cameras.reserve(preparedCaptures.size());
        IC4Ext::CameraThreadOptions threadOptions;
        threadOptions.readTimeoutMs = 1000;
        threadOptions.copyPerOutputQueue = false;
        threadOptions.stopOnReadError = false;

        for (std::size_t i = 0; i < preparedCaptures.size(); ++i) {
            auto camera = std::make_unique<IC4Ext::D3D12CameraCaptureThread>(
                std::move(preparedCaptures[i]), backend, threadOptions);
            camera->addOutputQueue(static_cast<std::uint32_t>(i), inputQueue);
            if (!camera->start()) {
                throw std::runtime_error("Camera worker start failed slot=" + std::to_string(i) +
                                         " deviceIndex=" + std::to_string(options.devices[i]) +
                                         ": " + camera->lastError().where + ": " + camera->lastError().message);
            }
            cameras.push_back(std::move(camera));
        }

        for (std::size_t i = 0; i < cameras.size(); ++i) {
            if (!cameras[i]->setIC4Property("AcquisitionStart", std::string("execute"))) {
                throw std::runtime_error("AcquisitionStart failed slot=" + std::to_string(i) +
                                         " deviceIndex=" + std::to_string(options.devices[i]) +
                                         ": " + cameras[i]->lastError().where + ": " + cameras[i]->lastError().message);
            }
            std::cout << "Acquisition started slot=" << i
                      << " deviceIndex=" << options.devices[i] << std::endl;
        }

        std::vector<IC4Ext::D3D12FrameReadback> readbacks(cameras.size());
        for (auto& readback : readbacks) {
            if (!readback.initialize(backend)) throw std::runtime_error(readback.lastError().message);
        }

        const Grid grid = MakeGrid(cameras.size());
        const int cellWidth = (options.canvasWidth - grid.gap * (grid.columns + 1)) / grid.columns;
        const int cellHeight = (options.canvasHeight - grid.gap * (grid.rows + 1)) / grid.rows;
        cv::Mat canvas(options.canvasHeight, options.canvasWidth, CV_8UC3, cv::Scalar(16, 16, 16));
        std::vector<MotionState> motionStates(cameras.size());

        cv::VideoWriter writer;
        if (!options.recordPath.empty()) {
            const double recordFps = options.fps > 0.0 ? options.fps : 30.0;
            writer.open(options.recordPath.string(), cv::VideoWriter::fourcc('a', 'v', 'c', '1'),
                        recordFps, canvas.size(), true);
            if (!writer.isOpened()) {
                writer.open(options.recordPath.string(), cv::VideoWriter::fourcc('m', 'p', '4', 'v'),
                            recordFps, canvas.size(), true);
            }
            if (!writer.isOpened()) throw std::runtime_error("Failed to open VideoWriter");
        }

        cv::namedWindow("IC4Ext Multi-Camera Analysis", cv::WINDOW_NORMAL);
        int emittedSets = 0;
        bool running = true;
        auto lastStatsPrint = std::chrono::steady_clock::now();

        while (running && (options.maxSets <= 0 || emittedSets < options.maxSets)) {
            if (options.triggerMode == TriggerMode::Software) {
                for (auto& camera : cameras) {
                    if (!camera->softwareTrigger()) {
                        throw std::runtime_error("softwareTrigger failed: " + camera->lastError().message);
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
                IC4Ext::CpuFrame cpu;
                auto& readback = readbacks[indexed.cameraIndex];
                if (!readback.readback(indexed.frame, IC4Ext::CpuFrameFormat::RGBA8, cpu, 5000)) {
                    throw std::runtime_error("readback failed: " + readback.lastError().message);
                }

                cv::Mat bgr = CpuFrameToBgr(cpu);
                AnalyzeAndAnnotate(bgr, motionStates[indexed.cameraIndex],
                                   options.devices[indexed.cameraIndex],
                                   options.motionThreshold, options.minMotionArea);

                const int column = static_cast<int>(indexed.cameraIndex) % grid.columns;
                const int row = static_cast<int>(indexed.cameraIndex) / grid.columns;
                const cv::Rect cell(grid.gap + column * (cellWidth + grid.gap),
                                    grid.gap + row * (cellHeight + grid.gap),
                                    cellWidth, cellHeight);
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

        for (auto& camera : cameras) camera->stopAndJoin();
        syncThread.stopAndJoin();
        writer.release();
        cv::destroyAllWindows();
        core->WaitIdle();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MultiCameraAnalysisDisplayD3D12 failed: " << e.what() << '\n';
        return 1;
    }
}
