#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

int ArgInt(int argc, char** argv, const char* name, int fallback)
{
    const char* value = ArgValue(argc, argv, name);
    return value ? std::atoi(value) : fallback;
}

double ArgDouble(int argc, char** argv, const char* name, double fallback)
{
    const char* value = ArgValue(argc, argv, name);
    return value ? std::atof(value) : fallback;
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& text)
{
    IC4Ext::CameraPixelFormat format{};
    if (IC4Ext::ParseCameraPixelFormat(text, format)) return format;
    return IC4Ext::CameraPixelFormat::BGR8;
}

IC4Ext::GpuFrameFormat ParseOutputFormat(const std::string& text)
{
    if (text == "R8") return IC4Ext::GpuFrameFormat::R8;
    return IC4Ext::GpuFrameFormat::RGBA8;
}

IC4Ext::D3D12::FrameSyncPolicy ParseSyncPolicy(const std::string& text)
{
    if (text == "timestamp" || text == "TimestampNearest") {
        return IC4Ext::D3D12::FrameSyncPolicy::TimestampNearest;
    }
    return IC4Ext::D3D12::FrameSyncPolicy::FrameNumberExact;
}

IC4Ext::D3D12::FrameSyncTimestampSource ParseTimestampSource(const std::string& text)
{
    if (text == "host" || text == "HostReceived") {
        return IC4Ext::D3D12::FrameSyncTimestampSource::HostReceived;
    }
    if (text == "device" || text == "Device") {
        return IC4Ext::D3D12::FrameSyncTimestampSource::Device;
    }
    return IC4Ext::D3D12::FrameSyncTimestampSource::Auto;
}

bool CreateD3D12Backend(IC4Ext::D3D12BackendContext& outBackend)
{
    try {
        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        outBackend = IC4Ext::D3D12BackendContext::FromCore(
            core,
            IC4Ext::D3D12BackendQueueKind::Direct);
        return outBackend.resolve();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Helper core creation failed: " << exception.what() << std::endl;
        return false;
    }
}

std::uint64_t AbsDiff(std::uint64_t a, std::uint64_t b) noexcept
{
    return a >= b ? a - b : b - a;
}

IC4Ext::CameraCaptureConfig MakeCameraConfig(
    int argc,
    char** argv,
    const char* jsonArg,
    const char* jsonDeviceIndexArg)
{
    IC4Ext::CameraCaptureConfig config;

    const int width = ArgInt(argc, argv, "--width", 0);
    const int height = ArgInt(argc, argv, "--height", 0);
    const int offsetX = ArgInt(argc, argv, "--offset-x", -1);
    const int offsetY = ArgInt(argc, argv, "--offset-y", -1);
    const double fps = ArgDouble(argc, argv, "--fps", 0.0);
    const char* jsonPath = ArgValue(argc, argv, jsonArg);
    const int jsonDeviceIndex = ArgInt(argc, argv, jsonDeviceIndexArg, 0);

    if (jsonPath) {
        config.ic4StateJson.path = jsonPath;
        config.ic4StateJson.deviceIndex = static_cast<std::size_t>(std::max(0, jsonDeviceIndex));
        config.ic4StateJson.strict = false;
    }

    config.streamRequest.width = width;
    config.streamRequest.height = height;
    config.streamRequest.fps = fps;
    config.streamRequest.requestedFormat = ParseCameraFormat(
        ArgValue(argc, argv, "--format") ? ArgValue(argc, argv, "--format") : "BGR8");
    config.streamRequest.forceRequestedFormat = HasArg(argc, argv, "--force-format");
    if (offsetX >= 0) config.streamRequest.offsetX = offsetX;
    if (offsetY >= 0) config.streamRequest.offsetY = offsetY;

    config.outputSpec.outputFormat = ParseOutputFormat(
        ArgValue(argc, argv, "--output") ? ArgValue(argc, argv, "--output") : "RGBA8");
    config.shaderConfig.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d12";
    config.queuePolicy = IC4Ext::FrameQueuePolicy::LatestOnly;
    config.maxPendingBuffers = 1;

    if (HasArg(argc, argv, "--hardware-trigger")) {
        const std::string triggerSource = ArgValue(argc, argv, "--trigger-source")
            ? ArgValue(argc, argv, "--trigger-source")
            : "Line1";
        IC4Ext::ConfigureHardwareTriggerSync(config, triggerSource);
    }

    return config;
}

struct DrainStats
{
    std::uint64_t pairSets = 0;
    std::uint64_t camera0Sets = 0;
    std::uint64_t camera1Sets = 0;
    std::uint64_t diffSamples = 0;
    std::uint64_t sumDeviceDiffNs = 0;
    std::uint64_t maxDeviceDiffNs = 0;
    IC4Ext::D3D12::SyncGroupId lastSyncGroupId = 0;
    std::uint64_t lastFrame0 = 0;
    std::uint64_t lastFrame1 = 0;
};

void DrainPairQueue(
    const std::shared_ptr<IC4Ext::D3D12::ReadOnlyFrameSetQueue>& queue,
    DrainStats& stats)
{
    for (;;) {
        auto item = queue->tryPop();
        if (!item) break;
        ++stats.pairSets;
        stats.lastSyncGroupId = item->syncGroupId();

        const auto* frame0 = item->find(0);
        const auto* frame1 = item->find(1);
        if (frame0) stats.lastFrame0 = frame0->timing().frameNumber;
        if (frame1) stats.lastFrame1 = frame1->timing().frameNumber;
        if (frame0 && frame1 &&
            frame0->timing().deviceTimestampNs != 0 &&
            frame1->timing().deviceTimestampNs != 0) {
            const auto diff = AbsDiff(
                frame0->timing().deviceTimestampNs,
                frame1->timing().deviceTimestampNs);
            ++stats.diffSamples;
            stats.sumDeviceDiffNs += diff;
            stats.maxDeviceDiffNs = std::max(stats.maxDeviceDiffNs, diff);
        }
    }
}

void DrainSingleQueue(
    const std::shared_ptr<IC4Ext::D3D12::ReadOnlyFrameSetQueue>& queue,
    std::uint64_t& counter)
{
    for (;;) {
        auto item = queue->tryPop();
        if (!item) break;
        ++counter;
    }
}

void PrintStats(
    const char* prefix,
    const DrainStats& stats,
    double elapsedSec)
{
    const double pairFps = elapsedSec > 0.0
        ? static_cast<double>(stats.pairSets) / elapsedSec
        : 0.0;
    const double cam0Fps = elapsedSec > 0.0
        ? static_cast<double>(stats.camera0Sets) / elapsedSec
        : 0.0;
    const double cam1Fps = elapsedSec > 0.0
        ? static_cast<double>(stats.camera1Sets) / elapsedSec
        : 0.0;
    const double meanDiffUs = stats.diffSamples > 0
        ? static_cast<double>(stats.sumDeviceDiffNs) / static_cast<double>(stats.diffSamples) / 1000.0
        : 0.0;
    const double maxDiffUs = static_cast<double>(stats.maxDeviceDiffNs) / 1000.0;

    std::cout << prefix
              << " pairSets=" << stats.pairSets
              << " pairFps=" << pairFps
              << " cam0Sets=" << stats.camera0Sets
              << " cam0Fps=" << cam0Fps
              << " cam1Sets=" << stats.camera1Sets
              << " cam1Fps=" << cam1Fps
              << " lastGroup=" << stats.lastSyncGroupId
              << " lastFrame0=" << stats.lastFrame0
              << " lastFrame1=" << stats.lastFrame1
              << " meanDeviceDiffUs=" << meanDiffUs
              << " maxDeviceDiffUs=" << maxDiffUs
              << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    namespace Pipe = IC4Ext::D3D12;
    using Clock = std::chrono::steady_clock;

    IC4Ext::D3D12BackendContext backend;
    if (!CreateD3D12Backend(backend)) {
        std::cerr << "Failed to create D3D12 backend context" << std::endl;
        return 1;
    }

    const int device0 = ArgInt(argc, argv, "--device0", 0);
    const int device1 = ArgInt(argc, argv, "--device1", 1);
    const int durationSec = ArgInt(argc, argv, "--duration-sec", 10);
    const int reportMs = ArgInt(argc, argv, "--report-ms", 1000);
    const int readTimeoutMs = ArgInt(argc, argv, "--read-timeout-ms", 1000);
    const int inputQueueSize = ArgInt(argc, argv, "--input-queue", 128);
    const int outputQueueSize = ArgInt(argc, argv, "--output-queue", 16);
    const int poolInitial = ArgInt(argc, argv, "--pool-initial", 8);
    const int poolMax = ArgInt(argc, argv, "--pool-max", 32);
    const double singleOutputFps = ArgDouble(argc, argv, "--single-output-fps", 30.0);
    const auto syncPolicy = ParseSyncPolicy(
        ArgValue(argc, argv, "--sync-policy") ? ArgValue(argc, argv, "--sync-policy") : "frame-number");
    const auto timestampSource = ParseTimestampSource(
        ArgValue(argc, argv, "--timestamp-source") ? ArgValue(argc, argv, "--timestamp-source") : "auto");
    const auto maxDiffUs = static_cast<std::uint64_t>(std::max(1, ArgInt(argc, argv, "--max-diff-us", 1000)));

    IC4Ext::IC4DeviceSelector selector0;
    selector0.deviceIndex = device0;
    IC4Ext::IC4DeviceSelector selector1;
    selector1.deviceIndex = device1;

    auto config0 = MakeCameraConfig(argc, argv, "--ic4-json0", "--json-device-index0");
    auto config1 = MakeCameraConfig(argc, argv, "--ic4-json1", "--json-device-index1");

    Pipe::CameraCaptureOptions captureOptions;
    captureOptions.initialFramePoolCapacity = static_cast<std::size_t>(std::max(1, poolInitial));
    captureOptions.maxFramePoolCapacity = static_cast<std::size_t>(std::max(poolInitial, poolMax));

    Pipe::CameraCaptureThreadOptions threadOptions;
    threadOptions.readTimeoutMs = static_cast<std::uint32_t>(std::max(1, readTimeoutMs));
    threadOptions.stopOnReadError = false;

    ThreadKit::Queues::QueueOptions ingressOptions;
    ingressOptions.maxSize = static_cast<std::size_t>(std::max(1, inputQueueSize));
    ingressOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(ingressOptions);

    Pipe::FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.policy = syncPolicy;
    syncConfig.timestampSource = timestampSource;
    syncConfig.maxTimestampDiffNs = maxDiffUs * 1000ull;
    syncConfig.maxBufferedFramesPerCamera = 16;
    syncConfig.groupTimeout = std::chrono::milliseconds(100);

    Pipe::FrameSyncThread syncThread(ingress, syncConfig);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = static_cast<std::size_t>(std::max(1, outputQueueSize));
    outputOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto pairQueue = std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);
    auto camera0Queue = std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);
    auto camera1Queue = std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);

    Pipe::FrameSyncOutputConfig pairOutput;
    pairOutput.requiredCameras = {0, 1};
    pairOutput.frameRate = Pipe::FrameRateLimit::Maximum();
    pairOutput.priority = 100;
    const auto pairOutputId = syncThread.registerOutput(pairQueue, pairOutput);

    Pipe::FrameSyncOutputConfig camera0Output;
    camera0Output.requiredCameras = {0};
    camera0Output.frameRate = Pipe::FrameRateLimit::Fixed(singleOutputFps);
    camera0Output.priority = 50;
    const auto camera0OutputId = syncThread.registerOutput(camera0Queue, camera0Output);

    Pipe::FrameSyncOutputConfig camera1Output = camera0Output;
    camera1Output.requiredCameras = {1};
    const auto camera1OutputId = syncThread.registerOutput(camera1Queue, camera1Output);

    if (pairOutputId == Pipe::InvalidFrameSyncOutputId ||
        camera0OutputId == Pipe::InvalidFrameSyncOutputId ||
        camera1OutputId == Pipe::InvalidFrameSyncOutputId) {
        const auto error = syncThread.lastError();
        std::cerr << "registerOutput failed: " << error.where << ": " << error.message << std::endl;
        return 1;
    }

    Pipe::CameraCaptureThread camera0(0, selector0, config0, backend, captureOptions, threadOptions);
    Pipe::CameraCaptureThread camera1(1, selector1, config1, backend, captureOptions, threadOptions);
    camera0.setOutputQueue(ingress);
    camera1.setOutputQueue(ingress);

    if (!syncThread.start()) {
        const auto error = syncThread.lastError();
        std::cerr << "sync start failed: " << error.where << ": " << error.message << std::endl;
        return 1;
    }
    if (!camera0.start()) {
        const auto error = camera0.lastError();
        std::cerr << "camera0 start failed: " << error.where << ": " << error.message << std::endl;
        syncThread.stopAndJoin();
        return 1;
    }
    if (!camera1.start()) {
        const auto error = camera1.lastError();
        std::cerr << "camera1 start failed: " << error.where << ": " << error.message << std::endl;
        camera0.stopAndJoin();
        syncThread.stopAndJoin();
        return 1;
    }

    std::cout << "running MultiCameraReadOnlySyncD3D12 for " << durationSec
              << " sec. device0=" << device0
              << " device1=" << device1
              << " syncPolicy=" << (syncPolicy == Pipe::FrameSyncPolicy::TimestampNearest ? "timestamp" : "frame-number")
              << std::endl;

    DrainStats stats;
    const auto start = Clock::now();
    auto nextReport = start + std::chrono::milliseconds(reportMs);
    const auto stopTime = start + std::chrono::seconds(std::max(1, durationSec));

    while (Clock::now() < stopTime) {
        DrainPairQueue(pairQueue, stats);
        DrainSingleQueue(camera0Queue, stats.camera0Sets);
        DrainSingleQueue(camera1Queue, stats.camera1Sets);

        const auto now = Clock::now();
        if (now >= nextReport) {
            const double elapsed = std::chrono::duration<double>(now - start).count();
            PrintStats("[progress]", stats, elapsed);
            nextReport = now + std::chrono::milliseconds(reportMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    DrainPairQueue(pairQueue, stats);
    DrainSingleQueue(camera0Queue, stats.camera0Sets);
    DrainSingleQueue(camera1Queue, stats.camera1Sets);

    camera0.stopAndJoin();
    camera1.stopAndJoin();
    syncThread.stopAndJoin();

    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    PrintStats("[final]", stats, elapsed);

    const auto syncStats = syncThread.stats();
    const auto camera0ThreadStats = camera0.stats();
    const auto camera1ThreadStats = camera1.stats();
    const auto pool0 = camera0.framePoolStats();
    const auto pool1 = camera1.framePoolStats();

    std::cout << "sync inputFrames=" << syncStats.inputFrames
              << " completedSets=" << syncStats.completedSets
              << " droppedFrames=" << syncStats.droppedFrames
              << " incompleteSets=" << syncStats.incompleteSets
              << " totalOutputSets=" << syncStats.totalOutputSets
              << " totalOutputQueueDrops=" << syncStats.totalOutputQueueDrops
              << std::endl;

    std::cout << "camera0 readFrames=" << camera0ThreadStats.readFrames
              << " pushedFrames=" << camera0ThreadStats.pushedFrames
              << " readTimeouts=" << camera0ThreadStats.readTimeouts
              << " readErrors=" << camera0ThreadStats.readErrors
              << " poolCapacity=" << pool0.capacity
              << " poolPublished=" << pool0.published
              << std::endl;

    std::cout << "camera1 readFrames=" << camera1ThreadStats.readFrames
              << " pushedFrames=" << camera1ThreadStats.pushedFrames
              << " readTimeouts=" << camera1ThreadStats.readTimeouts
              << " readErrors=" << camera1ThreadStats.readErrors
              << " poolCapacity=" << pool1.capacity
              << " poolPublished=" << pool1.published
              << std::endl;

    return 0;
}
