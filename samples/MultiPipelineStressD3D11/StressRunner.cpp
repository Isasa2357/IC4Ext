#include "PipelineWorkers.hpp"
#include "StressSupport.hpp"

#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>
#include <IC4Ext/IC4Ext.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Core/D3D11CoreConfig.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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

namespace Pipe = IC4Ext::D3D11;
using IC4ExtStressD3D11::Clock;
using IC4ExtStressD3D11::DisplaySlot;
using IC4ExtStressD3D11::FatalState;
using IC4ExtStressD3D11::FrameSetQueuePtr;
using IC4ExtStressD3D11::PipelineMetrics;
using IC4ExtStressD3D11::WorkerOptions;

std::atomic<bool>* g_stop = nullptr;

void HandleSignal(int)
{
    if (g_stop) g_stop->store(true);
}

const char* Arg(int argc, char** argv, const char* name)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == name) return argv[index + 1];
    }
    return nullptr;
}

bool Flag(int argc, char** argv, const char* name)
{
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == name) return true;
    }
    return false;
}

int IntArg(int argc, char** argv, const char* name, int fallback)
{
    const char* value = Arg(argc, argv, name);
    return value ? std::atoi(value) : fallback;
}

double DoubleArg(int argc, char** argv, const char* name, double fallback)
{
    const char* value = Arg(argc, argv, name);
    return value ? std::atof(value) : fallback;
}

std::uint64_t U64Arg(
    int argc,
    char** argv,
    const char* name,
    std::uint64_t fallback)
{
    const char* value = Arg(argc, argv, name);
    return value ? std::strtoull(value, nullptr, 10) : fallback;
}

struct Options
{
    bool synthetic = false;
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
    double minSyncFps = 1.0;
    std::uint64_t minSyncSets = 0;

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

    int syntheticWidth = 1536;
    int syntheticHeight = 1536;
    double syntheticFps = 160.0;
    std::int64_t syntheticOffsetUs = 500;
    std::uint64_t syntheticSeed0 = 0x0123456789abcdefull;
    std::uint64_t syntheticSeed1 = 0xfedcba9876543210ull;
    Pipe::SyntheticFramePattern syntheticPattern =
        Pipe::SyntheticFramePattern::HashNoise;

    int ingressQueue = 128;
    int latestQueue = 1;
    int allFrameQueue = 32;
    int poolInitial = 16;
    int poolMaximum = 64;
    int pendingBuffers = 64;
    int readTimeoutMs = 1000;
    int readbackTimeoutMs = 5000;

    double recordingFps = 160.0;
    std::string codec = "MJPG";
    std::filesystem::path outputDirectory = "stress_output_d3d11";
    std::filesystem::path csvPath = "stress_output_d3d11/metrics.csv";
    bool showWindows = true;
    int displayWidth = 1280;
    int displayHeight = 720;
};

Pipe::FrameSyncTimestampSource ParseTimestampSource(const std::string& text)
{
    if (text == "host") return Pipe::FrameSyncTimestampSource::HostReceived;
    if (text == "device") return Pipe::FrameSyncTimestampSource::Device;
    if (text == "auto") return Pipe::FrameSyncTimestampSource::Auto;
    throw std::runtime_error(
        "--timestamp-source must be host, device, or auto");
}

const char* TimestampSourceName(
    Pipe::FrameSyncTimestampSource source) noexcept
{
    switch (source) {
    case Pipe::FrameSyncTimestampSource::HostReceived: return "host";
    case Pipe::FrameSyncTimestampSource::Device: return "device";
    case Pipe::FrameSyncTimestampSource::Auto: return "auto";
    default: return "unknown";
    }
}

Pipe::SyntheticFramePattern ParseSyntheticPattern(const std::string& text)
{
    if (text == "hash" || text == "noise") {
        return Pipe::SyntheticFramePattern::HashNoise;
    }
    if (text == "gradient") {
        return Pipe::SyntheticFramePattern::Gradient;
    }
    if (text == "checker" || text == "checkerboard") {
        return Pipe::SyntheticFramePattern::Checkerboard;
    }
    if (text == "bars" || text == "counter") {
        return Pipe::SyntheticFramePattern::FrameCounterBars;
    }
    throw std::runtime_error(
        "--synthetic-pattern must be hash, gradient, checker, or bars");
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    options.synthetic = Flag(argc, argv, "--synthetic");
    options.device0 = IntArg(argc, argv, "--device0", options.device0);
    options.device1 = IntArg(argc, argv, "--device1", options.device1);
    options.warmupSec = IntArg(argc, argv, "--warmup-sec", options.warmupSec);
    options.durationSec = IntArg(
        argc,
        argv,
        "--duration-sec",
        options.durationSec);
    options.reportMs = IntArg(argc, argv, "--report-ms", options.reportMs);
    options.hardwareTrigger = Flag(argc, argv, "--hardware-trigger");
    if (const char* value = Arg(argc, argv, "--trigger-source")) {
        options.triggerSource = value;
    }
    if (const char* value = Arg(argc, argv, "--timestamp-source")) {
        options.timestampSource = ParseTimestampSource(value);
    }
    options.maxDiffUs = U64Arg(
        argc,
        argv,
        "--max-diff-us",
        options.maxDiffUs);
    options.minSyncFps = DoubleArg(
        argc,
        argv,
        "--min-sync-fps",
        options.minSyncFps);
    options.minSyncSets = U64Arg(
        argc,
        argv,
        "--min-sync-sets",
        options.minSyncSets);

    options.width = IntArg(argc, argv, "--width", options.width);
    options.height = IntArg(argc, argv, "--height", options.height);
    options.offsetX = IntArg(argc, argv, "--offset-x", options.offsetX);
    options.offsetY = IntArg(argc, argv, "--offset-y", options.offsetY);
    options.cameraFps = DoubleArg(argc, argv, "--fps", options.cameraFps);
    if (const char* value = Arg(argc, argv, "--format")) {
        if (!IC4Ext::ParseCameraPixelFormat(value, options.cameraFormat)) {
            throw std::runtime_error("unsupported --format value");
        }
        options.forceFormat = true;
    }
    if (const char* value = Arg(argc, argv, "--ic4-json0")) {
        options.json0 = value;
    }
    if (const char* value = Arg(argc, argv, "--ic4-json1")) {
        options.json1 = value;
    }
    options.jsonIndex0 = static_cast<std::size_t>(
        std::max(0, IntArg(argc, argv, "--json-device-index0", 0)));
    options.jsonIndex1 = static_cast<std::size_t>(
        std::max(0, IntArg(argc, argv, "--json-device-index1", 0)));

    options.syntheticWidth = IntArg(
        argc,
        argv,
        "--synthetic-width",
        options.syntheticWidth);
    options.syntheticHeight = IntArg(
        argc,
        argv,
        "--synthetic-height",
        options.syntheticHeight);
    options.syntheticFps = DoubleArg(
        argc,
        argv,
        "--synthetic-fps",
        options.syntheticFps);
    options.syntheticOffsetUs = static_cast<std::int64_t>(
        IntArg(
            argc,
            argv,
            "--synthetic-offset-us",
            static_cast<int>(options.syntheticOffsetUs)));
    options.syntheticSeed0 = U64Arg(
        argc,
        argv,
        "--synthetic-seed0",
        options.syntheticSeed0);
    options.syntheticSeed1 = U64Arg(
        argc,
        argv,
        "--synthetic-seed1",
        options.syntheticSeed1);
    if (const char* value = Arg(argc, argv, "--synthetic-pattern")) {
        options.syntheticPattern = ParseSyntheticPattern(value);
    }

    options.ingressQueue = IntArg(
        argc,
        argv,
        "--ingress-queue",
        options.ingressQueue);
    options.latestQueue = IntArg(
        argc,
        argv,
        "--latest-queue",
        options.latestQueue);
    options.allFrameQueue = IntArg(
        argc,
        argv,
        "--all-frame-queue",
        options.allFrameQueue);
    options.poolInitial = IntArg(
        argc,
        argv,
        "--capture-pool-initial",
        options.poolInitial);
    options.poolMaximum = IntArg(
        argc,
        argv,
        "--capture-pool-max",
        options.poolMaximum);
    options.pendingBuffers = IntArg(
        argc,
        argv,
        "--max-pending-buffers",
        options.pendingBuffers);
    options.readTimeoutMs = IntArg(
        argc,
        argv,
        "--read-timeout-ms",
        options.readTimeoutMs);
    options.readbackTimeoutMs = IntArg(
        argc,
        argv,
        "--readback-timeout-ms",
        options.readbackTimeoutMs);

    options.recordingFps = DoubleArg(
        argc,
        argv,
        "--record-fps",
        options.recordingFps);
    if (const char* value = Arg(argc, argv, "--video-codec")) {
        options.codec = value;
    }
    if (const char* value = Arg(argc, argv, "--output-dir")) {
        options.outputDirectory = value;
    }
    if (const char* value = Arg(argc, argv, "--csv")) {
        options.csvPath = value;
    }
    options.showWindows = !Flag(argc, argv, "--no-display-windows");
    options.displayWidth = IntArg(
        argc,
        argv,
        "--display-max-width",
        options.displayWidth);
    options.displayHeight = IntArg(
        argc,
        argv,
        "--display-max-height",
        options.displayHeight);

    if ((!options.synthetic && options.device0 == options.device1) ||
        options.warmupSec < 0 || options.durationSec <= 0 ||
        options.reportMs <= 0 || options.maxDiffUs == 0 ||
        options.minSyncFps < 0.0 || options.ingressQueue <= 0 ||
        options.latestQueue <= 0 || options.allFrameQueue <= 0 ||
        options.poolInitial <= 0 || options.poolMaximum < options.poolInitial ||
        options.pendingBuffers <= 0 || options.readTimeoutMs <= 0 ||
        options.readbackTimeoutMs <= 0 || options.recordingFps <= 0.0 ||
        options.codec.size() != 4 || options.syntheticWidth <= 0 ||
        options.syntheticHeight <= 0 || options.syntheticFps <= 0.0 ||
        options.syntheticSeed0 == options.syntheticSeed1) {
        throw std::runtime_error("invalid stress-test command-line options");
    }
    return options;
}

IC4Ext::CameraCaptureConfig MakeCameraConfig(
    const Options& options,
    const std::filesystem::path& jsonPath,
    std::size_t jsonIndex)
{
    IC4Ext::CameraCaptureConfig config;
    if (!jsonPath.empty()) {
        config.ic4StateJson.path = jsonPath;
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
    config.maxPendingBuffers = static_cast<std::size_t>(options.pendingBuffers);
    config.shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / "d3d11";
    if (options.hardwareTrigger) {
        IC4Ext::ConfigureHardwareTriggerSync(config, options.triggerSource);
    }
    return config;
}

FrameSetQueuePtr MakeQueue(std::size_t capacity, bool latest)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = capacity;
    options.overflowPolicy = latest
        ? ThreadKit::Queues::QueueOverflowPolicy::DropOldest
        : ThreadKit::Queues::QueueOverflowPolicy::RejectNew;
    return std::make_shared<Pipe::ReadOnlyFrameSetQueue>(options);
}

struct Pipeline
{
    std::string name;
    bool latest = false;
    std::vector<Pipe::CameraId> cameras;
    int priority = 0;
    FrameSetQueuePtr queue;
    Pipe::FrameSyncOutputId outputId = Pipe::InvalidFrameSyncOutputId;
    PipelineMetrics* metrics = nullptr;
    Pipe::FrameSyncOutputStats baseline{};
};

std::uint64_t Delta(
    std::uint64_t finalValue,
    std::uint64_t baseline) noexcept
{
    return finalValue >= baseline ? finalValue - baseline : 0;
}

Pipe::FrameSyncOutputStats Difference(
    const Pipe::FrameSyncOutputStats& finalValue,
    const Pipe::FrameSyncOutputStats& baseline)
{
    Pipe::FrameSyncOutputStats result;
    result.consideredSets = Delta(
        finalValue.consideredSets,
        baseline.consideredSets);
    result.skippedByFrameRate = Delta(
        finalValue.skippedByFrameRate,
        baseline.skippedByFrameRate);
    result.emittedSets = Delta(
        finalValue.emittedSets,
        baseline.emittedSets);
    result.queueDrops = Delta(
        finalValue.queueDrops,
        baseline.queueDrops);
    result.disabledSkips = Delta(
        finalValue.disabledSkips,
        baseline.disabledSkips);
    return result;
}

class WorkerGroup final
{
public:
    WorkerGroup(std::vector<Pipeline>& pipelines, std::atomic<bool>& stop)
        : pipelines_(pipelines), stop_(stop)
    {
    }

    ~WorkerGroup() { closeAndJoin(); }

    std::vector<std::thread>& threads() noexcept { return threads_; }

    void closeAndJoin() noexcept
    {
        if (joined_) return;
        stop_.store(true);
        for (const auto& pipeline : pipelines_) pipeline.queue->close();
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
        joined_ = true;
    }

private:
    std::vector<Pipeline>& pipelines_;
    std::atomic<bool>& stop_;
    std::vector<std::thread> threads_;
    bool joined_ = false;
};

void PumpWindows(
    bool enabled,
    const std::array<std::string, 4>& names,
    const std::array<DisplaySlot*, 4>& slots,
    std::array<std::uint64_t, 4>& sequences,
    std::atomic<bool>& stop)
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
    if (key == 27 || key == 'q' || key == 'Q') stop.store(true);
}

bool PipelinePassed(
    const Pipeline& pipeline,
    const Pipe::FrameSyncOutputStats& dispatch,
    std::string& reason)
{
    const auto metrics = pipeline.metrics->snapshot();
    const auto queue = pipeline.queue->stats();
    if (metrics.processed == 0) {
        reason = "processed zero frames";
        return false;
    }
    if (metrics.failures != 0) {
        reason = "worker failure";
        return false;
    }
    if (pipeline.latest) return true;
    if (dispatch.queueDrops != 0 || queue.rejectedNew != 0 ||
        queue.droppedOldest != 0) {
        reason = "all-frame queue dropped data";
        return false;
    }
    if (metrics.received != metrics.processed) {
        reason = "received/processed mismatch";
        return false;
    }
    return true;
}

void PrintPipelineProgress(
    const std::vector<Pipeline>& pipelines,
    double seconds)
{
    for (const auto& pipeline : pipelines) {
        const auto metrics = pipeline.metrics->snapshot();
        const auto queue = pipeline.queue->stats();
        std::cout << " | " << pipeline.name
                  << "{processed=" << metrics.processed
                  << ",fps=" << (seconds > 0.0
                          ? metrics.processed / seconds
                          : 0.0)
                  << ",queue=" << queue.currentSize
                  << ",queueMax=" << queue.maxObservedSize
                  << ",drop=" << queue.droppedOldest + queue.rejectedNew
                  << ",failure=" << metrics.failures << '}';
    }
}

void PrintProgress(
    const std::vector<Pipeline>& pipelines,
    const Pipe::FrameSyncThread& sync,
    const Pipe::CameraCaptureThread& camera0,
    const Pipe::CameraCaptureThread& camera1,
    const Pipe::FrameSyncStats& syncBaseline,
    const Pipe::CameraCaptureThreadStats& camera0Baseline,
    const Pipe::CameraCaptureThreadStats& camera1Baseline,
    const Pipe::FramePoolStats& pool0Baseline,
    const Pipe::FramePoolStats& pool1Baseline,
    const Pipe::FramePoolStats& pool0,
    const Pipe::FramePoolStats& pool1,
    double seconds)
{
    const auto syncStats = sync.stats();
    const auto camera0Stats = camera0.stats();
    const auto camera1Stats = camera1.stats();

    const auto completed = Delta(
        syncStats.completedSets,
        syncBaseline.completedSets);
    const auto dropped = Delta(
        syncStats.droppedFrames,
        syncBaseline.droppedFrames);
    const auto input = Delta(syncStats.inputFrames, syncBaseline.inputFrames);
    const auto cam0Read = Delta(
        camera0Stats.readFrames,
        camera0Baseline.readFrames);
    const auto cam1Read = Delta(
        camera1Stats.readFrames,
        camera1Baseline.readFrames);
    const auto cam0Push = Delta(
        camera0Stats.pushedFrames,
        camera0Baseline.pushedFrames);
    const auto cam1Push = Delta(
        camera1Stats.pushedFrames,
        camera1Baseline.pushedFrames);

    std::cout << std::fixed << std::setprecision(1)
              << "[progress] seconds=" << seconds
              << " sync{input=" << input
              << ",sets=" << completed
              << ",fps=" << (seconds > 0.0 ? completed / seconds : 0.0)
              << ",drops=" << dropped
              << ",dropRate=" << (input > 0
                      ? static_cast<double>(dropped) / input
                      : 0.0)
              << "} cam0{read=" << cam0Read
              << ",push=" << cam0Push
              << ",timeout=" << Delta(
                     camera0Stats.readTimeouts,
                     camera0Baseline.readTimeouts)
              << ",err=" << Delta(
                     camera0Stats.readErrors,
                     camera0Baseline.readErrors)
              << ",poolPub=" << pool0.published
              << ",poolDrop=" << Delta(
                     pool0.exhaustionDrops,
                     pool0Baseline.exhaustionDrops)
              << "} cam1{read=" << cam1Read
              << ",push=" << cam1Push
              << ",timeout=" << Delta(
                     camera1Stats.readTimeouts,
                     camera1Baseline.readTimeouts)
              << ",err=" << Delta(
                     camera1Stats.readErrors,
                     camera1Baseline.readErrors)
              << ",poolPub=" << pool1.published
              << ",poolDrop=" << Delta(
                     pool1.exhaustionDrops,
                     pool1Baseline.exhaustionDrops)
              << '}';
    PrintPipelineProgress(pipelines, seconds);
    std::cout << std::endl;
}

void WriteCsv(
    const std::filesystem::path& path,
    const std::vector<Pipeline>& pipelines,
    const std::vector<Pipe::FrameSyncOutputStats>& dispatch,
    double measurementSeconds,
    double drainSeconds)
{
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open CSV: " + path.string());
    }

    file << "pipeline,mode,received,processed,fps,failures,readback_failures,"
            "output_failures,average_ms,maximum_ms,queue_max,drop_oldest,"
            "reject_new,pop_latest_drop,dispatch_emitted,dispatch_drop,"
            "measurement_sec,drain_sec,pass,reason\n";

    for (std::size_t index = 0; index < pipelines.size(); ++index) {
        const auto& pipeline = pipelines[index];
        const auto metrics = pipeline.metrics->snapshot();
        const auto queue = pipeline.queue->stats();
        std::string reason;
        const bool passed = PipelinePassed(
            pipeline,
            dispatch[index],
            reason);
        file << pipeline.name << ','
             << (pipeline.latest ? "latest" : "all") << ','
             << metrics.received << ',' << metrics.processed << ','
             << (measurementSeconds > 0.0
                     ? metrics.processed / measurementSeconds
                     : 0.0) << ','
             << metrics.failures << ',' << metrics.readbackFailures << ','
             << metrics.outputFailures << ',' << metrics.averageProcessMs << ','
             << metrics.maximumProcessMs << ',' << queue.maxObservedSize << ','
             << queue.droppedOldest << ',' << queue.rejectedNew << ','
             << queue.droppedByPopLatest << ','
             << dispatch[index].emittedSets << ','
             << dispatch[index].queueDrops << ','
             << measurementSeconds << ',' << drainSeconds << ','
             << (passed ? 1 : 0) << ",\"" << reason << "\"\n";
    }
}

std::uint64_t RequiredSyncSets(
    const Options& options,
    double measurementSeconds)
{
    const auto fromFps = static_cast<std::uint64_t>(
        std::ceil(
            std::max(0.0, options.minSyncFps) * measurementSeconds));
    return std::max(options.minSyncSets, fromFps);
}

Pipe::FramePoolStats SourcePoolStats(
    const std::shared_ptr<Pipe::SyntheticFrameSource>& source,
    const Pipe::CameraCaptureThread& camera)
{
    return source ? source->framePoolStats() : camera.framePoolStats();
}

} // namespace

int main(int argc, char** argv)
{
    std::atomic<bool> stop{false};
    g_stop = &stop;
    std::signal(SIGINT, HandleSignal);

    try {
        const Options options = ParseOptions(argc, argv);
        std::error_code directoryError;
        std::filesystem::create_directories(
            options.outputDirectory,
            directoryError);
        if (directoryError) {
            throw std::runtime_error(
                "output directory error: " + directoryError.message());
        }

        D3D11CoreLib::D3D11CoreConfig coreConfig;
        coreConfig.enableDebugLayer = false;
        coreConfig.enableInfoQueue = false;
        coreConfig.allowWarpAdapter = true;
        coreConfig.enableMultithreadProtection = true;
        auto core = D3D11CoreLib::D3D11Core::CreateShared(coreConfig);
        auto backend = IC4Ext::D3D11BackendContext::FromCore(core, true);
        if (!backend.resolve()) {
            throw std::runtime_error("D3D11 backend resolution failed");
        }

        ThreadKit::Queues::QueueOptions ingressOptions;
        ingressOptions.maxSize = static_cast<std::size_t>(options.ingressQueue);
        ingressOptions.overflowPolicy =
            ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
        auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(
            ingressOptions);

        Pipe::FrameSyncConfig syncConfig;
        syncConfig.cameraIds = {0, 1};
        syncConfig.timestampSource = options.timestampSource;
        syncConfig.maxTimestampDiffNs = options.maxDiffUs * 1000ull;
        syncConfig.maxBufferedFramesPerCamera = 32;
        syncConfig.groupTimeout = std::chrono::milliseconds(100);
        Pipe::FrameSyncThread sync(ingress, syncConfig);

        std::array<PipelineMetrics, 10> metrics;
        const auto latestCapacity =
            static_cast<std::size_t>(options.latestQueue);
        const auto allCapacity =
            static_cast<std::size_t>(options.allFrameQueue);
        std::vector<Pipeline> pipelines = {
            {"pair_display", true, {0, 1}, 300,
             MakeQueue(latestCapacity, true), 0, &metrics[0]},
            {"id0_display", true, {0}, 290,
             MakeQueue(latestCapacity, true), 0, &metrics[1]},
            {"id1_display", true, {1}, 280,
             MakeQueue(latestCapacity, true), 0, &metrics[2]},
            {"pair_video", false, {0, 1}, 1000,
             MakeQueue(allCapacity, false), 0, &metrics[3]},
            {"id0_video", false, {0}, 990,
             MakeQueue(allCapacity, false), 0, &metrics[4]},
            {"id1_video", false, {1}, 980,
             MakeQueue(allCapacity, false), 0, &metrics[5]},
            {"hlsl_sobel", false, {0, 1}, 900,
             MakeQueue(allCapacity, false), 0, &metrics[6]},
            {"opencv_canny_id0", false, {0}, 800,
             MakeQueue(allCapacity, false), 0, &metrics[7]},
            {"opencv_sobel_id1", false, {1}, 790,
             MakeQueue(allCapacity, false), 0, &metrics[8]},
            {"opencv_pair_display", true, {0, 1}, 270,
             MakeQueue(latestCapacity, true), 0, &metrics[9]}
        };

        for (auto& pipeline : pipelines) {
            Pipe::FrameSyncOutputConfig config;
            config.requiredCameras = pipeline.cameras;
            config.frameRate = Pipe::FrameRateLimit::Maximum();
            config.priority = pipeline.priority;
            pipeline.outputId = sync.registerOutput(pipeline.queue, config);
            if (pipeline.outputId == Pipe::InvalidFrameSyncOutputId) {
                throw std::runtime_error(
                    std::string("output registration failed: ") +
                    pipeline.name);
            }
        }

        Pipe::CameraCaptureOptions captureOptions;
        captureOptions.initialFramePoolCapacity =
            static_cast<std::size_t>(options.poolInitial);
        captureOptions.maxFramePoolCapacity =
            static_cast<std::size_t>(options.poolMaximum);
        captureOptions.framePoolExhaustionPolicy =
            Pipe::FramePoolExhaustionPolicy::DropNewest;

        Pipe::CameraCaptureThreadOptions captureThreadOptions;
        captureThreadOptions.readTimeoutMs =
            static_cast<std::uint32_t>(options.readTimeoutMs);
        captureThreadOptions.stopOnReadError = false;

        std::shared_ptr<Pipe::SyntheticFrameSource> source0;
        std::shared_ptr<Pipe::SyntheticFrameSource> source1;
        std::unique_ptr<Pipe::CameraCaptureThread> camera0;
        std::unique_ptr<Pipe::CameraCaptureThread> camera1;

        if (options.synthetic) {
            Pipe::SyntheticFrameSourceConfig sourceConfig0;
            sourceConfig0.width =
                static_cast<std::uint32_t>(options.syntheticWidth);
            sourceConfig0.height =
                static_cast<std::uint32_t>(options.syntheticHeight);
            sourceConfig0.fps = options.syntheticFps;
            sourceConfig0.pattern = options.syntheticPattern;
            sourceConfig0.seed = options.syntheticSeed0;
            sourceConfig0.deviceTimestampOffsetNs = 0;
            sourceConfig0.initialFramePoolCapacity =
                static_cast<std::size_t>(options.poolInitial);
            sourceConfig0.maxFramePoolCapacity =
                static_cast<std::size_t>(options.poolMaximum);
            sourceConfig0.framePoolExhaustionPolicy =
                Pipe::FramePoolExhaustionPolicy::DropNewest;

            auto sourceConfig1 = sourceConfig0;
            sourceConfig1.seed = options.syntheticSeed1;
            sourceConfig1.deviceTimestampOffsetNs =
                options.syntheticOffsetUs * 1000ll;

            source0 = std::make_shared<Pipe::SyntheticFrameSource>();
            source1 = std::make_shared<Pipe::SyntheticFrameSource>();
            if (!source0->initialize(backend, sourceConfig0)) {
                const auto error = source0->lastError();
                throw std::runtime_error(
                    "synthetic source0 initialization failed: " +
                    error.where + ": " + error.message);
            }
            if (!source1->initialize(backend, sourceConfig1)) {
                const auto error = source1->lastError();
                throw std::runtime_error(
                    "synthetic source1 initialization failed: " +
                    error.where + ": " + error.message);
            }

            camera0 = std::make_unique<Pipe::CameraCaptureThread>(
                0,
                source0,
                captureThreadOptions);
            camera1 = std::make_unique<Pipe::CameraCaptureThread>(
                1,
                source1,
                captureThreadOptions);
        } else {
            IC4Ext::IC4DeviceSelector selector0;
            selector0.deviceIndex = options.device0;
            IC4Ext::IC4DeviceSelector selector1;
            selector1.deviceIndex = options.device1;

            camera0 = std::make_unique<Pipe::CameraCaptureThread>(
                0,
                selector0,
                MakeCameraConfig(options, options.json0, options.jsonIndex0),
                backend,
                captureOptions,
                captureThreadOptions);
            camera1 = std::make_unique<Pipe::CameraCaptureThread>(
                1,
                selector1,
                MakeCameraConfig(options, options.json1, options.jsonIndex1),
                backend,
                captureOptions,
                captureThreadOptions);
        }
        camera0->setOutputQueue(ingress);
        camera1->setOutputQueue(ingress);

        FatalState fatal;
        std::atomic<bool> measuring{false};
        DisplaySlot pairDisplay;
        DisplaySlot id0Display;
        DisplaySlot id1Display;
        DisplaySlot opencvPairDisplay;

        WorkerOptions workerOptions;
        workerOptions.readbackTimeoutMs =
            static_cast<std::uint32_t>(options.readbackTimeoutMs);
        workerOptions.recordFps = options.recordingFps;
        workerOptions.videoFourcc = cv::VideoWriter::fourcc(
            options.codec[0],
            options.codec[1],
            options.codec[2],
            options.codec[3]);
        workerOptions.displayMaximumWidth = options.displayWidth;
        workerOptions.displayMaximumHeight = options.displayHeight;
        workerOptions.outputDirectory = options.outputDirectory;

        WorkerGroup workers(pipelines, stop);
        auto& threads = workers.threads();
        threads.reserve(10);
        threads.push_back(IC4ExtStressD3D11::StartPairDisplayWorker(
            pipelines[0].queue,
            backend,
            pairDisplay,
            metrics[0],
            fatal,
            stop,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartSingleDisplayWorker(
            "Pipeline 2: latest id0",
            0,
            pipelines[1].queue,
            backend,
            id0Display,
            metrics[1],
            fatal,
            stop,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartSingleDisplayWorker(
            "Pipeline 3: latest id1",
            1,
            pipelines[2].queue,
            backend,
            id1Display,
            metrics[2],
            fatal,
            stop,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartPairVideoWorker(
            pipelines[3].queue,
            backend,
            metrics[3],
            fatal,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartSingleVideoWorker(
            "id0-video",
            0,
            "id0.avi",
            pipelines[4].queue,
            backend,
            metrics[4],
            fatal,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartSingleVideoWorker(
            "id1-video",
            1,
            "id1.avi",
            pipelines[5].queue,
            backend,
            metrics[5],
            fatal,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartSobelWorker(
            pipelines[6].queue,
            backend,
            metrics[6],
            fatal,
            measuring));
        threads.push_back(IC4ExtStressD3D11::StartOpenCvCannyWorker(
            pipelines[7].queue,
            backend,
            metrics[7],
            fatal,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartOpenCvSobelWorker(
            pipelines[8].queue,
            backend,
            metrics[8],
            fatal,
            measuring,
            workerOptions));
        threads.push_back(IC4ExtStressD3D11::StartOpenCvPairDisplayWorker(
            pipelines[9].queue,
            backend,
            opencvPairDisplay,
            metrics[9],
            fatal,
            stop,
            measuring,
            workerOptions));

        const std::array<std::string, 4> windowNames = {
            "1 Pair latest D3D11",
            "2 Camera 0 latest D3D11",
            "3 Camera 1 latest D3D11",
            "10 OpenCV pair latest D3D11"};
        const std::array<DisplaySlot*, 4> displaySlots = {
            &pairDisplay,
            &id0Display,
            &id1Display,
            &opencvPairDisplay};
        std::array<std::uint64_t, 4> displaySequences{};
        if (options.showWindows) {
            for (const auto& name : windowNames) {
                cv::namedWindow(name, cv::WINDOW_NORMAL);
            }
        }

        if (!sync.start()) {
            const auto error = sync.lastError();
            throw std::runtime_error(
                "FrameSyncThread start failed: " + error.where + ": " +
                error.message);
        }
        if (!camera0->start()) {
            const auto error = camera0->lastError();
            throw std::runtime_error(
                "camera0 start failed: " + error.where + ": " +
                error.message);
        }
        if (!camera1->start()) {
            const auto error = camera1->lastError();
            camera0->stopAndJoin();
            throw std::runtime_error(
                "camera1 start failed: " + error.where + ": " +
                error.message);
        }

        std::cout << "MultiPipelineStressD3D11 started: 10 pipelines, "
                     "independent staging cache per CPU consumer"
                  << " source=" << (options.synthetic ? "synthetic" : "camera")
                  << " warmup=" << options.warmupSec
                  << "s duration=" << options.durationSec
                  << "s timestampSource="
                  << TimestampSourceName(options.timestampSource)
                  << " maxDiffUs=" << options.maxDiffUs
                  << " minSyncFps=" << options.minSyncFps
                  << " minSyncSets=" << options.minSyncSets
                  << " allQueue=" << options.allFrameQueue
                  << " latestQueue=" << options.latestQueue
                  << " recordingFps=" << options.recordingFps
                  << " codec=" << options.codec;
        if (options.synthetic) {
            std::cout << " syntheticSize=" << options.syntheticWidth << 'x'
                      << options.syntheticHeight
                      << " syntheticFps=" << options.syntheticFps
                      << " syntheticOffsetUs=" << options.syntheticOffsetUs;
        }
        std::cout << std::endl;

        const auto warmupEnd =
            Clock::now() + std::chrono::seconds(options.warmupSec);
        while (!stop.load() && !fatal.triggered() &&
               Clock::now() < warmupEnd) {
            PumpWindows(
                options.showWindows,
                windowNames,
                displaySlots,
                displaySequences,
                stop);
        }

        for (auto& pipeline : pipelines) {
            pipeline.queue->clear();
            pipeline.queue->resetStats();
            pipeline.metrics->reset();
            pipeline.baseline = sync.outputStats(pipeline.outputId)
                .value_or(Pipe::FrameSyncOutputStats{});
        }
        const auto syncBaseline = sync.stats();
        const auto camera0Baseline = camera0->stats();
        const auto camera1Baseline = camera1->stats();
        const auto pool0Baseline = SourcePoolStats(source0, *camera0);
        const auto pool1Baseline = SourcePoolStats(source1, *camera1);

        measuring.store(true);
        const auto measurementStart = Clock::now();
        const auto measurementEnd =
            measurementStart + std::chrono::seconds(options.durationSec);
        auto nextReport =
            measurementStart + std::chrono::milliseconds(options.reportMs);

        while (!stop.load() && !fatal.triggered() &&
               Clock::now() < measurementEnd) {
            PumpWindows(
                options.showWindows,
                windowNames,
                displaySlots,
                displaySequences,
                stop);
            const auto now = Clock::now();
            if (now >= nextReport) {
                PrintProgress(
                    pipelines,
                    sync,
                    *camera0,
                    *camera1,
                    syncBaseline,
                    camera0Baseline,
                    camera1Baseline,
                    pool0Baseline,
                    pool1Baseline,
                    SourcePoolStats(source0, *camera0),
                    SourcePoolStats(source1, *camera1),
                    std::chrono::duration<double>(
                        now - measurementStart).count());
                nextReport =
                    now + std::chrono::milliseconds(options.reportMs);
            }
        }

        const auto captureStopTime = Clock::now();
        stop.store(true);
        camera0->stopAndJoin();
        camera1->stopAndJoin();
        if (!options.synthetic) {
            camera0->stopAcquisition();
            camera1->stopAcquisition();
        }
        sync.stopAndJoin();
        ingress->close();

        const auto drainStart = Clock::now();
        workers.closeAndJoin();
        const auto drainEnd = Clock::now();
        measuring.store(false);
        if (options.showWindows) cv::destroyAllWindows();

        const double measurementSeconds =
            std::chrono::duration<double>(
                captureStopTime - measurementStart).count();
        const double drainSeconds =
            std::chrono::duration<double>(drainEnd - drainStart).count();

        std::vector<Pipe::FrameSyncOutputStats> dispatchDeltas;
        dispatchDeltas.reserve(pipelines.size());
        bool passed = !fatal.triggered();

        std::cout << std::fixed << std::setprecision(3)
                  << "\n=== final measurement=" << measurementSeconds
                  << "s drain=" << drainSeconds << "s ===\n";

        for (const auto& pipeline : pipelines) {
            const auto finalDispatch = sync.outputStats(pipeline.outputId)
                .value_or(Pipe::FrameSyncOutputStats{});
            const auto dispatch = Difference(
                finalDispatch,
                pipeline.baseline);
            dispatchDeltas.push_back(dispatch);

            const auto metricsValue = pipeline.metrics->snapshot();
            const auto queueValue = pipeline.queue->stats();
            std::string reason;
            const bool pipelinePass = PipelinePassed(
                pipeline,
                dispatch,
                reason);
            passed = passed && pipelinePass;

            std::cout << pipeline.name
                      << " mode=" << (pipeline.latest ? "latest" : "all")
                      << " emitted=" << dispatch.emittedSets
                      << " dispatchDrop=" << dispatch.queueDrops
                      << " received=" << metricsValue.received
                      << " processed=" << metricsValue.processed
                      << " fps=" << (measurementSeconds > 0.0
                              ? metricsValue.processed / measurementSeconds
                              : 0.0)
                      << " avgMs=" << metricsValue.averageProcessMs
                      << " maxMs=" << metricsValue.maximumProcessMs
                      << " queueMax=" << queueValue.maxObservedSize
                      << " dropOldest=" << queueValue.droppedOldest
                      << " rejectNew=" << queueValue.rejectedNew
                      << " popLatestDrop=" << queueValue.droppedByPopLatest
                      << " failures=" << metricsValue.failures
                      << " result=" << (pipelinePass ? "PASS" : "FAIL");
            if (!reason.empty()) std::cout << " reason=" << reason;
            std::cout << '\n';
        }

        const auto syncFinal = sync.stats();
        const auto camera0Final = camera0->stats();
        const auto camera1Final = camera1->stats();
        const auto pool0Final = SourcePoolStats(source0, *camera0);
        const auto pool1Final = SourcePoolStats(source1, *camera1);

        const auto inputFrames = Delta(
            syncFinal.inputFrames,
            syncBaseline.inputFrames);
        const auto completedSets = Delta(
            syncFinal.completedSets,
            syncBaseline.completedSets);
        const auto droppedFrames = Delta(
            syncFinal.droppedFrames,
            syncBaseline.droppedFrames);
        const auto requiredSets = RequiredSyncSets(
            options,
            measurementSeconds);
        const double syncFps = measurementSeconds > 0.0
            ? static_cast<double>(completedSets) / measurementSeconds
            : 0.0;
        const auto camera0Errors = Delta(
            camera0Final.readErrors,
            camera0Baseline.readErrors);
        const auto camera1Errors = Delta(
            camera1Final.readErrors,
            camera1Baseline.readErrors);
        const auto pool0Drops = Delta(
            pool0Final.exhaustionDrops,
            pool0Baseline.exhaustionDrops);
        const auto pool1Drops = Delta(
            pool1Final.exhaustionDrops,
            pool1Baseline.exhaustionDrops);
        if (completedSets < requiredSets || camera0Errors != 0 ||
            camera1Errors != 0 || pool0Drops != 0 || pool1Drops != 0) {
            passed = false;
        }

        std::cout << "syncInput=" << inputFrames
                  << " syncSets=" << completedSets
                  << " syncFps=" << syncFps
                  << " minRequiredSets=" << requiredSets
                  << " syncDrops=" << droppedFrames
                  << " syncDropRate=" << (inputFrames > 0
                          ? static_cast<double>(droppedFrames) / inputFrames
                          : 0.0)
                  << " camera0Read=" << Delta(
                         camera0Final.readFrames,
                         camera0Baseline.readFrames)
                  << " camera1Read=" << Delta(
                         camera1Final.readFrames,
                         camera1Baseline.readFrames)
                  << " camera0Pushed=" << Delta(
                         camera0Final.pushedFrames,
                         camera0Baseline.pushedFrames)
                  << " camera1Pushed=" << Delta(
                         camera1Final.pushedFrames,
                         camera1Baseline.pushedFrames)
                  << " camera0Timeouts=" << Delta(
                         camera0Final.readTimeouts,
                         camera0Baseline.readTimeouts)
                  << " camera1Timeouts=" << Delta(
                         camera1Final.readTimeouts,
                         camera1Baseline.readTimeouts)
                  << " camera0Errors=" << camera0Errors
                  << " camera1Errors=" << camera1Errors
                  << " pool0Published=" << pool0Final.published
                  << " pool1Published=" << pool1Final.published
                  << " pool0Exhaustion=" << pool0Drops
                  << " pool1Exhaustion=" << pool1Drops << '\n';

        if (options.synthetic) {
            const auto source0Stats = source0->stats();
            const auto source1Stats = source1->stats();
            std::cout << "synthetic0Generated="
                      << source0Stats.generatedFrames
                      << " synthetic1Generated="
                      << source1Stats.generatedFrames
                      << " synthetic0Late=" << source0Stats.lateFrames
                      << " synthetic1Late=" << source1Stats.lateFrames
                      << '\n';
        }

        if (completedSets < requiredSets) {
            std::cout << "syncGate=FAIL completedSets=" << completedSets
                      << " requiredSets=" << requiredSets
                      << " hint=try a different --timestamp-source or larger "
                         "--max-diff-us"
                      << '\n';
        }
        if (fatal.triggered()) {
            std::cout << "fatal=" << fatal.message() << '\n';
        }

        WriteCsv(
            options.csvPath,
            pipelines,
            dispatchDeltas,
            measurementSeconds,
            drainSeconds);
        std::cout << "csv=" << options.csvPath << '\n'
                  << "pairVideo="
                  << (options.outputDirectory / "pair.avi") << '\n'
                  << "id0Video="
                  << (options.outputDirectory / "id0.avi") << '\n'
                  << "id1Video="
                  << (options.outputDirectory / "id1.avi") << '\n'
                  << "overall=" << (passed ? "PASS" : "FAIL")
                  << std::endl;

        g_stop = nullptr;
        return passed ? 0 : 2;
    } catch (const std::exception& exception) {
        g_stop = nullptr;
        std::cerr << "MultiPipelineStressD3D11 failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}
