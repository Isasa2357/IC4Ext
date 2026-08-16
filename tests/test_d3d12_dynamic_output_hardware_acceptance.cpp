#include "TestCameraUtils.hpp"

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace Pipe = IC4Ext::D3D12;
using Clock = std::chrono::steady_clock;

struct Settings
{
    int camera0Device = 0;
    int camera1Device = 1;
    std::string triggerSource = "Line1";
    double requestedFps = 160.0;
    double expectedFps = 160.0;
    double minimumRateRatio = 0.95;
    double maximumSyncDropRatio = 0.001;
    std::uint64_t toleranceNs = 4'000'000;
    int warmupSets = 100;
    int churnCycles = 200;
    int postStopPermanentSets = 4;
    int startupTimeoutSeconds = 60;
    int totalTimeoutSeconds = 180;
    int triggerArmDelayMs = 1;
    int interCameraOpenDelayMs = 1000;
    std::uint32_t readTimeoutMs = 1000;
    std::size_t inputQueueCapacity = 512;
    std::size_t permanentQueueCapacity = 512;
    std::size_t dynamicQueueCapacity = 32;
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
    Pipe::FrameSyncOutputStats permanentOutput;
};

struct PermanentConsumerState
{
    std::atomic<std::uint64_t> sets{0};
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::string failure;
};

template <class Value>
std::uint64_t Delta(Value current, Value baseline)
{
    return current >= baseline
               ? static_cast<std::uint64_t>(current - baseline)
               : 0;
}

std::size_t PositiveSizeEnv(const char* name, std::size_t fallback)
{
    const int value = IC4ExtTest::EnvInt(name, static_cast<int>(fallback));
    return value > 0 ? static_cast<std::size_t>(value) : fallback;
}

Settings LoadSettings()
{
    Settings settings;
    settings.camera0Device = IC4ExtTest::EnvInt(
        "IC4EXT_TEST_CAMERA0_DEVICE",
        IC4ExtTest::EnvInt("IC4EXT_TEST_DIRECT_DEVICE", 0));
    settings.camera1Device = IC4ExtTest::EnvInt(
        "IC4EXT_TEST_CAMERA1_DEVICE",
        IC4ExtTest::EnvInt("IC4EXT_TEST_THREADED_DEVICE", 1));

    if (const char* source = IC4ExtTest::Env("IC4EXT_TEST_TRIGGER_SOURCE")) {
        if (*source) settings.triggerSource = source;
    }

    settings.requestedFps =
        IC4ExtTest::EnvDouble("IC4EXT_TEST_FPS", settings.requestedFps);
    settings.expectedFps =
        IC4ExtTest::EnvDouble("IC4EXT_TEST_EXPECTED_FPS", settings.expectedFps);
    settings.minimumRateRatio = IC4ExtTest::EnvDouble(
        "IC4EXT_TEST_MIN_RATE_RATIO", settings.minimumRateRatio);
    settings.maximumSyncDropRatio = IC4ExtTest::EnvDouble(
        "IC4EXT_TEST_MAX_SYNC_DROP_RATIO", settings.maximumSyncDropRatio);
    settings.toleranceNs = IC4ExtTest::EnvUInt64(
        "IC4EXT_TEST_SYNC_TOLERANCE_NS", settings.toleranceNs);
    settings.warmupSets = std::max(
        1,
        IC4ExtTest::EnvInt("IC4EXT_TEST_WARMUP_SETS", settings.warmupSets));
    settings.churnCycles = std::max(
        1,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_DYNAMIC_OUTPUT_CYCLES", settings.churnCycles));
    settings.postStopPermanentSets = std::max(
        1,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_DYNAMIC_OUTPUT_POST_STOP_SETS",
            settings.postStopPermanentSets));
    settings.startupTimeoutSeconds = std::max(
        5,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_SYNC_TIMEOUT_SECONDS",
            settings.startupTimeoutSeconds));
    settings.totalTimeoutSeconds = std::max(
        settings.startupTimeoutSeconds + 10,
        IC4ExtTest::EnvInt(
            "IC4EXT_TEST_DYNAMIC_OUTPUT_TIMEOUT_SECONDS",
            settings.totalTimeoutSeconds));
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
    settings.inputQueueCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_SYNC_INPUT_QUEUE_CAPACITY",
        settings.inputQueueCapacity);
    settings.permanentQueueCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_SYNC_OUTPUT_QUEUE_CAPACITY",
        settings.permanentQueueCapacity);
    settings.dynamicQueueCapacity = PositiveSizeEnv(
        "IC4EXT_TEST_DYNAMIC_OUTPUT_QUEUE_CAPACITY",
        settings.dynamicQueueCapacity);
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

void SetPermanentFailure(
    PermanentConsumerState& state,
    const std::string& failure)
{
    bool expected = false;
    if (!state.failed.compare_exchange_strong(expected, true)) return;
    std::lock_guard<std::mutex> lock(state.failureMutex);
    state.failure = failure;
}

std::string PermanentFailure(PermanentConsumerState& state)
{
    std::lock_guard<std::mutex> lock(state.failureMutex);
    return state.failure;
}

bool ValidatePermanentSet(
    const Pipe::ReadOnlyFrameSet& set,
    Pipe::SyncGroupId& lastGroup,
    std::uint64_t& lastFrame0,
    std::uint64_t& lastFrame1,
    bool& havePrevious,
    std::string& failure)
{
    if (!set || set.size() != 2 || !set.contains(0) || !set.contains(1)) {
        failure = "Permanent output emitted an invalid two-camera frame set";
        return false;
    }

    const auto* frame0 = set.find(0);
    const auto* frame1 = set.find(1);
    if (!frame0 || !frame1 || !*frame0 || !*frame1 ||
        !frame0->hasResource() || !frame1->hasResource() ||
        !frame0->hasSrv() || !frame1->hasSrv()) {
        failure = "Permanent output contains an invalid GPU frame";
        return false;
    }

    if (havePrevious) {
        if (set.syncGroupId() <= lastGroup) {
            failure = "Permanent output syncGroupId did not advance monotonically";
            return false;
        }
        if (frame0->timing().frameNumber <= lastFrame0 ||
            frame1->timing().frameNumber <= lastFrame1) {
            failure = "Permanent output frame number did not advance monotonically";
            return false;
        }
    }

    lastGroup = set.syncGroupId();
    lastFrame0 = frame0->timing().frameNumber;
    lastFrame1 = frame1->timing().frameNumber;
    havePrevious = true;
    return true;
}

void PermanentConsumerLoop(
    const std::shared_ptr<Pipe::ReadOnlyFrameSetQueue>& queue,
    PermanentConsumerState& state)
{
    Pipe::SyncGroupId lastGroup = 0;
    std::uint64_t lastFrame0 = 0;
    std::uint64_t lastFrame1 = 0;
    bool havePrevious = false;

    try {
        for (;;) {
            auto set = queue->waitPopFor(std::chrono::milliseconds(250));
            if (!set) {
                if (queue->isClosed() && queue->empty()) break;
                continue;
            }

            std::string failure;
            if (!ValidatePermanentSet(
                    *set,
                    lastGroup,
                    lastFrame0,
                    lastFrame1,
                    havePrevious,
                    failure)) {
                SetPermanentFailure(state, failure);
                return;
            }
            state.sets.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception& exception) {
        SetPermanentFailure(
            state,
            std::string("Permanent consumer exception: ") + exception.what());
    } catch (...) {
        SetPermanentFailure(state, "Permanent consumer unknown exception");
    }
}

bool WaitForHealthyCondition(
    const std::function<bool()>& predicate,
    PermanentConsumerState& permanent,
    const Pipe::FrameSyncThread& sync,
    const Pipe::CameraCaptureThread& camera0,
    const Pipe::CameraCaptureThread& camera1,
    Clock::time_point deadline,
    std::string& failure)
{
    while (Clock::now() < deadline) {
        if (permanent.failed.load(std::memory_order_relaxed)) {
            failure = PermanentFailure(permanent);
            return false;
        }
        if (!sync.isRunning() || !camera0.isRunning() || !camera1.isRunning()) {
            failure = "Central sync or a CameraCaptureThread stopped during dynamic churn";
            return false;
        }
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    failure = "Timed out while waiting for dynamic-output acceptance progress";
    return false;
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
    Pipe::FrameSyncOutputId permanentOutputId)
{
    StatsSnapshot result;
    result.thread0 = camera0.stats();
    result.thread1 = camera1.stats();
    result.pool0 = camera0.framePoolStats();
    result.pool1 = camera1.framePoolStats();
    result.sync = sync.stats();
    if (const auto output = sync.outputStats(permanentOutputId)) {
        result.permanentOutput = *output;
    }
    return result;
}

bool ValidateFinalStats(
    const Settings& settings,
    const StatsSnapshot& baseline,
    const StatsSnapshot& finalStats,
    std::uint64_t permanentSets,
    double elapsedSeconds,
    std::string& failure)
{
    const auto camera0Read =
        Delta(finalStats.thread0.readFrames, baseline.thread0.readFrames);
    const auto camera1Read =
        Delta(finalStats.thread1.readFrames, baseline.thread1.readFrames);
    const auto camera0Timeouts =
        Delta(finalStats.thread0.readTimeouts, baseline.thread0.readTimeouts);
    const auto camera1Timeouts =
        Delta(finalStats.thread1.readTimeouts, baseline.thread1.readTimeouts);
    const auto camera0Errors =
        Delta(finalStats.thread0.readErrors, baseline.thread0.readErrors);
    const auto camera1Errors =
        Delta(finalStats.thread1.readErrors, baseline.thread1.readErrors);
    const auto camera0IngressDrops = Delta(
        finalStats.thread0.droppedOldestAndPushed,
        baseline.thread0.droppedOldestAndPushed);
    const auto camera1IngressDrops = Delta(
        finalStats.thread1.droppedOldestAndPushed,
        baseline.thread1.droppedOldestAndPushed);
    const auto camera0PushFailures =
        Delta(finalStats.thread0.pushFailures, baseline.thread0.pushFailures);
    const auto camera1PushFailures =
        Delta(finalStats.thread1.pushFailures, baseline.thread1.pushFailures);
    const auto pool0Exhaustion =
        Delta(finalStats.pool0.exhaustionDrops, baseline.pool0.exhaustionDrops);
    const auto pool1Exhaustion =
        Delta(finalStats.pool1.exhaustionDrops, baseline.pool1.exhaustionDrops);
    const auto pool0WaitTimeouts =
        Delta(finalStats.pool0.waitTimeouts, baseline.pool0.waitTimeouts);
    const auto pool1WaitTimeouts =
        Delta(finalStats.pool1.waitTimeouts, baseline.pool1.waitTimeouts);
    const auto syncInput =
        Delta(finalStats.sync.inputFrames, baseline.sync.inputFrames);
    const auto syncDropped =
        Delta(finalStats.sync.droppedFrames, baseline.sync.droppedFrames);
    const auto syncIncomplete =
        Delta(finalStats.sync.incompleteSets, baseline.sync.incompleteSets);
    const auto permanentDrops = Delta(
        finalStats.permanentOutput.queueDrops,
        baseline.permanentOutput.queueDrops);
    const auto permanentDispatchErrors = Delta(
        finalStats.permanentOutput.dispatchErrors,
        baseline.permanentOutput.dispatchErrors);
    const auto permanentClosedPushes = Delta(
        finalStats.permanentOutput.closedQueuePushes,
        baseline.permanentOutput.closedQueuePushes);

    if (camera0Errors || camera1Errors ||
        camera0PushFailures || camera1PushFailures ||
        camera0IngressDrops || camera1IngressDrops) {
        failure = "Camera read/push errors or ingress drops occurred during churn";
        return false;
    }
    if (camera0Timeouts || camera1Timeouts) {
        failure = "Camera read timeouts occurred during hardware-triggered churn";
        return false;
    }
    if (pool0Exhaustion || pool1Exhaustion ||
        pool0WaitTimeouts || pool1WaitTimeouts) {
        failure = "A CameraCapture FramePool was exhausted during churn";
        return false;
    }
    if (permanentDrops || permanentDispatchErrors || permanentClosedPushes) {
        failure = "Permanent output A recorded a drop or dispatch fault";
        return false;
    }

    const double dropRatio = syncInput == 0
        ? 0.0
        : static_cast<double>(syncDropped) / static_cast<double>(syncInput);
    if (!std::isfinite(dropRatio) || dropRatio > settings.maximumSyncDropRatio) {
        failure = "FrameSyncThread drop ratio exceeded the churn threshold";
        return false;
    }

    if (elapsedSeconds <= 0.0) {
        failure = "Dynamic churn measurement duration was invalid";
        return false;
    }
    const double permanentFps =
        static_cast<double>(permanentSets) / elapsedSeconds;
    const double requiredFps = settings.expectedFps * settings.minimumRateRatio;
    if (!std::isfinite(permanentFps) || permanentFps < requiredFps) {
        failure = "Permanent output A fell below the minimum delivery rate: " +
                  std::to_string(permanentFps) + " < " +
                  std::to_string(requiredFps);
        return false;
    }

    std::cout << std::fixed << std::setprecision(3)
              << "[dynamic-output-acceptance-stats]"
              << " permanentSets=" << permanentSets
              << " elapsedSec=" << elapsedSeconds
              << " permanentFps=" << permanentFps
              << " camera0Read=" << camera0Read
              << " camera1Read=" << camera1Read
              << " camera0Timeouts=" << camera0Timeouts
              << " camera1Timeouts=" << camera1Timeouts
              << " syncInput=" << syncInput
              << " syncDropped=" << syncDropped
              << " syncIncomplete=" << syncIncomplete
              << " permanentDrops=" << permanentDrops
              << " pool0Exhaustion=" << pool0Exhaustion
              << " pool1Exhaustion=" << pool1Exhaustion
              << '\n';
    return true;
}

} // namespace

int main()
{
    IC4ExtTest::CameraAccessCooldown cooldown;

    if (IC4ExtTest::EnvInt(
            "IC4EXT_TEST_ENABLE_DYNAMIC_OUTPUT_ACCEPTANCE", 0) == 0) {
        std::cerr
            << "Skipping manual dynamic-output hardware acceptance. Set "
               "IC4EXT_TEST_ENABLE_DYNAMIC_OUTPUT_ACCEPTANCE=1 to run it.\n";
        return 77;
    }

    const Settings settings = LoadSettings();
    std::cerr << "[dynamic-output-acceptance]"
              << " camera0Device=" << settings.camera0Device
              << " camera1Device=" << settings.camera1Device
              << " requestedFps=" << settings.requestedFps
              << " expectedFps=" << settings.expectedFps
              << " toleranceNs=" << settings.toleranceNs
              << " warmupSets=" << settings.warmupSets
              << " churnCycles=" << settings.churnCycles
              << " postStopPermanentSets=" << settings.postStopPermanentSets
              << " triggerSource=" << settings.triggerSource
              << '\n';

    if (settings.camera0Device < 0 || settings.camera1Device < 0 ||
        settings.camera0Device == settings.camera1Device) {
        std::cerr << "Two distinct non-negative device indices are required\n";
        return 1;
    }
    if (!IC4ExtTest::RequireCameraCount(2)) return 77;

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    try {
        core = D3D12CoreLib::D3D12Core::CreateShared();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Core creation failed; skipping test: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = Pipe::D3D12BackendContext::FromCore(core);
    if (!backend.resolve()) {
        std::cerr << "D3D12 backend resolve failed; skipping test\n";
        return 77;
    }

    ThreadKit::Queues::QueueOptions inputOptions;
    inputOptions.maxSize = settings.inputQueueCapacity;
    inputOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto inputQueue =
        std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(inputOptions);

    ThreadKit::Queues::QueueOptions permanentOptions;
    permanentOptions.maxSize = settings.permanentQueueCapacity;
    permanentOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto permanentQueue =
        std::make_shared<Pipe::ReadOnlyFrameSetQueue>(permanentOptions);

    Pipe::FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::HostReceived;
    syncConfig.maxTimestampDiffNs = settings.toleranceNs;
    syncConfig.maxBufferedFramesPerCamera = 64;
    syncConfig.groupTimeout = std::chrono::milliseconds(100);
    Pipe::FrameSyncThread sync(inputQueue, syncConfig);

    Pipe::FrameSyncOutputConfig permanentConfig;
    permanentConfig.requiredCameras = {0, 1};
    permanentConfig.frameRate = Pipe::FrameRateLimit::Maximum();
    permanentConfig.priority = 100;
    permanentConfig.enabled = true;

    const auto permanentId =
        sync.registerOutput(permanentQueue, permanentConfig);
    if (permanentId == Pipe::InvalidFrameSyncOutputId || !sync.start()) {
        const auto error = sync.lastError();
        std::cerr << "Permanent output or FrameSyncThread start failed: "
                  << error.where << ": " << error.message << '\n';
        return 1;
    }

    PermanentConsumerState permanentState;
    std::thread permanentConsumer(
        PermanentConsumerLoop,
        permanentQueue,
        std::ref(permanentState));

    auto config0 =
        IC4ExtTest::MakeCameraConfig("d3d12", settings.camera0Device);
    auto config1 =
        IC4ExtTest::MakeCameraConfig("d3d12", settings.camera1Device);
    config0.streamRequest.fps = settings.requestedFps;
    config1.streamRequest.fps = settings.requestedFps;
    config0.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config1.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config0.maxPendingBuffers = 64;
    config1.maxPendingBuffers = 64;
    IC4Ext::ConfigureHardwareTriggerSync(config0, settings.triggerSource);
    IC4Ext::ConfigureHardwareTriggerSync(config1, settings.triggerSource);

    Pipe::CameraCaptureOptions captureOptions;
    captureOptions.initialFramePoolCapacity = settings.initialFramePoolCapacity;
    captureOptions.maxFramePoolCapacity = settings.maxFramePoolCapacity;
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
        << "[dynamic-output-acceptance] starting two CameraCaptureThread "
           "instances through OpenAndStartMultiCameraGroup\n";
    auto started = Pipe::OpenAndStartMultiCameraGroup(
        backend,
        {},
        std::vector<Pipe::CameraCaptureThreadStartupConfig>{camera0, camera1},
        startupOptions);

    std::string failure;
    bool acquisitionsStopped = true;
    Pipe::FrameSyncOutputId currentDynamicId =
        Pipe::InvalidFrameSyncOutputId;
    std::shared_ptr<Pipe::ReadOnlyFrameSetQueue> currentDynamicQueue;

    if (!started || !started.captures.empty() ||
        started.captureThreads.size() != 2 ||
        !started.captureThreads[0] || !started.captureThreads[1]) {
        failure = started
            ? "Unexpected multi-camera startup result shape"
            : std::string("OpenAndStartMultiCameraGroup failed: ") +
                  started.error.where + ": " + started.error.message;
    }

    Pipe::CameraCaptureThread* thread0 =
        failure.empty() ? started.captureThreads[0].get() : nullptr;
    Pipe::CameraCaptureThread* thread1 =
        failure.empty() ? started.captureThreads[1].get() : nullptr;

    StatsSnapshot baseline;
    StatsSnapshot finalStats;
    std::uint64_t baselinePermanentSets = 0;
    std::uint64_t finalPermanentSets = 0;
    std::uint64_t dynamicEmittedBeforeStop = 0;
    std::uint64_t drainedSets = 0;
    std::uint64_t discardedSets = 0;
    Clock::time_point churnStart{};
    Clock::time_point churnEnd{};

    if (failure.empty()) {
        if (thread0->cameraId() != 0 || thread1->cameraId() != 1 ||
            !thread0->isRunning() || !thread1->isRunning()) {
            failure = "Unexpected logical camera IDs or worker state";
        }
    }

    if (failure.empty()) {
        std::cerr
            << "[dynamic-output-acceptance] cameras are armed. Start the "
               "external hardware trigger now\n";
        if (settings.triggerArmDelayMs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(settings.triggerArmDelayMs));
        }

        const auto warmupDeadline =
            Clock::now() + std::chrono::seconds(settings.startupTimeoutSeconds);
        if (!WaitForHealthyCondition(
                [&] {
                    return permanentState.sets.load(std::memory_order_relaxed) >=
                           static_cast<std::uint64_t>(settings.warmupSets);
                },
                permanentState,
                sync,
                *thread0,
                *thread1,
                warmupDeadline,
                failure)) {
            if (failure.empty()) failure = "Warmup did not complete";
        }
    }

    if (failure.empty()) {
        baseline = Snapshot(*thread0, *thread1, sync, permanentId);
        baselinePermanentSets =
            permanentState.sets.load(std::memory_order_relaxed);
        churnStart = Clock::now();
        const auto overallDeadline =
            churnStart + std::chrono::seconds(settings.totalTimeoutSeconds);

        for (int cycle = 0; cycle < settings.churnCycles; ++cycle) {
            if (Clock::now() >= overallDeadline) {
                failure = "Dynamic-output churn exceeded the overall timeout";
                break;
            }

            ThreadKit::Queues::QueueOptions dynamicOptions;
            dynamicOptions.maxSize = settings.dynamicQueueCapacity;
            dynamicOptions.overflowPolicy =
                ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
            currentDynamicQueue =
                std::make_shared<Pipe::ReadOnlyFrameSetQueue>(dynamicOptions);

            Pipe::FrameSyncOutputConfig dynamicConfig;
            dynamicConfig.requiredCameras = {0, 1};
            dynamicConfig.frameRate = Pipe::FrameRateLimit::Maximum();
            dynamicConfig.priority = 0;
            dynamicConfig.enabled = true;

            currentDynamicId =
                sync.registerOutput(currentDynamicQueue, dynamicConfig);
            if (currentDynamicId == Pipe::InvalidFrameSyncOutputId) {
                const auto error = sync.lastError();
                failure = std::string("Dynamic output register failed: ") +
                          error.where + ": " + error.message;
                break;
            }

            // Vary stop timing deliberately: immediate, then with 1/2/4 queued
            // sets. This repeatedly hits different register/dispatch boundaries.
            const int phase = cycle % 4;
            const std::size_t backlogTarget =
                phase == 0 ? 0u : static_cast<std::size_t>(1u << (phase - 1));
            if (backlogTarget > 0) {
                if (!WaitForHealthyCondition(
                        [&] { return currentDynamicQueue->size() >= backlogTarget; },
                        permanentState,
                        sync,
                        *thread0,
                        *thread1,
                        std::min(
                            overallDeadline,
                            Clock::now() + std::chrono::seconds(2)),
                        failure)) {
                    break;
                }
            }

            const auto permanentAtStop =
                permanentState.sets.load(std::memory_order_relaxed);
            if (!sync.stopOutputSupply(currentDynamicId)) {
                const auto error = sync.lastError();
                failure = std::string("stopOutputSupply failed: ") +
                          error.where + ": " + error.message;
                break;
            }
            if (sync.outputState(currentDynamicId) !=
                Pipe::FrameSyncOutputState::SupplyStopped) {
                failure = "Dynamic output did not enter SupplyStopped";
                break;
            }

            const auto stoppedStats = sync.outputStats(currentDynamicId);
            if (!stoppedStats) {
                failure = "Dynamic output stats disappeared before channel close";
                break;
            }
            if (stoppedStats->dispatchErrors != 0 ||
                stoppedStats->closedQueuePushes != 0) {
                failure = "Dynamic output faulted before normal supply stop";
                break;
            }
            const auto emittedAtStop = stoppedStats->emittedSets;
            const auto queueSizeAtStop = currentDynamicQueue->size();
            dynamicEmittedBeforeStop += emittedAtStop;

            // Keep camera/sync/permanent A running after the barrier. Neither
            // emittedSets nor queue size may increase after stop returns.
            if (!WaitForHealthyCondition(
                    [&] {
                        return permanentState.sets.load(std::memory_order_relaxed) >=
                               permanentAtStop +
                                   static_cast<std::uint64_t>(
                                       settings.postStopPermanentSets);
                    },
                    permanentState,
                    sync,
                    *thread0,
                    *thread1,
                    std::min(
                        overallDeadline,
                        Clock::now() + std::chrono::seconds(2)),
                    failure)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            const auto afterBarrier = sync.outputStats(currentDynamicId);
            if (!afterBarrier || afterBarrier->emittedSets != emittedAtStop ||
                currentDynamicQueue->size() != queueSizeAtStop) {
                failure = "Late push detected after stopOutputSupply returned";
                break;
            }

            if ((cycle % 2) == 0) {
                if (!sync.closeOutputChannel(currentDynamicId)) {
                    const auto error = sync.lastError();
                    failure = std::string("closeOutputChannel failed: ") +
                              error.where + ": " + error.message;
                    break;
                }
                const auto drained = currentDynamicQueue->tryPopAll();
                drainedSets += drained.size();
            } else {
                discardedSets += currentDynamicQueue->size();
                currentDynamicQueue->clear();
                if (!sync.closeOutputChannel(currentDynamicId)) {
                    const auto error = sync.lastError();
                    failure = std::string("closeOutputChannel failed: ") +
                              error.where + ": " + error.message;
                    break;
                }
            }

            if (!currentDynamicQueue->isClosed() ||
                sync.outputState(currentDynamicId).has_value() ||
                sync.outputStats(currentDynamicId).has_value()) {
                failure = "Closed dynamic output remained visible in the registry";
                break;
            }

            currentDynamicId = Pipe::InvalidFrameSyncOutputId;
            currentDynamicQueue.reset();

            if ((cycle + 1) % 25 == 0 || cycle + 1 == settings.churnCycles) {
                std::cerr
                    << "[dynamic-output-acceptance] cycles=" << (cycle + 1)
                    << "/" << settings.churnCycles
                    << " permanentSets="
                    << Delta(
                           permanentState.sets.load(std::memory_order_relaxed),
                           baselinePermanentSets)
                    << " dynamicEmittedBeforeStop=" << dynamicEmittedBeforeStop
                    << " drained=" << drainedSets
                    << " discarded=" << discardedSets
                    << '\n';
            }
        }

        churnEnd = Clock::now();
        finalPermanentSets =
            permanentState.sets.load(std::memory_order_relaxed);
        finalStats = Snapshot(*thread0, *thread1, sync, permanentId);

        if (failure.empty() && permanentState.failed.load(std::memory_order_relaxed)) {
            failure = PermanentFailure(permanentState);
        }
        if (failure.empty() &&
            (!sync.isRunning() || !thread0->isRunning() || !thread1->isRunning())) {
            failure = "A central worker stopped before churn completion";
        }
        if (failure.empty()) {
            const double elapsedSeconds =
                std::chrono::duration<double>(churnEnd - churnStart).count();
            const auto permanentMeasuredSets =
                Delta(finalPermanentSets, baselinePermanentSets);
            ValidateFinalStats(
                settings,
                baseline,
                finalStats,
                permanentMeasuredSets,
                elapsedSeconds,
                failure);
        }
    }

    // Best-effort cleanup for a cycle that failed between register and close.
    if (currentDynamicId != Pipe::InvalidFrameSyncOutputId) {
        sync.stopOutputSupply(currentDynamicId);
        sync.closeOutputChannel(currentDynamicId);
    }

    Pipe::FrameSyncOutputStats permanentFinalOutputStats{};
    if (const auto stats = sync.outputStats(permanentId)) {
        permanentFinalOutputStats = *stats;
    }
    sync.stopOutputSupply(permanentId);
    sync.closeOutputChannel(permanentId);
    if (permanentConsumer.joinable()) permanentConsumer.join();

    if (started) acquisitionsStopped = StopStartedGroup(started);
    sync.stopAndJoin();
    inputQueue->close();
    core->WaitIdle();

    if (failure.empty() && permanentState.failed.load(std::memory_order_relaxed)) {
        failure = PermanentFailure(permanentState);
    }
    if (failure.empty() && !acquisitionsStopped) {
        failure = "One or more AcquisitionStop commands failed";
    }

    if (!failure.empty()) {
        std::cerr << "[dynamic-output-acceptance] FAILED: " << failure << '\n';
        return 1;
    }

    std::cout << std::fixed << std::setprecision(3)
              << "[dynamic-output-acceptance-result]"
              << " cycles=" << settings.churnCycles
              << " permanentSets="
              << Delta(finalPermanentSets, baselinePermanentSets)
              << " dynamicEmittedBeforeStop=" << dynamicEmittedBeforeStop
              << " drained=" << drainedSets
              << " discarded=" << discardedSets
              << " permanentDrops="
              << Delta(
                     permanentFinalOutputStats.queueDrops,
                     baseline.permanentOutput.queueDrops)
              << " latePushes=0"
              << " syncRunningThroughChurn=1"
              << '\n';
    std::cout << "test_d3d12_dynamic_output_hardware_acceptance passed\n";
    return 0;
}
