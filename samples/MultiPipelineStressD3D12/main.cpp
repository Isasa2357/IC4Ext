#include "PipelineWorkers.hpp"
#include "StressSupport.hpp"

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <IC4Ext/IC4Ext.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Pipe = IC4Ext::D3D12;
using IC4ExtStress::Clock;
using IC4ExtStress::DisplaySlot;
using IC4ExtStress::FatalState;
using IC4ExtStress::FrameSetQueuePtr;
using IC4ExtStress::PipelineMetrics;
using IC4ExtStress::PipelineSnapshot;
using IC4ExtStress::WorkerOptions;

std::atomic<bool>* SignalStop = nullptr;

void OnSignal(int)
{
    if (SignalStop) SignalStop->store(true);
}

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == name) return argv[index + 1];
    }
    return nullptr;
}

bool HasArg(int argc, char** argv, const char* name)
{
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == name) return true;
    }
    return false;
}

int ArgInt(int argc, char** argv, const char* name, int fallback)
{
    const auto* value = ArgValue(argc, argv, name);
    return value ? std::atoi(value) : fallback;
}

double ArgDouble(int argc, char** argv, const char* name, double fallback)
{
    const auto* value = ArgValue(argc, argv, name);
    return value ? std::atof(value) : fallback;
}

std::uint64_t ArgUint64(
    int argc,
    char** argv,
    const char* name,
    std::uint64_t fallback)
{
    const auto* value = ArgValue(argc, argv, name);
    return value ? std::strtoull(value, nullptr, 10) : fallback;
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& value)
{
    IC4Ext::CameraPixelFormat format{};
    if (!IC4Ext::ParseCameraPixelFormat(value, format)) {
        throw std::runtime_error("unsupported camera format: " + value);
    }
    return format;
}

Pipe::FrameSyncTimestampSource ParseTimestampSource(const std::string& value)
{
    if (value == "host" || value == "HostReceived") {
        return Pipe::FrameSyncTimestampSource::HostReceived;
    }
    if (value == "device" || value == "Device") {
        return Pipe::FrameSyncTimestampSource::Device;
    }
    if (value == "auto" || value == "Auto") {
        return Pipe::FrameSyncTimestampSource::Auto;
    }
    throw std::runtime_error("--timestamp-source must be auto, host, or device");
}

const char* TimestampSourceName(Pipe::FrameSyncTimestampSource source)
{
    switch (source) {
    case Pipe::FrameSyncTimestampSource::HostReceived: return "host";
    case Pipe::FrameSyncTimestampSource::Device: return "device";
    case Pipe::FrameSyncTimestampSource::Auto:
    default: return "auto";
    }
}

struct Options
{
    int device0 = 0;
    int device1 = 1;
    int warmupSec = 5;
    int durationSec = 30;
    int reportMs = 1000;
    bool hardwareTrigger = false;
    std::string triggerSource = "Line1";
    Pipe::FrameSyncTimestampSource timestampSource =
        Pipe::FrameSyncTimestampSource::HostReceived;
    std::uint64_t maxDiffUs = 4000;

    int width = 0;
    int height = 0;
    int offsetX = -1;
    int offsetY = -1;
    double cameraFps = 0.0;
    IC4Ext::CameraPixelFormat cameraFormat = IC4Ext::CameraPixelFormat::BGR8;
    bool forceFormat = false;

    std::filesystem::path json0;
    std::filesystem::path json1;
    std::size_t jsonIndex0 = 0;
    std::size_t jsonIndex1 = 0;

    int ingressQueue = 128;
    int latestQueue = 1;
    int allFrameQueue = 32;
    int capturePoolInitial = 16;
    int capturePoolMax = 64;
    int maxPendingBuffers = 64;
    int readTimeoutMs = 1000;
    int readbackTimeoutMs = 5000;

    double recordFps = 160.0;
    std::string videoCodec = "MJPG";
    std::filesystem::path outputDirectory = "stress_output";
    std::filesystem::path csvPath = "stress_output/metrics.csv";

    bool showWindows = true;
    int displayMaximumWidth = 1280;
    int displayMaximumHeight = 720;
};

Options ParseOptions(int argc, char** argv)
{
    Options options;
    options.device0 = ArgInt(argc, argv, "--device0", options.device0);
    options.device1 = ArgInt(argc, argv, "--device1", options.device1);
    options.warmupSec = ArgInt(argc, argv, "--warmup-sec", options.warmupSec);
    options.durationSec = ArgInt(argc, argv, "--duration-sec", options.durationSec);
    options.reportMs = ArgInt(argc, argv, "--report-ms", options.reportMs);
    options.hardwareTrigger = HasArg(argc, argv, "--hardware-trigger");
    if (const auto* value = ArgValue(argc, argv, "--trigger-source")) {
        options.triggerSource = value;
    }
    if (const auto* value = ArgValue(argc, argv, "--timestamp-source")) {
        options.timestampSource = ParseTimestampSource(value);
    }
    options.maxDiffUs = ArgUint64(argc, argv, "--max-diff-us", options.maxDiffUs);

    options.width = ArgInt(argc, argv, "--width", options.width);
    options.height = ArgInt(argc, argv, "--height", options.height);
    options.offsetX = ArgInt(argc, argv, "--offset-x", options.offsetX);
    options.offsetY = ArgInt(argc, argv, "--offset-y", options.offsetY);
    options.cameraFps = ArgDouble(argc, argv, "--fps", options.cameraFps);
    if (const auto* value = ArgValue(argc, argv, "--format")) {
        options.cameraFormat = ParseCameraFormat(value);
        options.forceFormat = true;
    }

    if (const auto* value = ArgValue(argc, argv, "--ic4-json0")) options.json0 = value;
    if (const auto* value = ArgValue(argc, argv, "--ic4-json1")) options.json1 = value;
    options.jsonIndex0 = static_cast<std::size_t>(
        std::max(0, ArgInt(argc, argv, "--json-device-index0", 0)));
    options.jsonIndex1 = static_cast<std::size_t>(
        std::max(0, ArgInt(argc, argv, "--json-device-index1", 0)));

    options.ingressQueue = ArgInt(argc, argv, "--ingress-queue", options.ingressQueue);
    options.latestQueue = ArgInt(argc, argv, "--latest-queue", options.latestQueue);
    options.allFrameQueue = ArgInt(argc, argv, "--all-frame-queue", options.allFrameQueue);
    options.capturePoolInitial = ArgInt(
        argc, argv, "--capture-pool-initial", options.capturePoolInitial);
    options.capturePoolMax = ArgInt(
        argc, argv, "--capture-pool-max", options.capturePoolMax);
    options.maxPendingBuffers = ArgInt(
        argc, argv, "--max-pending-buffers", options.maxPendingBuffers);
    options.readTimeoutMs = ArgInt(argc, argv, "--read-timeout-ms", options.readTimeoutMs);
    options.readbackTimeoutMs = ArgInt(
        argc, argv, "--readback-timeout-ms", options.readbackTimeoutMs);

    options.recordFps = ArgDouble(argc, argv, "--record-fps", options.recordFps);
    if (const auto* value = ArgValue(argc, argv, "--video-codec")) {
        options.videoCodec = value;
    }
    if (const auto* value = ArgValue(argc, argv, "--output-dir")) {
        options.outputDirectory = value;
    }
    if (const auto* value = ArgValue(argc, argv, "--csv")) options.csvPath = value;

    options.showWindows = !HasArg(argc, argv, "--no-display-windows");
    options.displayMaximumWidth = ArgInt(
        argc, argv, "--display-max-width", options.displayMaximumWidth);
    options.displayMaximumHeight = ArgInt(
        argc, argv, "--display-max-height", options.displayMaximumHeight);

    if (options.device0 == options.device1) {
        throw std::runtime_error("--device0 and --device1 must be different");
    }
    if (options.warmupSec < 0 || options.durationSec <= 0 || options.reportMs <= 0) {
        throw std::runtime_error("invalid duration/report options");
    }
    if (options.maxDiffUs == 0 || options.ingressQueue <= 0 ||
        options.latestQueue <= 0 || options.allFrameQueue <= 0) {
        throw std::runtime_error("queue sizes and timestamp tolerance must be positive");
    }
    if (options.capturePoolInitial <= 0 ||
        options.capturePoolMax < options.capturePoolInitial) {
        throw std::runtime_error("invalid capture pool capacity");
    }
    if (options.maxPendingBuffers <= 0 || options.readTimeoutMs <= 0 ||
        options.readbackTimeoutMs <= 0 || options.recordFps <= 0.0) {
        throw std::runtime_error("invalid capture/readback/recording options");
    }
    if (options.videoCodec.size() != 4) {
        throw std::runtime_error("--video-codec must contain exactly four characters");
    }
    return options;
}

IC4Ext::CameraCaptureConfig MakeCameraConfig(
    const Options& options,
    const std::filesystem::path& json,
    std::size_t jsonIndex)
{
    IC4Ext::CameraCaptureConfig config;
    if (!json.empty()) {
        config.ic4StateJson.path = json;
        config.ic4StateJson.deviceIndex = jsonIndex;
        config.ic4StateJson.strict = false;
        config.ic4StateJson.applyNestedSelectorStates = true;
    }

    config.streamRequest.width = options.width;
    config.streamRequest.height = options.height;
    config.streamRequest.fps = options.cameraFps;
    config.streamRequest.requestedFormat = options.cameraFormat;
    config.streamRequest.forceRequestedFormat = options.forceFormat;
    if (options.offsetX >= 0) config.streamRequest.offsetX = options.offsetX;
    if (options.offsetY >= 0) config.streamRequest.offsetY = options.offsetY;

    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.outputSpec.createSrv = true;
    config.outputSpec.createUav = true;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config.maxPendingBuffers = static_cast<std::size_t>(options.maxPendingBuffers);
    config.shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / "d3d12";
    config.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;

    if (options.hardwareTrigger) {
        IC4Ext::ConfigureHardwareTriggerSync(config, options.triggerSource);
    }
    return config;
}

FrameSetQueuePtr MakeOutputQueue(std::size_t capacity, bool latest)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = capacity;
    options.overflowPolicy = latest
        ? ThreadKit::Queues::QueueOverflowPolicy::DropOldest
        : ThreadKit::Queues::QueueOverflowPolicy::RejectNew;
    return std::make_shared<Pipe::ReadOnlyFrameSetQueue>(options);
}

struct PipelineReport
{
    std::string name;
    bool latest = false;
    std::vector<Pipe::CameraId> cameras;
    int priority = 0;
    FrameSetQueuePtr queue;
    Pipe::FrameSyncOutputId outputId = Pipe::InvalidFrameSyncOutputId;
    PipelineMetrics* metrics = nullptr;
    Pipe::FrameSyncOutputStats outputBaseline{};
};

Pipe::FrameSyncOutputId RegisterPipeline(
    Pipe::FrameSyncThread& syncThread,
    const PipelineReport& pipeline)
{
    Pipe::FrameSyncOutputConfig config;
    config.requiredCameras = pipeline.cameras;
    config.frameRate = Pipe::FrameRateLimit::Maximum();
    config.priority = pipeline.priority;
    config.enabled = true;
    return syncThread.registerOutput(pipeline.queue, std::move(config));
}

std::uint64_t Delta(std::uint64_t finalValue, std::uint64_t baseline) noexcept
{
    return finalValue >= baseline ? finalValue - baseline : 0;
}

Pipe::FrameSyncOutputStats Difference(
    const Pipe::FrameSyncOutputStats& finalValue,
    const Pipe::FrameSyncOutputStats& baseline)
{
    Pipe::FrameSyncOutputStats result;
    result.consideredSets = Delta(finalValue.consideredSets, baseline.consideredSets);
    result.skippedByFrameRate = Delta(
        finalValue.skippedByFrameRate, baseline.skippedByFrameRate);
    result.emittedSets = Delta(finalValue.emittedSets, baseline.emittedSets);
    result.queueDrops = Delta(finalValue.queueDrops, baseline.queueDrops);
    result.disabledSkips = Delta(finalValue.disabledSkips, baseline.disabledSkips);
    return result;
}

void PumpDisplay(
    bool enabled,
    const std::array<std::string, 4>& names,
    std::array<DisplaySlot*, 4> slots,
    std::array<std::uint64_t, 4>& sequences,
    std::atomic<bool>& stopRequested)
{
    if (!enabled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    for (std::size_t index = 0; index < slots.size(); ++index) {
        cv::Mat image;
        if (slots[index]->snapshot(image, sequences[index])) {
            cv::imshow(names[index], image);
        }
    }
    const int key = cv::waitKey(1);
    if (key == 27 || key == 'q' || key == 'Q') stopRequested.store(true);
}

void PrintProgress(
    const std::vector<PipelineReport>& pipelines,
    double elapsedSec)
{
    std::cout << std::fixed << std::setprecision(2)
              << "[progress] elapsed=" << elapsedSec << "s";
    for (const auto& pipeline : pipelines) {
        const auto snapshot = pipeline.metrics->snapshot();
        const double fps = elapsedSec > 0.0
            ? static_cast<double>(snapshot.processed) / elapsedSec
            : 0.0;
        const auto queueStats = pipeline.queue->stats();
        std::cout << " | " << pipeline.name
                  << "{recv=" << snapshot.received
                  << ",proc=" << snapshot.processed
                  << ",fps=" << fps
                  << ",q=" << queueStats.currentSize
                  << ",qmax=" << queueStats.maxObservedSize
                  << ",drop=" << (queueStats.droppedOldest + queueStats.rejectedNew)
                  << ",fail=" << snapshot.failures << "}";
    }
    std::cout << std::endl;
}

bool EvaluatePipeline(
    const PipelineReport& pipeline,
    const Pipe::FrameSyncOutputStats& outputDelta,
    std::string& reason)
{
    const auto snapshot = pipeline.metrics->snapshot();
    const auto queueStats = pipeline.queue->stats();

    if (snapshot.processed == 0) {
        reason = "processed zero frames";
        return false;
    }
    if (snapshot.failures != 0) {
        reason = "worker failures=" + std::to_string(snapshot.failures);
        return false;
    }
    if (pipeline.latest) return true;

    if (outputDelta.queueDrops != 0 || queueStats.rejectedNew != 0 ||
        queueStats.droppedOldest != 0) {
        reason = "all-frame queue dropped frames";
        return false;
    }
    if (snapshot.received != snapshot.processed) {
        reason = "received/processed mismatch";
        return false;
    }
    if (snapshot.received != outputDelta.emittedSets) {
        reason = "dispatch/worker count mismatch";
        return false;
    }
    return true;
}

void WriteCsv(
    const std::filesystem::path& path,
    const std::vector<PipelineReport>& pipelines,
    const std::vector<Pipe::FrameSyncOutputStats>& outputDeltas,
    double measurementSec,
    double drainSec)
{
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream stream(path);
    if (!stream) throw std::runtime_error("failed to open CSV: " + path.string());

    stream << "pipeline,mode,cameras,priority,received,processed,fps,failures,"
              "readback_failures,output_failures,average_ms,maximum_ms,checksum,"
              "queue_pushed,queue_popped,queue_dropped_oldest,queue_rejected_new,"
              "queue_dropped_by_pop_latest,queue_max_observed,dispatch_emitted,"
              "dispatch_drops,measurement_sec,drain_sec,pass,reason\n";

    for (std::size_t index = 0; index < pipelines.size(); ++index) {
        const auto& pipeline = pipelines[index];
        const auto snapshot = pipeline.metrics->snapshot();
        const auto queueStats = pipeline.queue->stats();
        std::string reason;
        const bool passed = EvaluatePipeline(
            pipeline, outputDeltas[index], reason);
        std::string cameras;
        for (std::size_t cameraIndex = 0;
             cameraIndex < pipeline.cameras.size();
             ++cameraIndex) {
            if (cameraIndex != 0) cameras += "+";
            cameras += std::to_string(pipeline.cameras[cameraIndex]);
        }

        stream << pipeline.name << ','
               << (pipeline.latest ? "latest" : "all") << ','
               << cameras << ','
               << pipeline.priority << ','
               << snapshot.received << ','
               << snapshot.processed << ','
               << (measurementSec > 0.0
                       ? static_cast<double>(snapshot.processed) / measurementSec
                       : 0.0) << ','
               << snapshot.failures << ','
               << snapshot.readbackFailures << ','
               << snapshot.outputFailures << ','
               << snapshot.averageProcessMs << ','
               << snapshot.maximumProcessMs << ','
               << snapshot.checksum << ','
               << queueStats.pushed << ','
               << queueStats.popped << ','
               << queueStats.droppedOldest << ','
               << queueStats.rejectedNew << ','
               << queueStats.droppedByPopLatest << ','
               << queueStats.maxObservedSize << ','
               << outputDeltas[index].emittedSets << ','
               << outputDeltas[index].queueDrops << ','
               << measurementSec << ','
               << drainSec << ','
               << (passed ? 1 : 0) << ','
               << '"' << reason << '"' << '\n';
    }
}

void CloseQueues(const std::vector<PipelineReport>& pipelines)
{
    for (const auto& pipeline : pipelines) pipeline.queue->close();
}

void JoinWorkers(std::vector<std::thread>& workers)
{
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::atomic<bool> stopRequested{false};
    SignalStop = &stopRequested;
    std::signal(SIGINT, OnSignal);

    try {
        const Options options = ParseOptions(argc, argv);
        std::error_code directoryError;
        std::filesystem::create_directories(options.outputDirectory, directoryError);
        if (directoryError) {
            throw std::runtime_error(
                "failed to create output directory: " + directoryError.message());
        }

        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        auto backend = IC4Ext::D3D12BackendContext::FromCore(
            core,
            IC4Ext::D3D12BackendQueueKind::Direct);
        if (!backend.resolve()) throw std::runtime_error("failed to resolve D3D12 backend");

        ThreadKit::Queues::QueueOptions ingressOptions;
        ingressOptions.maxSize = static_cast<std::size_t>(options.ingressQueue);
        ingressOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
        auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(ingressOptions);

        Pipe::FrameSyncConfig syncConfig;
        syncConfig.cameraIds = {0, 1};
        syncConfig.timestampSource = options.timestampSource;
        syncConfig.maxTimestampDiffNs = options.maxDiffUs * 1000ull;
        syncConfig.maxBufferedFramesPerCamera = 32;
        syncConfig.groupTimeout = std::chrono::milliseconds(100);
        Pipe::FrameSyncThread syncThread(ingress, syncConfig);

        std::array<PipelineMetrics, 10> metrics;
        const auto latestCapacity = static_cast<std::size_t>(options.latestQueue);
        const auto allCapacity = static_cast<std::size_t>(options.allFrameQueue);

        std::vector<PipelineReport> pipelines;
        pipelines.reserve(10);
        pipelines.push_back({"pair_display", true, {0, 1}, 300,
                             MakeOutputQueue(latestCapacity, true), 0, &metrics[0]});
        pipelines.push_back({"id0_display", true, {0}, 290,
                             MakeOutputQueue(latestCapacity, true), 0, &metrics[1]});
        pipelines.push_back({"id1_display", true, {1}, 280,
                             MakeOutputQueue(latestCapacity, true), 0, &metrics[2]});
        pipelines.push_back({"pair_video", false, {0, 1}, 1000,
                             MakeOutputQueue(allCapacity, false), 0, &metrics[3]});
        pipelines.push_back({"id0_video", false, {0}, 990,
                             MakeOutputQueue(allCapacity, false), 0, &metrics[4]});
        pipelines.push_back({"id1_video", false, {1}, 980,
                             MakeOutputQueue(allCapacity, false), 0, &metrics[5]});
        pipelines.push_back({"hlsl_sobel", false, {0, 1}, 900,
                             MakeOutputQueue(allCapacity, false), 0, &metrics[6]});
        pipelines.push_back({"opencv_canny_id0", false, {0}, 800,
                             MakeOutputQueue(allCapacity, false), 0, &metrics[7]});
        pipelines.push_back({"opencv_sobel_id1", false, {1}, 790,
                             MakeOutputQueue(allCapacity, false), 0, &metrics[8]});
        pipelines.push_back({"opencv_pair_display", true, {0, 1}, 270,
                             MakeOutputQueue(latestCapacity, true), 0, &metrics[9]});

        for (auto& pipeline : pipelines) {
            pipeline.outputId = RegisterPipeline(syncThread, pipeline);
            if (pipeline.outputId == Pipe::InvalidFrameSyncOutputId) {
                const auto error = syncThread.lastError();
                throw std::runtime_error(
                    "registerOutput failed for " + pipeline.name + ": " +
                    error.where + ": " + error.message);
            }
        }

        IC4Ext::IC4DeviceSelector selector0;
        selector0.deviceIndex = options.device0;
        IC4Ext::IC4DeviceSelector selector1;
        selector1.deviceIndex = options.device1;

        Pipe::CameraCaptureOptions captureOptions;
        captureOptions.initialFramePoolCapacity =
            static_cast<std::size_t>(options.capturePoolInitial);
        captureOptions.maxFramePoolCapacity =
            static_cast<std::size_t>(options.capturePoolMax);
        captureOptions.framePoolExhaustionPolicy =
            Pipe::FramePoolExhaustionPolicy::DropNewest;

        Pipe::CameraCaptureThreadOptions cameraThreadOptions;
        cameraThreadOptions.readTimeoutMs =
            static_cast<std::uint32_t>(options.readTimeoutMs);
        cameraThreadOptions.stopOnReadError = false;

        Pipe::CameraCaptureThread camera0(
            0,
            selector0,
            MakeCameraConfig(options, options.json0, options.jsonIndex0),
            backend,
            captureOptions,
            cameraThreadOptions);
        Pipe::CameraCaptureThread camera1(
            1,
            selector1,
            MakeCameraConfig(options, options.json1, options.jsonIndex1),
            backend,
            captureOptions,
            cameraThreadOptions);
        camera0.setOutputQueue(ingress);
        camera1.setOutputQueue(ingress);

        FatalState fatal;
        std::atomic<bool> measuring{false};
        WorkerOptions workerOptions;
        workerOptions.readbackTimeoutMs =
            static_cast<std::uint32_t>(options.readbackTimeoutMs);
        workerOptions.recordFps = options.recordFps;
        workerOptions.videoFourcc = cv::VideoWriter::fourcc(
            options.videoCodec[0],
            options.videoCodec[1],
            options.videoCodec[2],
            options.videoCodec[3]);
        workerOptions.displayMaximumWidth = options.displayMaximumWidth;
        workerOptions.displayMaximumHeight = options.displayMaximumHeight;
        workerOptions.outputDirectory = options.outputDirectory;

        DisplaySlot pairDisplay;
        DisplaySlot id0Display;
        DisplaySlot id1Display;
        DisplaySlot opencvPairDisplay;

        std::vector<std::thread> workers;
        workers.reserve(10);
        workers.push_back(IC4ExtStress::StartPairDisplayWorker(
            pipelines[0].queue, backend, pairDisplay, metrics[0], fatal,
            stopRequested, measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartSingleDisplayWorker(
            "Pipeline 2: latest id0 display", 0,
            pipelines[1].queue, backend, id0Display, metrics[1], fatal,
            stopRequested, measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartSingleDisplayWorker(
            "Pipeline 3: latest id1 display", 1,
            pipelines[2].queue, backend, id1Display, metrics[2], fatal,
            stopRequested, measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartPairVideoWorker(
            pipelines[3].queue, backend, metrics[3], fatal,
            measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartSingleVideoWorker(
            "id0-video", 0, "id0.avi", pipelines[4].queue, backend,
            metrics[4], fatal, measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartSingleVideoWorker(
            "id1-video", 1, "id1.avi", pipelines[5].queue, backend,
            metrics[5], fatal, measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartSobelWorker(
            pipelines[6].queue, backend, metrics[6], fatal, measuring));
        workers.push_back(IC4ExtStress::StartOpenCvCannyWorker(
            pipelines[7].queue, backend, metrics[7], fatal,
            measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartOpenCvSobelWorker(
            pipelines[8].queue, backend, metrics[8], fatal,
            measuring, workerOptions));
        workers.push_back(IC4ExtStress::StartOpenCvPairDisplayWorker(
            pipelines[9].queue, backend, opencvPairDisplay, metrics[9], fatal,
            stopRequested, measuring, workerOptions));

        const std::array<std::string, 4> windowNames = {
            "1 Pair latest",
            "2 Camera 0 latest",
            "3 Camera 1 latest",
            "10 OpenCV pair latest"};
        if (options.showWindows) {
            for (const auto& name : windowNames) cv::namedWindow(name, cv::WINDOW_NORMAL);
        }
        std::array<DisplaySlot*, 4> displaySlots = {
            &pairDisplay, &id0Display, &id1Display, &opencvPairDisplay};
        std::array<std::uint64_t, 4> displaySequences{};

        if (!syncThread.start()) {
            const auto error = syncThread.lastError();
            throw std::runtime_error(
                "FrameSyncThread start failed: " + error.where + ": " + error.message);
        }
        if (!camera0.start()) {
            const auto error = camera0.lastError();
            syncThread.stopAndJoin();
            throw std::runtime_error(
                "camera0 start failed: " + error.where + ": " + error.message);
        }
        if (!camera1.start()) {
            const auto error = camera1.lastError();
            camera0.stopAndJoin();
            syncThread.stopAndJoin();
            throw std::runtime_error(
                "camera1 start failed: " + error.where + ": " + error.message);
        }

        std::cout << "MultiPipelineStressD3D12 started"
                  << " device0=" << options.device0
                  << " device1=" << options.device1
                  << " warmupSec=" << options.warmupSec
                  << " durationSec=" << options.durationSec
                  << " timestampSource=" << TimestampSourceName(options.timestampSource)
                  << " maxDiffUs=" << options.maxDiffUs
                  << " latestQueue=" << options.latestQueue
                  << " allFrameQueue=" << options.allFrameQueue
                  << " recordFps=" << options.recordFps
                  << " codec=" << options.videoCodec
                  << std::endl;

        const auto warmupDeadline = Clock::now() +
            std::chrono::seconds(options.warmupSec);
        while (!stopRequested.load() && !fatal.triggered() &&
               Clock::now() < warmupDeadline) {
            PumpDisplay(
                options.showWindows,
                windowNames,
                displaySlots,
                displaySequences,
                stopRequested);
        }

        for (auto& pipeline : pipelines) {
            pipeline.queue->clear();
            pipeline.queue->resetStats();
            pipeline.metrics->reset();
            const auto outputStats = syncThread.outputStats(pipeline.outputId);
            pipeline.outputBaseline = outputStats.value_or(Pipe::FrameSyncOutputStats{});
        }
        const auto syncBaseline = syncThread.stats();
        const auto camera0Baseline = camera0.stats();
        const auto camera1Baseline = camera1.stats();
        const auto pool0Baseline = camera0.framePoolStats();
        const auto pool1Baseline = camera1.framePoolStats();

        measuring.store(true);
        const auto measurementStart = Clock::now();
        const auto measurementDeadline = measurementStart +
            std::chrono::seconds(options.durationSec);
        auto nextReport = measurementStart +
            std::chrono::milliseconds(options.reportMs);

        while (!stopRequested.load() && !fatal.triggered() &&
               Clock::now() < measurementDeadline) {
            PumpDisplay(
                options.showWindows,
                windowNames,
                displaySlots,
                displaySequences,
                stopRequested);
            const auto now = Clock::now();
            if (now >= nextReport) {
                PrintProgress(
                    pipelines,
                    std::chrono::duration<double>(now - measurementStart).count());
                nextReport = now + std::chrono::milliseconds(options.reportMs);
            }
        }
        const auto captureStopTime = Clock::now();
        stopRequested.store(true);

        camera0.stopAndJoin();
        camera1.stopAndJoin();
        camera0.stopAcquisition();
        camera1.stopAcquisition();
        syncThread.stopAndJoin();
        ingress->close();
        CloseQueues(pipelines);

        const auto drainStart = Clock::now();
        JoinWorkers(workers);
        const auto drainEnd = Clock::now();
        measuring.store(false);

        if (options.showWindows) cv::destroyAllWindows();

        const double measurementSec = std::chrono::duration<double>(
            captureStopTime - measurementStart).count();
        const double drainSec = std::chrono::duration<double>(
            drainEnd - drainStart).count();

        std::vector<Pipe::FrameSyncOutputStats> outputDeltas;
        outputDeltas.reserve(pipelines.size());
        bool passed = !fatal.triggered();

        std::cout << std::fixed << std::setprecision(3)
                  << "\n=== MultiPipelineStressD3D12 final ===\n"
                  << "measurementSec=" << measurementSec
                  << " drainSec=" << drainSec << '\n';

        for (const auto& pipeline : pipelines) {
            const auto finalOutput = syncThread.outputStats(pipeline.outputId)
                .value_or(Pipe::FrameSyncOutputStats{});
            const auto outputDelta = Difference(finalOutput, pipeline.outputBaseline);
            outputDeltas.push_back(outputDelta);

            const auto snapshot = pipeline.metrics->snapshot();
            const auto queueStats = pipeline.queue->stats();
            std::string reason;
            const bool pipelinePassed = EvaluatePipeline(
                pipeline, outputDelta, reason);
            passed = passed && pipelinePassed;

            std::cout << pipeline.name
                      << " mode=" << (pipeline.latest ? "latest" : "all")
                      << " emitted=" << outputDelta.emittedSets
                      << " dispatchDrop=" << outputDelta.queueDrops
                      << " received=" << snapshot.received
                      << " processed=" << snapshot.processed
                      << " fps=" << (measurementSec > 0.0
                              ? static_cast<double>(snapshot.processed) / measurementSec
                              : 0.0)
                      << " avgMs=" << snapshot.averageProcessMs
                      << " maxMs=" << snapshot.maximumProcessMs
                      << " qMax=" << queueStats.maxObservedSize
                      << " dropOldest=" << queueStats.droppedOldest
                      << " rejectNew=" << queueStats.rejectedNew
                      << " popLatestDrop=" << queueStats.droppedByPopLatest
                      << " failures=" << snapshot.failures
                      << " result=" << (pipelinePassed ? "PASS" : "FAIL");
            if (!reason.empty()) std::cout << " reason=" << reason;
            std::cout << '\n';
        }

        const auto syncFinal = syncThread.stats();
        const auto camera0Final = camera0.stats();
        const auto camera1Final = camera1.stats();
        const auto pool0Final = camera0.framePoolStats();
        const auto pool1Final = camera1.framePoolStats();

        const auto completedSets = Delta(
            syncFinal.completedSets, syncBaseline.completedSets);
        const auto syncDrops = Delta(syncFinal.droppedFrames, syncBaseline.droppedFrames);
        const auto camera0Errors = Delta(
            camera0Final.readErrors, camera0Baseline.readErrors);
        const auto camera1Errors = Delta(
            camera1Final.readErrors, camera1Baseline.readErrors);
        const auto pool0Drops = Delta(
            pool0Final.exhaustionDrops, pool0Baseline.exhaustionDrops);
        const auto pool1Drops = Delta(
            pool1Final.exhaustionDrops, pool1Baseline.exhaustionDrops);

        std::cout << "sync completedSets=" << completedSets
                  << " droppedFrames=" << syncDrops
                  << " camera0Read=" << Delta(camera0Final.readFrames, camera0Baseline.readFrames)
                  << " camera1Read=" << Delta(camera1Final.readFrames, camera1Baseline.readFrames)
                  << " camera0Errors=" << camera0Errors
                  << " camera1Errors=" << camera1Errors
                  << " pool0Exhaustion=" << pool0Drops
                  << " pool1Exhaustion=" << pool1Drops
                  << '\n';

        if (completedSets == 0 || camera0Errors != 0 || camera1Errors != 0 ||
            pool0Drops != 0 || pool1Drops != 0) {
            passed = false;
        }
        if (fatal.triggered()) {
            std::cout << "fatal=" << fatal.message() << '\n';
        }

        WriteCsv(
            options.csvPath,
            pipelines,
            outputDeltas,
            measurementSec,
            drainSec);
        std::cout << "csv=" << options.csvPath << '\n'
                  << "videoPair=" << (options.outputDirectory / "pair.avi") << '\n'
                  << "video0=" << (options.outputDirectory / "id0.avi") << '\n'
                  << "video1=" << (options.outputDirectory / "id1.avi") << '\n'
                  << "overall=" << (passed ? "PASS" : "FAIL") << std::endl;

        SignalStop = nullptr;
        return passed ? 0 : 2;
    } catch (const std::exception& exception) {
        SignalStop = nullptr;
        std::cerr << "MultiPipelineStressD3D12 failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}
