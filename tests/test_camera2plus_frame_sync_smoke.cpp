#include "TestCameraUtils.hpp"

#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kBuildMarker = "camera2plus-command-lifecycle-v6-latency";

std::uint64_t HostTimestampNs(const IC4Ext::D3D12IndexedCameraFrame& frame)
{
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        frame.frame.timing.hostReceivedTime.time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t AbsDiff(std::uint64_t lhs, std::uint64_t rhs)
{
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

bool DurationNs(std::chrono::steady_clock::time_point later,
                std::chrono::steady_clock::time_point earlier,
                std::uint64_t& output) noexcept
{
    const auto value =
        std::chrono::duration_cast<std::chrono::nanoseconds>(later - earlier).count();
    if (value < 0) return false;
    output = static_cast<std::uint64_t>(value);
    return true;
}

struct DistributionSummary
{
    std::size_t count = 0;
    double meanNs = 0.0;
    std::uint64_t minimumNs = 0;
    std::uint64_t p50Ns = 0;
    std::uint64_t p95Ns = 0;
    std::uint64_t p99Ns = 0;
    std::uint64_t maximumNs = 0;
};

std::uint64_t PercentileNearestRank(const std::vector<std::uint64_t>& sorted,
                                    double fraction)
{
    if (sorted.empty()) return 0;
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    if (clamped <= 0.0) return sorted.front();

    const auto rank = static_cast<std::size_t>(
        std::ceil(clamped * static_cast<double>(sorted.size())));
    const auto index = std::min(sorted.size() - 1, rank > 0 ? rank - 1 : 0);
    return sorted[index];
}

DistributionSummary Summarize(const std::vector<std::uint64_t>& values)
{
    DistributionSummary summary;
    if (values.empty()) return summary;

    std::vector<std::uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());

    long double total = 0.0L;
    for (const auto value : sorted) total += static_cast<long double>(value);

    summary.count = sorted.size();
    summary.meanNs = static_cast<double>(total / static_cast<long double>(sorted.size()));
    summary.minimumNs = sorted.front();
    summary.p50Ns = PercentileNearestRank(sorted, 0.50);
    summary.p95Ns = PercentileNearestRank(sorted, 0.95);
    summary.p99Ns = PercentileNearestRank(sorted, 0.99);
    summary.maximumNs = sorted.back();
    return summary;
}

void PrintDistribution(const char* name, const std::vector<std::uint64_t>& values)
{
    const auto summary = Summarize(values);
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
           << "[latency] " << name
           << " count=" << summary.count
           << " meanUs=" << summary.meanNs / 1'000.0
           << " minUs=" << static_cast<double>(summary.minimumNs) / 1'000.0
           << " p50Us=" << static_cast<double>(summary.p50Ns) / 1'000.0
           << " p95Us=" << static_cast<double>(summary.p95Ns) / 1'000.0
           << " p99Us=" << static_cast<double>(summary.p99Ns) / 1'000.0
           << " maxUs=" << static_cast<double>(summary.maximumNs) / 1'000.0;
    std::cout << stream.str() << std::endl;
}

struct LatencySample
{
    std::uint64_t hostPairDiffNs = 0;
    std::uint64_t camera0HostToEmitNs = 0;
    std::uint64_t camera1HostToEmitNs = 0;
    std::uint64_t oldestHostToEmitNs = 0;
    std::uint64_t latestHostToEmitNs = 0;
    std::uint64_t emitToPopNs = 0;
};

struct LatencyCollector
{
    std::vector<std::uint64_t> hostPairDiffNs;
    std::vector<std::uint64_t> camera0HostToEmitNs;
    std::vector<std::uint64_t> camera1HostToEmitNs;
    std::vector<std::uint64_t> oldestHostToEmitNs;
    std::vector<std::uint64_t> latestHostToEmitNs;
    std::vector<std::uint64_t> emitToPopNs;

    void reserve(std::size_t count)
    {
        hostPairDiffNs.reserve(count);
        camera0HostToEmitNs.reserve(count);
        camera1HostToEmitNs.reserve(count);
        oldestHostToEmitNs.reserve(count);
        latestHostToEmitNs.reserve(count);
        emitToPopNs.reserve(count);
    }

    void add(const LatencySample& sample)
    {
        hostPairDiffNs.push_back(sample.hostPairDiffNs);
        camera0HostToEmitNs.push_back(sample.camera0HostToEmitNs);
        camera1HostToEmitNs.push_back(sample.camera1HostToEmitNs);
        oldestHostToEmitNs.push_back(sample.oldestHostToEmitNs);
        latestHostToEmitNs.push_back(sample.latestHostToEmitNs);
        emitToPopNs.push_back(sample.emitToPopNs);
    }

    void print() const
    {
        PrintDistribution("host_pair_diff", hostPairDiffNs);
        PrintDistribution("camera0_host_received_to_sync_emit", camera0HostToEmitNs);
        PrintDistribution("camera1_host_received_to_sync_emit", camera1HostToEmitNs);
        PrintDistribution("oldest_host_received_to_sync_emit", oldestHostToEmitNs);
        PrintDistribution("latest_host_received_to_sync_emit", latestHostToEmitNs);
        PrintDistribution("sync_emit_to_test_pop", emitToPopNs);
    }
};

bool BuildLatencySample(const IC4Ext::D3D12SyncedFrameSet& set,
                        const IC4Ext::D3D12IndexedCameraFrame& frame0,
                        const IC4Ext::D3D12IndexedCameraFrame& frame1,
                        std::chrono::steady_clock::time_point popTime,
                        LatencySample& sample,
                        std::string& failure)
{
    const auto host0 = frame0.frame.timing.hostReceivedTime;
    const auto host1 = frame1.frame.timing.hostReceivedTime;
    const auto emitted = set.emittedTime;

    if (host0 == std::chrono::steady_clock::time_point{} ||
        host1 == std::chrono::steady_clock::time_point{}) {
        failure = "Synced set has no host receive timestamp";
        return false;
    }
    if (emitted == std::chrono::steady_clock::time_point{}) {
        failure = "Synced set has no emitted timestamp";
        return false;
    }

    if (!DurationNs(emitted, host0, sample.camera0HostToEmitNs) ||
        !DurationNs(emitted, host1, sample.camera1HostToEmitNs)) {
        failure = "Sync emitted timestamp precedes host receive timestamp";
        return false;
    }
    if (!DurationNs(popTime, emitted, sample.emitToPopNs)) {
        failure = "Test pop timestamp precedes sync emitted timestamp";
        return false;
    }

    const std::uint64_t timestamp0 = HostTimestampNs(frame0);
    const std::uint64_t timestamp1 = HostTimestampNs(frame1);
    if (timestamp0 == 0 || timestamp1 == 0) {
        failure = "Synced set has no host receive timestamp";
        return false;
    }

    sample.hostPairDiffNs = AbsDiff(timestamp0, timestamp1);
    sample.oldestHostToEmitNs =
        std::max(sample.camera0HostToEmitNs, sample.camera1HostToEmitNs);
    sample.latestHostToEmitNs =
        std::min(sample.camera0HostToEmitNs, sample.camera1HostToEmitNs);
    return true;
}

void WriteLatencyCsvHeader(std::ofstream& stream)
{
    stream << "sampleIndex,syncGroupId,camera0FrameNumber,camera1FrameNumber,"
              "hostPairDiffNs,camera0HostToEmitNs,camera1HostToEmitNs,"
              "oldestHostToEmitNs,latestHostToEmitNs,emitToPopNs\n";
}

void WriteLatencyCsvRow(std::ofstream& stream,
                        int sampleIndex,
                        const IC4Ext::D3D12SyncedFrameSet& set,
                        const IC4Ext::D3D12IndexedCameraFrame& frame0,
                        const IC4Ext::D3D12IndexedCameraFrame& frame1,
                        const LatencySample& sample)
{
    stream << sampleIndex << ','
           << set.syncGroupId << ','
           << frame0.frame.timing.frameNumber << ','
           << frame1.frame.timing.frameNumber << ','
           << sample.hostPairDiffNs << ','
           << sample.camera0HostToEmitNs << ','
           << sample.camera1HostToEmitNs << ','
           << sample.oldestHostToEmitNs << ','
           << sample.latestHostToEmitNs << ','
           << sample.emitToPopNs << '\n';
}

void PrintStage(const char* stage)
{
    std::cerr << "[camera2plus] " << stage << std::endl;
}

void PrintStats(const IC4Ext::D3D12CameraCaptureThread& camera0,
                const IC4Ext::D3D12CameraCaptureThread& camera1,
                const IC4Ext::D3D12FrameSyncThread& sync,
                int measuredSets,
                int warmupSets)
{
    const auto stats0 = camera0.stats();
    const auto stats1 = camera1.stats();
    const auto syncStats = sync.stats();
    std::cerr << "[camera2plus] measuredSets=" << measuredSets
              << " warmupSets=" << warmupSets
              << " syncInput=" << syncStats.inputFrames
              << " syncEmitted=" << syncStats.emittedSets
              << " syncDropped=" << syncStats.droppedFrames
              << " syncIgnored=" << syncStats.ignoredFrames
              << " camera0{read=" << stats0.readFrames
              << ",pushed=" << stats0.pushedFrames
              << ",timeouts=" << stats0.readTimeouts
              << ",errors=" << stats0.readErrors << "}"
              << " camera1{read=" << stats1.readFrames
              << ",pushed=" << stats1.pushedFrames
              << ",timeouts=" << stats1.readTimeouts
              << ",errors=" << stats1.readErrors << "}"
              << std::endl;
}

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    std::cerr << "[camera2plus] build=" << kBuildMarker << std::endl;
    PrintStage("enumerating cameras");
    if (!IC4ExtTest::RequireCameraCount(2)) return 77;

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        PrintStage("creating D3D12Core");
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Core creation failed; skipping camera2plus test: "
                  << exception.what() << "\n";
        return 77;
    }

    auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping camera2plus test\n";
        return 77;
    }

    const bool hardwareTrigger = IC4ExtTest::EnvInt("IC4EXT_TEST_HW_TRIGGER", 0) != 0;
    const char* triggerSourceEnvironment = IC4ExtTest::Env("IC4EXT_TEST_TRIGGER_SOURCE");
    const std::string triggerSource = triggerSourceEnvironment
                                          ? triggerSourceEnvironment
                                          : "Line1";
    const std::uint64_t toleranceNs = IC4ExtTest::EnvUInt64(
        "IC4EXT_TEST_SYNC_TOLERANCE_NS",
        hardwareTrigger ? 4'000'000ull : 10'000'000ull);
    const int targetSets =
        std::max(1, IC4ExtTest::EnvInt("IC4EXT_TEST_SYNC_SETS", 100));
    const int latencyWarmupSets =
        std::max(0, IC4ExtTest::EnvInt("IC4EXT_TEST_LATENCY_WARMUP_SETS", 100));
    const int timeoutSeconds =
        std::max(5, IC4ExtTest::EnvInt("IC4EXT_TEST_SYNC_TIMEOUT_SECONDS", 30));
    const int interCameraDelayMs =
        std::max(0, IC4ExtTest::EnvInt("IC4EXT_TEST_INTER_CAMERA_DELAY_MS", 1000));
    const unsigned readyTimeoutMs = static_cast<unsigned>(
        std::max(1000, IC4ExtTest::EnvInt("IC4EXT_TEST_GPU_READY_TIMEOUT_MS", 5000)));

    std::string latencyCsvPath;
    if (const char* path = IC4ExtTest::Env("IC4EXT_TEST_LATENCY_CSV")) {
        latencyCsvPath = path;
    }

    std::ofstream latencyCsv;
    if (!latencyCsvPath.empty()) {
        latencyCsv.open(latencyCsvPath, std::ios::out | std::ios::trunc);
        if (!latencyCsv) {
            std::cerr << "Failed to open latency CSV: " << latencyCsvPath << "\n";
            return 1;
        }
        WriteLatencyCsvHeader(latencyCsv);
    }

    std::cerr << "[camera2plus] mode=" << (hardwareTrigger ? "hardware" : "free-run")
              << " triggerSource=" << triggerSource
              << " toleranceNs=" << toleranceNs
              << " targetSets=" << targetSets
              << " latencyWarmupSets=" << latencyWarmupSets
              << " timeoutSeconds=" << timeoutSeconds
              << " interCameraDelayMs=" << interCameraDelayMs
              << " readyTimeoutMs=" << readyTimeoutMs
              << " latencyCsv=" << (latencyCsvPath.empty() ? "<disabled>" : latencyCsvPath)
              << std::endl;

    auto config0 = IC4ExtTest::MakeCameraConfig("d3d12", 0);
    auto config1 = IC4ExtTest::MakeCameraConfig("d3d12", 1);

    config0.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;
    config1.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Immediate;
    config0.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config1.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config0.maxPendingBuffers = 32;
    config1.maxPendingBuffers = 32;

    if (hardwareTrigger) {
        IC4Ext::ConfigureHardwareTriggerSync(config0, triggerSource);
        IC4Ext::ConfigureHardwareTriggerSync(config1, triggerSource);
    } else {
        auto gate = IC4Ext::MakeSoftwareTriggerSyncConfig();
        gate.setExposureAutoOff = false;
        IC4Ext::ConfigureCameraSync(config0, gate);
        IC4Ext::ConfigureCameraSync(config1, gate);
    }

    IC4Ext::IC4DeviceSelector selector0;
    selector0.deviceIndex = 0;
    IC4Ext::IC4DeviceSelector selector1;
    selector1.deviceIndex = 1;

    IC4Ext::D3D12CameraCapture capture0;
    IC4Ext::D3D12CameraCapture capture1;

    PrintStage("opening camera 0");
    if (!capture0.open(selector0, config0, backend)) {
        std::cerr << "camera0 open failed: " << capture0.lastError().where << ": "
                  << capture0.lastError().message << "\n";
        return 1;
    }
    PrintStage("pausing camera 0 with AcquisitionStop command");
    if (!capture0.setIC4Property("AcquisitionStop", std::string("execute"))) {
        std::cerr << "camera0 AcquisitionStop command failed: "
                  << capture0.lastError().where << ": "
                  << capture0.lastError().message << "\n";
        return 1;
    }

    if (interCameraDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interCameraDelayMs));
    }

    PrintStage("opening camera 1");
    if (!capture1.open(selector1, config1, backend)) {
        std::cerr << "camera1 open failed: " << capture1.lastError().where << ": "
                  << capture1.lastError().message << "\n";
        capture0.close();
        return 1;
    }
    PrintStage("pausing camera 1 with AcquisitionStop command");
    if (!capture1.setIC4Property("AcquisitionStop", std::string("execute"))) {
        std::cerr << "camera1 AcquisitionStop command failed: "
                  << capture1.lastError().where << ": "
                  << capture1.lastError().message << "\n";
        capture0.close();
        capture1.close();
        return 1;
    }

    ThreadKit::Queues::QueueOptions inputOptions;
    inputOptions.maxSize = 256;
    auto inputQueue = std::make_shared<IC4Ext::D3D12IndexedFrameQueue>(inputOptions);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = 32;
    auto outputQueue = std::make_shared<IC4Ext::D3D12SyncedFrameQueue>(outputOptions);

    IC4Ext::FrameSyncOptions syncOptions;
    syncOptions.policy = IC4Ext::FrameSyncPolicy::TimestampNearest;
    syncOptions.timestampSource = IC4Ext::FrameSyncTimestampSource::HostReceived;
    syncOptions.cameraIndices = {0, 1};
    syncOptions.maxTimestampDiffNs = toleranceNs;
    syncOptions.maxBufferedFramesPerCamera = 32;

    IC4Ext::D3D12FrameSyncThread sync(inputQueue, outputQueue, syncOptions);
    PrintStage("starting sync thread");
    if (!sync.start()) {
        std::cerr << "D3D12FrameSyncThread start failed: " << sync.lastError().where
                  << ": " << sync.lastError().message << "\n";
        return 1;
    }

    IC4Ext::CameraThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 1000;
    threadOptions.copyPerOutputQueue = false;
    threadOptions.stopOnReadError = false;

    IC4Ext::D3D12CameraCaptureThread camera0(std::move(capture0), backend, threadOptions);
    IC4Ext::D3D12CameraCaptureThread camera1(std::move(capture1), backend, threadOptions);
    camera0.addOutputQueue(0, inputQueue);
    camera1.addOutputQueue(1, inputQueue);

    PrintStage("starting camera workers");
    if (!camera0.start() || !camera1.start()) {
        std::cerr << "camera worker start failed\n";
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        sync.stopAndJoin();
        return 1;
    }

    if (!hardwareTrigger) {
        PrintStage("releasing free-run trigger gates");
        if (!camera0.setIC4Property("TriggerMode", std::string("Off")) ||
            !camera1.setIC4Property("TriggerMode", std::string("Off"))) {
            std::cerr << "Failed to release free-run trigger gate\n";
            camera0.stopAndJoin();
            camera1.stopAndJoin();
            sync.stopAndJoin();
            return 1;
        }
    }

    PrintStage("starting camera acquisitions with AcquisitionStart commands");
    if (!camera0.startAcquisition()) {
        std::cerr << "camera0 startAcquisition failed: " << camera0.lastError().where
                  << ": " << camera0.lastError().message << "\n";
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        sync.stopAndJoin();
        return 1;
    }
    if (!camera1.startAcquisition()) {
        std::cerr << "camera1 startAcquisition failed: " << camera1.lastError().where
                  << ": " << camera1.lastError().message << "\n";
        camera0.stopAcquisition();
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        sync.stopAndJoin();
        return 1;
    }

    int receivedSets = 0;
    int completedWarmupSets = 0;
    std::uint64_t maximumObservedDiffNs = 0;
    std::string validationFailure;
    LatencyCollector latency;
    latency.reserve(static_cast<std::size_t>(targetSets));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    auto nextStatsPrint = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    PrintStage("collecting synchronized sets and latency samples");
    while (receivedSets < targetSets && std::chrono::steady_clock::now() < deadline) {
        auto set = outputQueue->waitPopFor(std::chrono::milliseconds(200));
        const auto popTime = std::chrono::steady_clock::now();

        if (popTime >= nextStatsPrint) {
            PrintStats(camera0, camera1, sync, receivedSets, completedWarmupSets);
            nextStatsPrint = popTime + std::chrono::seconds(1);
        }

        if (!set) continue;

        if (set->frames.size() != 2) {
            validationFailure = "Unexpected synced frame count: " +
                                std::to_string(set->frames.size());
            std::cerr << validationFailure << "\n";
            break;
        }

        const IC4Ext::D3D12IndexedCameraFrame* frame0 = nullptr;
        const IC4Ext::D3D12IndexedCameraFrame* frame1 = nullptr;
        for (const auto& indexed : set->frames) {
            if (indexed.cameraIndex == 0) {
                if (frame0) {
                    validationFailure = "Synced set contains duplicate camera index 0";
                    break;
                }
                frame0 = &indexed;
            } else if (indexed.cameraIndex == 1) {
                if (frame1) {
                    validationFailure = "Synced set contains duplicate camera index 1";
                    break;
                }
                frame1 = &indexed;
            } else {
                validationFailure = "Synced set contains unexpected camera index " +
                                    std::to_string(indexed.cameraIndex);
                break;
            }
        }
        if (!validationFailure.empty()) {
            std::cerr << validationFailure << "\n";
            break;
        }
        if (!frame0 || !frame1) {
            validationFailure = "Synced set does not contain both camera 0 and camera 1";
            std::cerr << validationFailure << "\n";
            break;
        }

        if (!frame0->frame.texture || !frame1->frame.texture) {
            validationFailure = "Synced set contains an empty GPU texture";
            std::cerr << validationFailure << "\n";
            break;
        }
        if (!frame0->frame.ready.isValid() || !frame1->frame.ready.isValid()) {
            validationFailure = "Synced set contains an invalid GPU ready token";
            std::cerr << validationFailure << "\n";
            break;
        }

        LatencySample sample;
        if (!BuildLatencySample(*set, *frame0, *frame1, popTime, sample, validationFailure)) {
            std::cerr << validationFailure << "\n";
            break;
        }

        maximumObservedDiffNs = std::max(maximumObservedDiffNs, sample.hostPairDiffNs);
        if (sample.hostPairDiffNs > toleranceNs) {
            validationFailure = "Host timestamp difference exceeded tolerance: diff=" +
                                std::to_string(sample.hostPairDiffNs) +
                                " tolerance=" + std::to_string(toleranceNs);
            std::cerr << validationFailure << "\n";
            break;
        }

        if (!frame0->frame.ready.wait(readyTimeoutMs) ||
            !frame1->frame.ready.wait(readyTimeoutMs)) {
            validationFailure = "Timed out waiting for synchronized GPU frames";
            std::cerr << validationFailure << "\n";
            break;
        }

        if (completedWarmupSets < latencyWarmupSets) {
            ++completedWarmupSets;
            continue;
        }

        latency.add(sample);
        if (latencyCsv) {
            WriteLatencyCsvRow(
                latencyCsv, receivedSets, *set, *frame0, *frame1, sample);
        }
        ++receivedSets;
    }

    if (latencyCsv) {
        latencyCsv.flush();
        if (!latencyCsv) {
            validationFailure = "Failed while writing latency CSV";
            std::cerr << validationFailure << "\n";
        }
    }

    PrintStats(camera0, camera1, sync, receivedSets, completedWarmupSets);
    PrintStage("stopping acquisitions");
    const bool camera0Stopped = camera0.stopAcquisition();
    const bool camera1Stopped = camera1.stopAcquisition();
    PrintStage("joining workers");
    camera0.stopAndJoin();
    camera1.stopAndJoin();
    sync.stopAndJoin();
    core->WaitIdle();

    const auto camera0Stats = camera0.stats();
    const auto camera1Stats = camera1.stats();
    const auto syncStats = sync.stats();

    if (!latency.hostPairDiffNs.empty()) {
        latency.print();
    }
    if (!latencyCsvPath.empty()) {
        std::cout << "[latency] csv=" << latencyCsvPath << std::endl;
    }

    if (!validationFailure.empty()) return 1;
    if (!camera0Stopped || !camera1Stopped) {
        std::cerr << "One or more cameras failed AcquisitionStop command\n";
        return 1;
    }
    if (receivedSets < targetSets) {
        std::cerr << "Timed out while waiting for latency samples: measured="
                  << receivedSets << " target=" << targetSets
                  << " warmupCompleted=" << completedWarmupSets
                  << " warmupTarget=" << latencyWarmupSets << "\n";
        return 1;
    }
    if (camera0Stats.readErrors != 0 || camera1Stats.readErrors != 0) {
        std::cerr << "Camera read errors were recorded: camera0="
                  << camera0Stats.readErrors << " camera1=" << camera1Stats.readErrors << "\n";
        return 1;
    }

    std::cout << "test_camera2plus_frame_sync_smoke passed"
              << " mode=" << (hardwareTrigger ? "hardware" : "free-run")
              << " sets=" << receivedSets
              << " latencyWarmupSets=" << completedWarmupSets
              << " toleranceNs=" << toleranceNs
              << " maxObservedDiffNs=" << maximumObservedDiffNs
              << " syncInput=" << syncStats.inputFrames
              << " syncEmitted=" << syncStats.emittedSets
              << " syncDropped=" << syncStats.droppedFrames
              << " camera0Read=" << camera0Stats.readFrames
              << " camera1Read=" << camera1Stats.readFrames
              << "\n";
    return 0;
}
