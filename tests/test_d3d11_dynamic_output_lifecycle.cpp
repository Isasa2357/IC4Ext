#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Core/D3D11CoreConfig.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>

namespace {

namespace Pipe = IC4Ext::D3D11;
using Clock = std::chrono::steady_clock;

std::shared_ptr<Pipe::ReadOnlyFrameSetQueue> MakeOutputQueue(
    std::size_t capacity = 64)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = capacity;
    options.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    return std::make_shared<Pipe::ReadOnlyFrameSetQueue>(options);
}

template<class Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

bool ConsumePermanentSets(
    const std::shared_ptr<Pipe::ReadOnlyFrameSetQueue>& queue,
    std::size_t count,
    std::chrono::seconds timeout = std::chrono::seconds(5))
{
    const auto deadline = Clock::now() + timeout;
    std::size_t consumed = 0;
    while (consumed < count && Clock::now() < deadline) {
        auto set = queue->waitPopFor(std::chrono::milliseconds(250));
        if (!set) continue;
        if (set->size() != 2 || !set->contains(0) || !set->contains(1)) {
            return false;
        }
        ++consumed;
    }
    return consumed == count;
}

} // namespace

int main()
{
    std::shared_ptr<D3D11CoreLib::D3D11Core> core;
    try {
        D3D11CoreLib::D3D11CoreConfig coreConfig;
        coreConfig.enableDebugLayer = false;
        coreConfig.enableInfoQueue = false;
        coreConfig.enableMultithreadProtection = true;
        coreConfig.allowWarpAdapter = true;
        core = D3D11CoreLib::D3D11Core::CreateShared(coreConfig);
    } catch (const std::exception& exception) {
        std::cerr << "D3D11 core creation failed; skipping: "
                  << exception.what() << '\n';
        return 77;
    }

    auto backend = IC4Ext::D3D11BackendContext::FromCore(core, true);
    if (!backend.resolve()) {
        std::cerr << "D3D11 backend resolve failed; skipping\n";
        return 77;
    }

    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 48;
    constexpr double fps = 240.0;
    constexpr std::uint64_t timestampOffsetNs = 100'000;
    constexpr std::uint64_t frameLimit = 50'000;
    constexpr int churnCycles = 200;

    Pipe::SyntheticFrameSourceConfig sourceConfig0;
    sourceConfig0.width = width;
    sourceConfig0.height = height;
    sourceConfig0.fps = fps;
    sourceConfig0.pattern = Pipe::SyntheticFramePattern::HashNoise;
    sourceConfig0.seed = 0x0123456789abcdefull;
    sourceConfig0.deviceTimestampOriginNs = 10'000'000'000ull;
    sourceConfig0.deviceTimestampOffsetNs = 0;
    sourceConfig0.frameLimit = frameLimit;
    sourceConfig0.initialFramePoolCapacity = 64;
    sourceConfig0.maxFramePoolCapacity = 256;

    auto sourceConfig1 = sourceConfig0;
    sourceConfig1.seed = 0xfedcba9876543210ull;
    sourceConfig1.deviceTimestampOffsetNs = timestampOffsetNs;

    auto source0 = std::make_shared<Pipe::SyntheticFrameSource>();
    auto source1 = std::make_shared<Pipe::SyntheticFrameSource>();
    if (!source0->initialize(backend, sourceConfig0) ||
        !source1->initialize(backend, sourceConfig1)) {
        std::cerr << "Synthetic source initialization failed; skipping\n";
        return 77;
    }

    ThreadKit::Queues::QueueOptions ingressOptions;
    ingressOptions.maxSize = 1024;
    ingressOptions.overflowPolicy =
        ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(
        ingressOptions);

    Pipe::FrameSyncConfig syncConfig;
    syncConfig.cameraIds = {0, 1};
    syncConfig.timestampSource = Pipe::FrameSyncTimestampSource::Device;
    syncConfig.maxTimestampDiffNs = timestampOffsetNs + 1;
    syncConfig.maxBufferedFramesPerCamera = 64;
    syncConfig.groupTimeout = std::chrono::seconds(5);

    Pipe::FrameSyncThread sync(ingress, syncConfig);

    Pipe::FrameSyncOutputConfig permanentConfig;
    permanentConfig.requiredCameras = {0, 1};
    permanentConfig.frameRate = Pipe::FrameRateLimit::Maximum();
    permanentConfig.priority = 100;

    auto permanentQueue = MakeOutputQueue(1024);
    const auto permanentId =
        sync.registerOutput(permanentQueue, permanentConfig);
    assert(permanentId != Pipe::InvalidFrameSyncOutputId);
    assert(sync.outputState(permanentId) == Pipe::FrameSyncOutputState::Active);

    // Closing is intentionally not a one-step removal operation.
    assert(!sync.closeOutputChannel(permanentId));

    // One communication path may belong to only one output ID.
    assert(sync.registerOutput(permanentQueue, permanentConfig) ==
           Pipe::InvalidFrameSyncOutputId);

    Pipe::CameraCaptureThreadOptions threadOptions;
    threadOptions.readTimeoutMs = 20;
    threadOptions.stopOnReadError = false;

    Pipe::CameraCaptureThread camera0(0, source0, threadOptions);
    Pipe::CameraCaptureThread camera1(1, source1, threadOptions);
    camera0.setOutputQueue(ingress);
    camera1.setOutputQueue(ingress);

    assert(sync.start());
    assert(camera0.start());
    assert(camera1.start());
    assert(ConsumePermanentSets(permanentQueue, 10));

    std::uint64_t drainedAfterClose = 0;
    std::uint64_t discardedBeforeClose = 0;

    for (int cycle = 0; cycle < churnCycles; ++cycle) {
        auto dynamicQueue = MakeOutputQueue();
        Pipe::FrameSyncOutputConfig dynamicConfig;
        dynamicConfig.requiredCameras = {0, 1};
        dynamicConfig.frameRate = Pipe::FrameRateLimit::Maximum();
        dynamicConfig.priority = 0;

        const auto dynamicId =
            sync.registerOutput(dynamicQueue, dynamicConfig);
        assert(dynamicId != Pipe::InvalidFrameSyncOutputId);
        assert(sync.outputState(dynamicId) ==
               Pipe::FrameSyncOutputState::Active);

        const bool waitForBacklog = (cycle % 3) != 0;
        if (waitForBacklog) {
            assert(WaitUntil(
                [&dynamicQueue] { return dynamicQueue->size() >= 2; },
                std::chrono::seconds(2)));
        }

        assert(sync.stopOutputSupply(dynamicId));
        // Supply stop is idempotent and terminal.
        assert(sync.stopOutputSupply(dynamicId));
        assert(sync.outputState(dynamicId) ==
               Pipe::FrameSyncOutputState::SupplyStopped);

        const auto stoppedStats = sync.outputStats(dynamicId);
        assert(stoppedStats.has_value());
        const auto emittedAtStop = stoppedStats->emittedSets;

        // Updating a retired output would be an implicit replacement/revival.
        assert(!sync.updateOutput(dynamicId, dynamicConfig));

        // Keep the producer and permanent output active. A hard supply barrier
        // guarantees that the retired output's emitted count cannot increase.
        assert(ConsumePermanentSets(permanentQueue, 4));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto afterBarrier = sync.outputStats(dynamicId);
        assert(afterBarrier.has_value());
        assert(afterBarrier->emittedSets == emittedAtStop);
        assert(sync.isRunning());

        if ((cycle % 2) == 0) {
            // Drain policy: close first, then consume everything already queued.
            assert(sync.closeOutputChannel(dynamicId));
            assert(dynamicQueue->isClosed());
            const auto queued = dynamicQueue->tryPopAll();
            drainedAfterClose += queued.size();
            assert(!dynamicQueue->waitPopFor(std::chrono::milliseconds(1)));
        } else {
            // Discard policy: clear the backlog, then close the communication path.
            discardedBeforeClose += dynamicQueue->size();
            dynamicQueue->clear();
            assert(sync.closeOutputChannel(dynamicId));
            assert(dynamicQueue->isClosed());
            assert(!dynamicQueue->tryPop());
        }

        assert(!sync.outputState(dynamicId).has_value());
        assert(!sync.outputStats(dynamicId).has_value());
        assert(!sync.closeOutputChannel(dynamicId));
        assert(sync.isRunning());
    }

    // A consumer that closes its queue out of order must fault only that output.
    auto faultQueue = MakeOutputQueue();
    Pipe::FrameSyncOutputConfig faultConfig = permanentConfig;
    faultConfig.priority = -100;
    const auto faultId = sync.registerOutput(faultQueue, faultConfig);
    assert(faultId != Pipe::InvalidFrameSyncOutputId);
    faultQueue->close();
    assert(ConsumePermanentSets(permanentQueue, 8));
    assert(WaitUntil(
        [&sync, faultId] {
            return sync.outputState(faultId) ==
                   Pipe::FrameSyncOutputState::Faulted;
        },
        std::chrono::seconds(2)));
    const auto faultStats = sync.outputStats(faultId);
    assert(faultStats.has_value());
    assert(faultStats->closedQueuePushes >= 1);
    assert(faultStats->dispatchErrors >= 1);
    assert(sync.outputLastError(faultId).has_value());
    assert(sync.isRunning());
    assert(sync.stopOutputSupply(faultId));
    assert(sync.closeOutputChannel(faultId));

    // Closed channels cannot be published again as a new output.
    assert(sync.registerOutput(faultQueue, faultConfig) ==
           Pipe::InvalidFrameSyncOutputId);

    assert(sync.stopOutputSupply(permanentId));
    const auto permanentStats = sync.outputStats(permanentId);
    assert(permanentStats.has_value());
    assert(permanentStats->emittedSets > 0);
    assert(permanentStats->dispatchErrors == 0);
    assert(sync.closeOutputChannel(permanentId));
    assert(permanentQueue->isClosed());

    camera0.stopAndJoin();
    camera1.stopAndJoin();
    sync.stopAndJoin();
    ingress->close();

    const auto camera0Stats = camera0.stats();
    const auto camera1Stats = camera1.stats();
    assert(camera0Stats.readErrors == 0);
    assert(camera1Stats.readErrors == 0);
    assert(camera0Stats.pushFailures == 0);
    assert(camera1Stats.pushFailures == 0);

    const auto syncStats = sync.stats();
    assert(syncStats.completedSets > 0);

    std::cout << "test_d3d11_dynamic_output_lifecycle passed"
              << " cycles=" << churnCycles
              << " drained=" << drainedAfterClose
              << " discarded=" << discardedBeforeClose
              << " permanentSets=" << permanentStats->emittedSets
              << '\n';
    return 0;
}
