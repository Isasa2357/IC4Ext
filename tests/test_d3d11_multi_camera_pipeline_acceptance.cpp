#include "TestCameraUtils.hpp"

#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Core/D3D11CoreConfig.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace Pipe = IC4Ext::D3D11;
using Clock = std::chrono::steady_clock;

enum class TestMode
{
    FreeRun,
    HardwareTrigger,
    LongRun160Fps,
};

struct Settings
{
    TestMode mode = TestMode::FreeRun;
    int camera0Device = 0;
    int camera1Device = 1;
    std::string triggerSource = "Line1";
    double requestedFps = 30.0;
    double expectedFps = 30.0;
    double minimumRateRatio = 0.0;
    double maximumSyncDropRatio = 0.01;
    std::uint64_t toleranceNs = 20'000'000;
    int warmupSets = 20;
    int targetSets = 100;
    int acceptanceSeconds = 600;
    int startupTimeoutSeconds = 30;
    int triggerArmDelayMs = 0;
    int interCameraOpenDelayMs = 1000;
    std::uint32_t readTimeoutMs = 1000;
    std::uint32_t readyTimeoutMs = 5000;
    std::size_t inputQueueCapacity = 512;
    std::size_t outputQueueCapacity = 128;
    std::size_t initialFramePoolCapacity = 64;
    std::size_t maxFramePoolCapacity = 256;
};

struct StatsSnapshot
{
    Pipe::CameraCaptureThreadStats thread0;
    Pipe::CameraCaptureThreadStats thread1;
    Pipe::FramePoolStats pool0;
    Pipe::FramePoolStats pool1;
    Pipe::FrameSyncStats sync;
    Pipe::FrameSyncOutputStats output;
};

struct Measurement
{
    std::uint64_t sets = 0;
    std::uint64_t maximumPairDiffNs = 0;
    long double totalPairDiffNs = 0.0L;
    std::uint64_t lastFrame0 = 0;
    std::uint64_t lastFrame1 = 0;
    bool haveLastFrameNumbers = false;
    Pipe::SyncGroupId lastSyncGroupId = 0;
    bool haveLastSyncGroup = false;
    Clock::time_point firstMeasuredTime{};
    Clock::time_point lastMeasuredTime{};
};

const char* ModeName(TestMode mode)
{
    switch (mode) {
    case TestMode::FreeRun: return "free-run";
    case TestMode::HardwareTrigger: return "hardware-trigger";
    case TestMode::LongRun160Fps: return "long-run-160fps";
    default: return "unknown";
    }
}

TestMode ParseMode()
{
    const char* value = IC4ExtTest::Env("IC4EXT_TEST_PIPELINE_MODE");
    if (!value || !*value) return TestMode::FreeRun;

    const std::string text(value);
    if (text == "free-run" || text == "freerun" || text == "e2e") {
        return TestMode::FreeRun;
    }
    if (text == "hardware" || text == "hardware-trigger" || text == "hw") {
        return TestMode::HardwareTrigger;
    }
    if (text == "long-run" || text == "long-run-160fps" ||
        text == "160fps") {
        return TestMode::LongRun160Fps;
    }

    std::cerr << "Unknown IC4EXT_TEST_PIPELINE_MODE='" << text
              << "'; using free-run\n";
    return TestMode::FreeRun;
}

std::size_t PositiveSizeEnv(const char* name, std::size_t fallback)
{
    const int value = IC4ExtTest::EnvInt(name, static_cast<int>(fallback));
    return value > 0 ? static_cast<std::size_t>(value) : fallback;
}

Settings LoadSettings()
{
    Settings settings;
    settings.mode = ParseMode();
    settings.camera0Device = IC4ExtTest::EnvInt(
        "IC4EXT_TEST_CAMERA0_DEVICE",
        IC4ExtTest::EnvInt("IC4EXT_TEST_DIRECT_DEVICE", 0));
    settings.camera1Device = IC4ExtTest::EnvInt(
        "IC4EXT_TEST_CAMERA1_DEVICE",
        IC4ExtTest::EnvInt("IC4EXT_TEST_THREADED_DEVICE", 1));

    if (const char* source = IC4ExtTest::Env("IC4EXT_TEST_TRIGGER_SOURCE")) {
        if (*source) settings.triggerSource = source;
    }

    switch (settings.mode) {
    case TestMode::FreeRun:
        settings.requestedFps = 30.0;
        settings.expectedFps = 30.0;
        settings.minimumRateRatio = 0.0;
        settings.toleranceNs = 20'000'000;
        settings.warmupSets = 20;
        settings.targetSets = 100;
        break;
    case TestMode::HardwareTrigger:
        settings.requestedFps = 160.0;
        settings.expectedFps = 160.0;
        settings.minimumRateRatio = 0.0;
        settings.toleranceNs = 4'000'000;
        settings.warmupSets = 100;
        settings.targetSets = 1000;
        break;
    case TestMode::LongRun160Fps:
        settings.requestedFps = 160.0;
        settings.expectedFps = 160.0;
        settings.minimumRateRatio = 0.95;
        settings.maximumSyncDropRatio = 0.001;
        settings.toleranceNs = 4'000'000;
        settings.warmupSets = 500;
        settings.acceptanceSeconds = 1800;
        break;
    }

    settings.requestedFps = IC4ExtTest::EnvDouble(
        "IC4EXT_TEST_FPS", settings.requestedFps);
    settings.expectedFps = IC4ExtTest::EnvDouble(
        "IC4EXT_TEST_EXPECTED_FPS", settings.expectedFps);
    settings.minimumRateRatio = IC4ExtTest::EnvDouble(
        "IC4EXT_TEST_MIN_RATE_RATIO", settings.minimumRateRatio);
    settings.maximumSyncDropRatio = IC4ExtTest::EnvDouble(
        "IC4EXT_TEST_MAX_SYNC_DROP_RATIO", settings.maximumSyncDropRatio);
    settings.toleranceNs = IC4ExtTest::EnvUInt64(
        "IC4EXT_TEST_SYNC_TOLERANCE_NS", settings.toleranceNs);
    settings.warmupSets = std::max(
        0,
        IC4ExtTest::EnvInt("IC4EXT_TEST_WARMUP_SETS", settings.warmupSets));
    settings.targetSets = std::max(
        1,
        IC4ExtTest::EnvInt("IC4EXT_TEST_SYNC_SETS", settings.targetSets));
    settings.acceptanceSeconds = std::max(
        1,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_ACCEPTANCE_SECONDS", settings.acceptanceSeconds));
    settings.startupTimeoutSeconds = std::max(
        5,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS",
            settings.startupTimeoutSeconds));
    settings.triggerArmDelayMs = std::max(
        0,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_TRIGGER_ARM_DELAY_MS", settings.triggerArmDelayMs));
    settings.interCameraOpenDelayMs = std::max(
        0,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_INTER_CAMERA_DELAY_MS",
            settings.interCameraOpenDelayMs));
    settings.readTimeoutMs = static_cast<std::uint32_t>(std::max(
        100,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_READ_TIMEOUT_MS",
            static_cast<int>(settings.readTimeoutMs))));
    settings.readyTimeoutMs = static_cast<std::uint32_t>(std::max(
        1000,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_GPU_READY_TIMEOUT_MS",
            static_cast<int>(settings.readyTimeoutMs))));
    settings.inputQueueCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_SYNC_INPUT_QUEUE_CAPACITY",
        settings.inputQueueCapacity);
    settings.outputQueueCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_SYNC_OUTPUT_QUEUE_CAPACITY",
        settings.outputQueueCapacity);
    settings.initialFramePoolCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_FRAME_POOL_INITIAL",
        settings.initialFramePoolCapacity);
    settings.maxFramePoolCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_FRAME_POOL_MAX",
        settings.maxFramePoolCapacity);
    settings.maxFramePoolCapacity = std::max(
        settings.initialFramePoolCapacity,
        settings.maxFramePoolCapacity);
    return settings;
}

bool IsManualModeEnabled(TestMode mode)
{
    if (mode == TestMode::HardwareTrigger) {
        return IC4ExtTest::EnvInt(
                   "IC4EXT_TEST_ENABLE_D3D11_HW_ACCEPTANCE",
                   IC4ExtTest::EnvInt("IC4EXT_TEST_ENABLE_HW_ACCEPTANCE", 0)) !=
               0;
    }
    if (mode == TestMode::LongRun160Fps) {
        return IC4ExtTest::EnvInt(
                   "IC4EXT_TEST_ENABLE_D3D11_LONG_RUN_ACCEPTANCE",
                   IC4ExtTest::EnvInt(
                       "IC4EXT_TEST_ENABLE_LONG_RUN_ACCEPTANCE", 0)) != 0;
    }
    return true;
}

std::uint64_t HostTimestampNs(const Pipe::ReadOnlyFrame& frame)
{
    const auto time = frame.timing().hostReceivedTime;
    if (time == Clock::time_point{}) return 0;
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        time.time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t AbsDiff(std::uint64_t lhs, std::uint64_t rhs)
{
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

template<class Value>
std::uint64_t Delta(Value current, Value baseline)
{
    return current >= baseline
               ? static_cast<std::uint64_t>(current - baseline)
               : 0;
}

bool StopStartedGroup(Pipe::MultiCameraStartupResult& started) noexcept
{
    bool stopped = true;
    for (auto& thread : started.captureThreads) {
        if (thread) stopped = thread->stopAcquisition() && stopped;
    }
    for (auto& capture : started.captures) {
        stopped = capture.capture.stopAcquisition() && stopped;
    }
    for (auto& thread : started.captureThreads) {
        if (thread) thread->stopAndJoin();
    }
    for (auto& capture : started.captures) {
        capture.capture.close();
    }
    return stopped;
}

StatsSnapshot Snapshot(
    const Pipe::CameraCaptureThread& camera0,
    const Pipe::CameraCaptureThread& camera1,
    const Pipe::FrameSyncThread& sync,
    Pipe::FrameSyncOutputId outputId)
{
    StatsSnapshot result;
    result.thread0 = camera0.stats();
    result.thread1 = camera1.stats();
    result.pool0 = camera0.framePoolStats();
    result.pool1 = camera1.framePoolStats();
    result.sync = sync.stats();
    if (const auto output = sync.outputStats(outputId)) {
        result.output = *output;
    }
    return result;
}

void PrintProgress(
    const Settings& settings,
    const Measurement& measurement,
    const StatsSnapshot& baseline,
    const StatsSnapshot& current)
{
    const auto now = Clock::now();
    double elapsedSeconds = 0.0;
    if (measurement.firstMeasuredTime != Clock::time_point{}) {
        elapsedSeconds = std::chrono::duration<double>(
            now - measurement.firstMeasuredTime).count();
    }
    const double fps = elapsedSeconds > 0.0
                           ? static_cast<double>(measurement.sets) /
                                 elapsedSeconds
                           : 0.0;

    std::cerr << std::fixed << std::setprecision(3)
              << "[d3d11-v2-acceptance] mode=" << ModeName(settings.mode)
              << " sets=" << measurement.sets
              << " elapsedSec=" << elapsedSeconds
              << " syncFps=" << fps
              << " camera0Read="
              << Delta(current.thread0.readFrames, baseline.thread0.readFrames)
              << " camera1Read="
              << Delta(current.thread1.readFrames, baseline.thread1.readFrames)
              << " syncCompleted="
              << Delta(current.sync.completedSets, baseline.sync.completedSets)
              << " syncDropped="
              << Delta(current.sync.droppedFrames, baseline.sync.droppedFrames)
              << " outputDrops="
              << Delta(current.output.queueDrops, baseline.output.queueDrops)
              << " pool0Exhaustion="
              << Delta(
                     current.pool0.exhaustionDrops,
                     baseline.pool0.exhaustionDrops)
              << " pool1Exhaustion="
              << Delta(
                     current.pool1.exhaustionDrops,
                     baseline.pool1.exhaustionDrops)
              << '\n';
}

bool ValidateFrameSet(
    const Pipe::ReadOnlyFrameSet& set,
    const Settings& settings,
    Measurement& measurement,
    std::string& failure)
{
    if (!set) {
        failure = "FrameSyncThread emitted an invalid ReadOnlyFrameSet";
        return false;
    }
    if (set.size() != 2 || !set.contains(0) || !set.contains(1)) {
        failure = "Synchronized set does not contain exactly camera IDs 0 and 1";
        return false;
    }

    const Pipe::ReadOnlyFrame* frame0 = set.find(0);
    const Pipe::ReadOnlyFrame* frame1 = set.find(1);
    if (!frame0 || !frame1 || !*frame0 || !*frame1) {
        failure = "Synchronized set contains an invalid frame";
        return false;
    }
    if (!frame0->hasResource() || !frame1->hasResource() ||
        !frame0->hasSrv() || !frame1->hasSrv()) {
        failure = "Synchronized set contains an incomplete GPU frame";
        return false;
    }
    if (!frame0->waitReady(settings.readyTimeoutMs) ||
        !frame1->waitReady(settings.readyTimeoutMs)) {
        failure = "Timed out waiting for synchronized GPU frames";
        return false;
    }
    if (frame0->format().width <= 0 || frame0->format().height <= 0 ||
        frame1->format().width <= 0 || frame1->format().height <= 0) {
        failure = "Synchronized set contains invalid frame dimensions";
        return false;
    }

    const std::uint64_t host0 = HostTimestampNs(*frame0);
    const std::uint64_t host1 = HostTimestampNs(*frame1);
    if (host0 == 0 || host1 == 0) {
        failure = "Synchronized set has no host-received timestamp";
        return false;
    }

    const std::uint64_t pairDiff = AbsDiff(host0, host1);
    if (pairDiff > settings.toleranceNs) {
        failure = "Host timestamp difference exceeded tolerance: diff=" +
                  std::to_string(pairDiff) +
                  " tolerance=" + std::to_string(settings.toleranceNs);
        return false;
    }

    if (measurement.haveLastFrameNumbers &&
        (frame0->timing().frameNumber <= measurement.lastFrame0 ||
         frame1->timing().frameNumber <= measurement.lastFrame1)) {
        failure = "Frame numbers did not advance monotonically";
        return false;
    }
    if (measurement.haveLastSyncGroup &&
        set.syncGroupId() <= measurement.lastSyncGroupId) {
        failure = "Sync group ID did not advance monotonically";
        return false;
    }

    measurement.lastFrame0 = frame0->timing().frameNumber;
    measurement.lastFrame1 = frame1->timing().frameNumber;
    measurement.haveLastFrameNumbers = true;
    measurement.lastSyncGroupId = set.syncGroupId();
    measurement.haveLastSyncGroup = true;
    measurement.maximumPairDiffNs =
        std::max(measurement.maximumPairDiffNs, pairDiff);
    measurement.totalPairDiffNs += static_cast<long double>(pairDiff);
    return true;
}

bool ShouldFinish(
    const Settings& settings,
    const Measurement& measurement,
    Clock::time_point now)
{
    if (settings.mode == TestMode::LongRun160Fps) {
        if (measurement.firstMeasuredTime == Clock::time_point{}) return false;
        return now - measurement.firstMeasuredTime >=
               std::chrono::seconds(settings.acceptanceSeconds);
    }
    return measurement.sets >= static_cast<std::uint64_t>(settings.targetSets);
}

bool ValidateAcceptance(
    const Settings& settings,
    const Measurement& measurement,
    const StatsSnapshot& baseline,
    const StatsSnapshot& finalStats,
    std::string& failure)
{
    if (measurement.sets == 0 ||
        measurement.firstMeasuredTime == Clock::time_point{} ||
        measurement.lastMeasuredTime == Clock::time_point{}) {
        failure = "No measured synchronized sets were produced";
        return false;
    }

    double elapsedSeconds = std::chrono::duration<double>(
        measurement.lastMeasuredTime - measurement.firstMeasuredTime).count();
    if (elapsedSeconds <= 0.0) {
        elapsedSeconds = std::numeric_limits<double>::epsilon();
    }
    const double observedFps =
        static_cast<double>(measurement.sets) / elapsedSeconds;

    const std::uint64_t camera0Read =
        Delta(finalStats.thread0.readFrames, baseline.thread0.readFrames);
    const std::uint64_t camera1Read =
        Delta(finalStats.thread1.readFrames, baseline.thread1.readFrames);
    const std::uint64_t camera0Timeouts =
        Delta(finalStats.thread0.readTimeouts, baseline.thread0.readTimeouts);
    const std::uint64_t camera1Timeouts =
        Delta(finalStats.thread1.readTimeouts, baseline.thread1.readTimeouts);
    const std::uint64_t camera0Errors =
        Delta(finalStats.thread0.readErrors, baseline.thread0.readErrors);
    const std::uint64_t camera1Errors =
        Delta(finalStats.thread1.readErrors, baseline.thread1.readErrors);
    const std::uint64_t camera0InputDrops = Delta(
        finalStats.thread0.droppedOldestAndPushed,
        baseline.thread0.droppedOldestAndPushed);
    const std::uint64_t camera1InputDrops = Delta(
        finalStats.thread1.droppedOldestAndPushed,
        baseline.thread1.droppedOldestAndPushed);
    const std::uint64_t camera0PushFailures =
        Delta(finalStats.thread0.pushFailures, baseline.thread0.pushFailures);
    const std::uint64_t camera1PushFailures =
        Delta(finalStats.thread1.pushFailures, baseline.thread1.pushFailures);
    const std::uint64_t pool0Exhaustion = Delta(
        finalStats.pool0.exhaustionDrops, baseline.pool0.exhaustionDrops);
    const std::uint64_t pool1Exhaustion = Delta(
        finalStats.pool1.exhaustionDrops, baseline.pool1.exhaustionDrops);
    const std::uint64_t pool0WaitTimeouts =
        Delta(finalStats.pool0.waitTimeouts, baseline.pool0.waitTimeouts);
    const std::uint64_t pool1WaitTimeouts =
        Delta(finalStats.pool1.waitTimeouts, baseline.pool1.waitTimeouts);
    const std::uint64_t syncInput =
        Delta(finalStats.sync.inputFrames, baseline.sync.inputFrames);
    const std::uint64_t syncDropped =
        Delta(finalStats.sync.droppedFrames, baseline.sync.droppedFrames);
    const std::uint64_t syncIncomplete =
        Delta(finalStats.sync.incompleteSets, baseline.sync.incompleteSets);
    const std::uint64_t outputDrops =
        Delta(finalStats.output.queueDrops, baseline.output.queueDrops);
    const std::uint64_t outputErrors =
        Delta(finalStats.output.dispatchErrors, baseline.output.dispatchErrors);

    if (camera0Errors != 0 || camera1Errors != 0) {
        failure = "Camera read errors were recorded";
        return false;
    }
    if (camera0PushFailures != 0 || camera1PushFailures != 0) {
        failure = "Camera-to-sync queue push failures were recorded";
        return false;
    }
    if (camera0InputDrops != 0 || camera1InputDrops != 0) {
        failure = "Camera-to-sync ingress queue dropped frames";
        return false;
    }
    if (pool0Exhaustion != 0 || pool1Exhaustion != 0 ||
        pool0WaitTimeouts != 0 || pool1WaitTimeouts != 0) {
        failure = "One or more CameraCapture FramePools were exhausted";
        return false;
    }
    if (outputDrops != 0 || outputErrors != 0) {
        failure = "FrameSyncThread output recorded drops or dispatch errors";
        return false;
    }

    if (settings.mode == TestMode::LongRun160Fps) {
        const double requiredFps =
            settings.expectedFps * settings.minimumRateRatio;
        if (!std::isfinite(observedFps) || observedFps < requiredFps) {
            failure = "Observed synchronized FPS was below acceptance threshold: " +
                      std::to_string(observedFps) + " < " +
                      std::to_string(requiredFps);
            return false;
        }
        if (camera0Timeouts != 0 || camera1Timeouts != 0) {
            failure = "Camera read timeouts occurred during long-run measurement";
            return false;
        }

        const double dropRatio = syncInput == 0
                                     ? 0.0
                                     : static_cast<double>(syncDropped) /
                                           static_cast<double>(syncInput);
        if (!std::isfinite(dropRatio) ||
            dropRatio > settings.maximumSyncDropRatio) {
            failure = "FrameSyncThread drop ratio exceeded threshold: " +
                      std::to_string(dropRatio) + " > " +
                      std::to_string(settings.maximumSyncDropRatio);
            return false;
        }

        const std::uint64_t smallerRead = std::min(camera0Read, camera1Read);
        const std::uint64_t largerRead = std::max(camera0Read, camera1Read);
        const std::uint64_t allowedImbalance = std::max<std::uint64_t>(
            2,
            static_cast<std::uint64_t>(
                std::ceil(static_cast<double>(smallerRead) * 0.01)));
        if (largerRead - smallerRead > allowedImbalance) {
            failure = "Camera read-count imbalance exceeded 1%";
            return false;
        }
    }

    const double meanPairDiffUs = static_cast<double>(
                                      measurement.totalPairDiffNs /
                                      static_cast<long double>(measurement.sets)) /
                                  1000.0;

    std::cout << std::fixed << std::setprecision(3)
              << "[d3d11-v2-acceptance-result]"
              << " mode=" << ModeName(settings.mode)
              << " sets=" << measurement.sets
              << " elapsedSec=" << elapsedSeconds
              << " syncFps=" << observedFps
              << " expectedFps=" << settings.expectedFps
              << " minRateRatio=" << settings.minimumRateRatio
              << " maxPairDiffUs="
              << static_cast<double>(measurement.maximumPairDiffNs) / 1000.0
              << " meanPairDiffUs=" << meanPairDiffUs
              << " camera0Read=" << camera0Read
              << " camera1Read=" << camera1Read
              << " camera0Timeouts=" << camera0Timeouts
              << " camera1Timeouts=" << camera1Timeouts
              << " syncInput=" << syncInput
              << " syncDropped=" << syncDropped
              << " syncIncomplete=" << syncIncomplete
              << " outputDrops=" << outputDrops
              << " outputErrors=" << outputErrors
              << " pool0Exhaustion=" << pool0Exhaustion
              << " pool1Exhaustion=" << pool1Exhaustion
              << '\n';
    return true;
}

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;
    const Settings settings = LoadSettings();

    std::cerr << "[d3d11-v2-acceptance] mode=" << ModeName(settings.mode)
              << " camera0Device=" << settings.camera0Device
              << " camera1Device=" << settings.camera1Device
              << " requestedFps=" << settings.requestedFps
              << " expectedFps=" << settings.expectedFps
              << " toleranceNs=" << settings.toleranceNs
              << " warmupSets=" << settings.warmupSets
              << " targetSets=" << settings.targetSets
              << " acceptanceSeconds=" << settings.acceptanceSeconds
              << " triggerSource=" << settings.triggerSource
              << " triggerArmDelayMs=" << settings.triggerArmDelayMs
              << '\n';

    if (!IsManualModeEnabled(settings.mode)) {
        std::cerr
            << "Skipping manual D3D11 hardware acceptance mode. Set "
            << (settings.mode == TestMode::LongRun160Fps
                    ? "IC4EXT_TEST_ENABLE_D3D11_LONG_RUN_ACCEPTANCE=1"
                    : "IC4EXT_TEST_ENABLE_D3D11_HW_ACCEPTANCE=1")
            << " to run it.\n";
        return 77;
    }

    if (settings.camera0Device < 0 || settings.camera1Device < 0 ||
        settings.camera0Device == settings.camera1Device) {
        std::cerr << "Two distinct non-negative device indices are required\n";
        return 1;
    }
    if (!IC4ExtTest::RequireCameraCount(2)) return 77;

    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        D3D11CoreLib::D3D11CoreConfig coreConfig;
        coreConfig.enableDebugLayer = false;
        coreConfig.enableInfoQueue = false;
        coreConfig.enableMultithreadProtection = true;
        coreConfig.allowWarpAdapter = true;
        core = D3D11CoreLib::D3D11Core::CreateShared(coreConfig);
    } catch (const std::exception& exception) {
        std::cerr << "D3D11Core creation failed; skipping test: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D11BackendContext::FromCore(core, true);
    if (!backend.resolve()) {
        std::cerr << "D3D11 backend resolve failed; skipping test\n";
        return 77;
    }

    ThreadKit::Queues::QueueOptions inputOptions;
    inputOptions.maxSize = settings.inputQueueCapacity;
    inputOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto inputQueue =
        std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(inputOptions);

    ThreadKit::Queues::QueueOptions outputOptions;
    outputOptions.maxSize = settings.outputQueueCapacity;
    outputOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto outputQueue =
        std::make_shared<Pipe::ReadOnlyFrameSetQueue>(outputOptions);

    Pipe::FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::HostReceived;
    syncConfig.maxTimestampDiffNs = settings.toleranceNs;
    syncConfig.maxBufferedFramesPerCamera = 64;
    syncConfig.groupTimeout = std::chrono::milliseconds(100);

    Pipe::FrameSyncThread sync(inputQueue, syncConfig);

    Pipe::FrameSyncOutputConfig outputConfig;
    outputConfig.requiredCameras = {0, 1};
    outputConfig.frameRate = Pipe::FrameRateLimit::Maximum();
    outputConfig.priority = 0;
    outputConfig.enabled = true;

    const Pipe::FrameSyncOutputId outputId =
        sync.registerOutput(outputQueue, outputConfig);
    if (outputId == Pipe::InvalidFrameSyncOutputId) {
        const auto error = sync.lastError();
        std::cerr << "FrameSyncThread output registration failed: "
                  << error.where << ": " << error.message << '\n';
        return 1;
    }
    if (!sync.start()) {
        const auto error = sync.lastError();
        std::cerr << "FrameSyncThread start failed: "
                  << error.where << ": " << error.message << '\n';
        return 1;
    }

    auto config0 = IC4ExtTest::MakeCameraConfig(
        "d3d11", settings.camera0Device);
    auto config1 = IC4ExtTest::MakeCameraConfig(
        "d3d11", settings.camera1Device);
    config0.streamRequest.fps = settings.requestedFps;
    config1.streamRequest.fps = settings.requestedFps;
    config0.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config1.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config0.maxPendingBuffers = 64;
    config1.maxPendingBuffers = 64;

    const bool hardwareTriggered = settings.mode != TestMode::FreeRun;
    if (hardwareTriggered) {
        IC4Ext::ConfigureHardwareTriggerSync(config0, settings.triggerSource);
        IC4Ext::ConfigureHardwareTriggerSync(config1, settings.triggerSource);
    } else {
        IC4Ext::ConfigureNoSync(config0);
        IC4Ext::ConfigureNoSync(config1);
    }

    Pipe::CameraCaptureOptions captureOptions;
    captureOptions.initialFramePoolCapacity =
        settings.initialFramePoolCapacity;
    captureOptions.maxFramePoolCapacity =
        settings.maxFramePoolCapacity;
    captureOptions.framePoolExhaustionPolicy =
        Pipe::FramePoolExhaustionPolicy::DropNewest;
    captureOptions.framePoolWaitTimeout = std::chrono::milliseconds(5);

    Pipe::CameraCaptureThreadStartupConfig camera0;
    camera0.capture.cameraId = 0;
    camera0.capture.selector.deviceIndex = settings.camera0Device;
    camera0.capture.captureConfig = config0;
    camera0.capture.captureOptions = captureOptions;
    camera0.capture.openOrder = 0;
    camera0.threadOptions.readTimeoutMs = settings.readTimeoutMs;
    camera0.threadOptions.stopOnReadError = false;
    camera0.outputQueue = inputQueue;

    Pipe::CameraCaptureThreadStartupConfig camera1;
    camera1.capture.cameraId = 1;
    camera1.capture.selector.deviceIndex = settings.camera1Device;
    camera1.capture.captureConfig = config1;
    camera1.capture.captureOptions = captureOptions;
    camera1.capture.openOrder = 1;
    camera1.threadOptions.readTimeoutMs = settings.readTimeoutMs;
    camera1.threadOptions.stopOnReadError = false;
    camera1.outputQueue = inputQueue;

    Pipe::MultiCameraStartupOptions startupOptions;
    startupOptions.interCameraOpenDelay =
        std::chrono::milliseconds(settings.interCameraOpenDelayMs);

    std::cerr
        << "[d3d11-v2-acceptance] starting two CameraCaptureThread instances "
           "through OpenAndStartMultiCameraGroup\n";

    auto started = Pipe::OpenAndStartMultiCameraGroup(
        backend,
        {},
        std::vector<Pipe::CameraCaptureThreadStartupConfig>{camera0, camera1},
        startupOptions);

    if (!started) {
        const auto error = started.error;
        std::cerr << "OpenAndStartMultiCameraGroup failed: "
                  << error.where << ": " << error.message << '\n';
        sync.stopAndJoin();
        return 1;
    }

    if (!started.captures.empty() ||
        started.captureThreads.size() != 2 ||
        !started.captureThreads[0] || !started.captureThreads[1] ||
        started.captureThreads[0]->cameraId() != 0 ||
        started.captureThreads[1]->cameraId() != 1 ||
        !started.captureThreads[0]->isRunning() ||
        !started.captureThreads[1]->isRunning()) {
        std::cerr << "Unexpected startup result shape or worker state\n";
        StopStartedGroup(started);
        sync.stopAndJoin();
        return 1;
    }

    if (hardwareTriggered) {
        std::cerr
            << "[d3d11-v2-acceptance] cameras are armed. Start the external "
               "hardware trigger now";
        if (settings.triggerArmDelayMs > 0) {
            std::cerr << "; collection begins in "
                      << settings.triggerArmDelayMs << " ms";
        }
        std::cerr << '\n';
        if (settings.triggerArmDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(settings.triggerArmDelayMs));
        }
    }

    Pipe::CameraCaptureThread& thread0 = *started.captureThreads[0];
    Pipe::CameraCaptureThread& thread1 = *started.captureThreads[1];

    Measurement measurement;
    std::string failure;
    int completedWarmupSets = 0;
    bool baselineCaptured = false;
    StatsSnapshot baseline;

    const auto loopStart = Clock::now();
    const auto startupDeadline =
        loopStart + std::chrono::seconds(settings.startupTimeoutSeconds);
    const double nominalTargetSeconds = settings.expectedFps > 0.0
                                            ? static_cast<double>(
                                                  settings.targetSets) /
                                                  settings.expectedFps
                                            : static_cast<double>(
                                                  settings.startupTimeoutSeconds);
    const int collectionTimeoutSeconds =
        settings.mode == TestMode::LongRun160Fps
            ? settings.acceptanceSeconds + 30
            : std::max(
                  30,
                  static_cast<int>(std::ceil(nominalTargetSeconds * 3.0)) +
                      10);
    const auto overallDeadline =
        loopStart + std::chrono::seconds(
                        settings.startupTimeoutSeconds +
                        collectionTimeoutSeconds);
    auto nextProgress = loopStart + std::chrono::seconds(5);

    while (true) {
        const auto now = Clock::now();
        if (now >= overallDeadline) {
            failure = "Timed out before completing the requested acceptance run";
            break;
        }
        if (!baselineCaptured && now >= startupDeadline) {
            failure = "Timed out before completing synchronized warmup";
            break;
        }
        if (baselineCaptured && ShouldFinish(settings, measurement, now)) {
            break;
        }

        auto set = outputQueue->waitPopFor(std::chrono::milliseconds(250));
        const auto popTime = Clock::now();

        if (popTime >= nextProgress) {
            const StatsSnapshot current =
                Snapshot(thread0, thread1, sync, outputId);
            if (baselineCaptured) {
                PrintProgress(settings, measurement, baseline, current);
            }
            nextProgress = popTime + std::chrono::seconds(5);
        }

        if (!set) continue;

        Measurement validationState = measurement;
        if (!ValidateFrameSet(
                *set, settings, validationState, failure)) {
            break;
        }
        measurement = validationState;

        if (completedWarmupSets < settings.warmupSets) {
            ++completedWarmupSets;
            if (completedWarmupSets == settings.warmupSets) {
                baseline = Snapshot(thread0, thread1, sync, outputId);
                baselineCaptured = true;
                measurement = {};
                std::cerr
                    << "[d3d11-v2-acceptance] warmup complete; measurement begins\n";
            }
            continue;
        }

        if (!baselineCaptured) {
            baseline = Snapshot(thread0, thread1, sync, outputId);
            baselineCaptured = true;
            measurement = {};
        }

        if (measurement.sets == 0) {
            measurement.firstMeasuredTime = popTime;
        }
        measurement.lastMeasuredTime = popTime;
        ++measurement.sets;

        if (settings.mode != TestMode::LongRun160Fps &&
            measurement.sets >= static_cast<std::uint64_t>(
                                    settings.targetSets)) {
            break;
        }
    }

    const StatsSnapshot finalStats =
        Snapshot(thread0, thread1, sync, outputId);

    std::cerr << "[d3d11-v2-acceptance] stopping acquisitions and workers\n";
    const bool acquisitionsStopped = StopStartedGroup(started);
    const bool supplyStopped = sync.stopOutputSupply(outputId);
    outputQueue->clear();
    const bool outputClosed = sync.closeOutputChannel(outputId);
    sync.stopAndJoin();
    inputQueue->close();

    if (!failure.empty()) {
        std::cerr << "[d3d11-v2-acceptance] FAILED: " << failure << '\n';
        return 1;
    }
    if (!acquisitionsStopped) {
        std::cerr
            << "[d3d11-v2-acceptance] FAILED: one or more AcquisitionStop commands failed\n";
        return 1;
    }
    if (!supplyStopped || !outputClosed) {
        std::cerr
            << "[d3d11-v2-acceptance] FAILED: output retirement failed\n";
        return 1;
    }
    if (!baselineCaptured) {
        std::cerr << "[d3d11-v2-acceptance] FAILED: no measurement baseline\n";
        return 1;
    }
    if (!ValidateAcceptance(
            settings, measurement, baseline, finalStats, failure)) {
        std::cerr << "[d3d11-v2-acceptance] FAILED: " << failure << '\n';
        return 1;
    }

    std::cout << "test_d3d11_multi_camera_pipeline_acceptance passed"
              << " mode=" << ModeName(settings.mode) << '\n';
    return 0;
}
