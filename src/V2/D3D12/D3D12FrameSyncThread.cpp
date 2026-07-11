#include "IC4Ext/V2/D3D12/D3D12FrameSyncThread.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace IC4Ext::V2 {

namespace {

bool IsV2QueuePushSucceeded(ThreadKit::Queues::QueuePushResult result) noexcept
{
    return result == ThreadKit::Queues::QueuePushResult::Pushed ||
           result == ThreadKit::Queues::QueuePushResult::DroppedOldestAndPushed;
}

bool DidV2QueueDrop(ThreadKit::Queues::QueuePushResult result) noexcept
{
    return result != ThreadKit::Queues::QueuePushResult::Pushed;
}

std::uint64_t SteadyTimeNs(std::chrono::steady_clock::time_point value) noexcept
{
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
        value.time_since_epoch()).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

struct RateGate
{
    FrameRateLimit limit = FrameRateLimit::Maximum();
    bool initialized = false;
    std::uint64_t intervalNs = 0;
    std::uint64_t nextEmitTimestampNs = 0;

    explicit RateGate(FrameRateLimit value) noexcept : limit(value)
    {
        if (limit.mode == FrameRateMode::Fixed && limit.isValid()) {
            const double interval = 1'000'000'000.0 / limit.fps;
            intervalNs = static_cast<std::uint64_t>(std::max(1.0, std::round(interval)));
        }
    }

    bool shouldEmit(std::uint64_t timestampNs) noexcept
    {
        if (limit.mode == FrameRateMode::Maximum) return true;
        if (intervalNs == 0) return false;
        if (!initialized) {
            initialized = true;
            nextEmitTimestampNs = timestampNs > std::numeric_limits<std::uint64_t>::max() - intervalNs
                ? std::numeric_limits<std::uint64_t>::max()
                : timestampNs + intervalNs;
            return true;
        }
        if (timestampNs < nextEmitTimestampNs) return false;

        const std::uint64_t overdue = timestampNs - nextEmitTimestampNs;
        const std::uint64_t steps = overdue / intervalNs + 1;
        if (steps > (std::numeric_limits<std::uint64_t>::max() - nextEmitTimestampNs) / intervalNs) {
            nextEmitTimestampNs = std::numeric_limits<std::uint64_t>::max();
        } else {
            nextEmitTimestampNs += steps * intervalNs;
        }
        return true;
    }
};

struct OutputCounters
{
    std::atomic<std::uint64_t> consideredSets{0};
    std::atomic<std::uint64_t> skippedByFrameRate{0};
    std::atomic<std::uint64_t> emittedSets{0};
    std::atomic<std::uint64_t> queueDrops{0};
    std::atomic<std::uint64_t> disabledSkips{0};

    FrameSyncOutputStats snapshot() const noexcept
    {
        FrameSyncOutputStats result;
        result.consideredSets = consideredSets.load(std::memory_order_relaxed);
        result.skippedByFrameRate = skippedByFrameRate.load(std::memory_order_relaxed);
        result.emittedSets = emittedSets.load(std::memory_order_relaxed);
        result.queueDrops = queueDrops.load(std::memory_order_relaxed);
        result.disabledSkips = disabledSkips.load(std::memory_order_relaxed);
        return result;
    }
};

struct OutputEntry
{
    FrameSyncOutputId id = InvalidFrameSyncOutputId;
    std::uint64_t registrationOrder = 0;
    std::shared_ptr<D3D12ReadOnlyFrameSetQueue> queue;
    FrameSyncOutputConfig config;
    std::shared_ptr<OutputCounters> counters;
    RateGate rateGate{FrameRateLimit::Maximum()};

    OutputEntry(FrameSyncOutputId outputId,
                std::uint64_t order,
                std::shared_ptr<D3D12ReadOnlyFrameSetQueue> outputQueue,
                FrameSyncOutputConfig outputConfig,
                std::shared_ptr<OutputCounters> outputCounters)
        : id(outputId),
          registrationOrder(order),
          queue(std::move(outputQueue)),
          config(std::move(outputConfig)),
          counters(std::move(outputCounters)),
          rateGate(config.frameRate)
    {
    }
};

using OutputTable = std::vector<std::shared_ptr<OutputEntry>>;

void SortOutputTable(OutputTable& table)
{
    std::stable_sort(table.begin(), table.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs->config.priority != rhs->config.priority) {
            return lhs->config.priority > rhs->config.priority;
        }
        return lhs->registrationOrder < rhs->registrationOrder;
    });
}

} // namespace

class D3D12FrameSyncThread::Impl
{
public:
    Impl(std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> input, FrameSyncConfig syncConfig)
        : inputQueue(std::move(input)), configValue(std::move(syncConfig))
    {
        std::atomic_store_explicit(
            &outputTable,
            std::shared_ptr<const OutputTable>(std::make_shared<OutputTable>()),
            std::memory_order_release);
    }

    ~Impl()
    {
        stopAndJoin();
    }

    struct CameraBuffer
    {
        CameraId cameraId = 0;
        std::deque<D3D12IndexedReadOnlyCameraFrame> frames;
    };

    struct CompleteFrameSet
    {
        SyncGroupId syncGroupId = 0;
        std::uint64_t referenceTimestampNs = 0;
        std::chrono::steady_clock::time_point completedTime{};
        std::vector<D3D12IndexedReadOnlyFrame> frames;
    };

    bool start()
    {
        clearError();
        if (running.load(std::memory_order_acquire)) return true;
        if (!inputQueue) {
            setError(ErrorCode::InvalidArgument, "D3D12FrameSyncThread::start", "Input queue is null");
            return false;
        }
        if (!configValue.isValid()) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D12FrameSyncThread::start",
                "Invalid frame synchronization configuration");
            return false;
        }

        buffers.clear();
        buffers.reserve(configValue.cameraIds.size());
        for (CameraId cameraId : configValue.cameraIds) {
            buffers.push_back(CameraBuffer{cameraId, {}});
        }
        nextSyncGroupId = 1;
        stopRequested.store(false, std::memory_order_release);
        running.store(true, std::memory_order_release);
        try {
            worker = std::thread([this] { workerLoop(); });
        } catch (const std::exception& e) {
            running.store(false, std::memory_order_release);
            setError(ErrorCode::ThreadError, "D3D12FrameSyncThread::start", e.what());
            return false;
        }
        return true;
    }

    void requestStop() noexcept
    {
        stopRequested.store(true, std::memory_order_release);
    }

    void join() noexcept
    {
        if (worker.joinable()) worker.join();
        running.store(false, std::memory_order_release);
    }

    void stopAndJoin() noexcept
    {
        requestStop();
        join();
    }

    bool isRunning() const noexcept
    {
        return running.load(std::memory_order_acquire);
    }

    FrameSyncOutputId registerOutput(
        std::shared_ptr<D3D12ReadOnlyFrameSetQueue> queue,
        FrameSyncOutputConfig config)
    {
        if (!queue) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D12FrameSyncThread::registerOutput",
                "Output queue is null");
            return InvalidFrameSyncOutputId;
        }
        if (!validateOutputConfig(config, "D3D12FrameSyncThread::registerOutput")) {
            return InvalidFrameSyncOutputId;
        }

        std::lock_guard<std::mutex> lock(outputUpdateMutex);
        auto current = loadOutputTable();
        auto next = std::make_shared<OutputTable>(*current);
        const FrameSyncOutputId id = nextOutputId++;
        const std::uint64_t order = nextRegistrationOrder++;
        next->push_back(std::make_shared<OutputEntry>(
            id,
            order,
            std::move(queue),
            std::move(config),
            std::make_shared<OutputCounters>()));
        SortOutputTable(*next);
        storeOutputTable(std::move(next));
        clearError();
        return id;
    }

    bool updateOutput(FrameSyncOutputId id, FrameSyncOutputConfig config)
    {
        if (!validateOutputConfig(config, "D3D12FrameSyncThread::updateOutput")) {
            return false;
        }

        std::lock_guard<std::mutex> lock(outputUpdateMutex);
        auto current = loadOutputTable();
        auto next = std::make_shared<OutputTable>();
        next->reserve(current->size());
        bool found = false;
        for (const auto& entry : *current) {
            if (entry->id == id) {
                next->push_back(std::make_shared<OutputEntry>(
                    entry->id,
                    entry->registrationOrder,
                    entry->queue,
                    config,
                    entry->counters));
                found = true;
            } else {
                next->push_back(entry);
            }
        }
        if (!found) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D12FrameSyncThread::updateOutput",
                "Output ID was not found");
            return false;
        }
        SortOutputTable(*next);
        storeOutputTable(std::move(next));
        clearError();
        return true;
    }

    bool replaceOutputQueue(
        FrameSyncOutputId id,
        std::shared_ptr<D3D12ReadOnlyFrameSetQueue> queue)
    {
        if (!queue) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D12FrameSyncThread::replaceOutputQueue",
                "Output queue is null");
            return false;
        }

        std::lock_guard<std::mutex> lock(outputUpdateMutex);
        auto current = loadOutputTable();
        auto next = std::make_shared<OutputTable>();
        next->reserve(current->size());
        bool found = false;
        for (const auto& entry : *current) {
            if (entry->id == id) {
                next->push_back(std::make_shared<OutputEntry>(
                    entry->id,
                    entry->registrationOrder,
                    queue,
                    entry->config,
                    entry->counters));
                found = true;
            } else {
                next->push_back(entry);
            }
        }
        if (!found) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D12FrameSyncThread::replaceOutputQueue",
                "Output ID was not found");
            return false;
        }
        storeOutputTable(std::move(next));
        clearError();
        return true;
    }

    bool unregisterOutput(FrameSyncOutputId id)
    {
        std::lock_guard<std::mutex> lock(outputUpdateMutex);
        auto current = loadOutputTable();
        auto next = std::make_shared<OutputTable>();
        next->reserve(current->size());
        bool found = false;
        for (const auto& entry : *current) {
            if (entry->id == id) {
                found = true;
                continue;
            }
            next->push_back(entry);
        }
        if (!found) {
            setError(
                ErrorCode::InvalidArgument,
                "D3D12FrameSyncThread::unregisterOutput",
                "Output ID was not found");
            return false;
        }
        storeOutputTable(std::move(next));
        clearError();
        return true;
    }

    std::optional<FrameSyncOutputConfig> outputConfig(FrameSyncOutputId id) const
    {
        const auto table = loadOutputTable();
        for (const auto& entry : *table) {
            if (entry->id == id) return entry->config;
        }
        return std::nullopt;
    }

    std::vector<FrameSyncOutputInfo> outputs() const
    {
        std::vector<FrameSyncOutputInfo> result;
        const auto table = loadOutputTable();
        result.reserve(table->size());
        for (const auto& entry : *table) {
            result.push_back(FrameSyncOutputInfo{
                entry->id,
                entry->config,
                entry->registrationOrder});
        }
        return result;
    }

    std::optional<FrameSyncOutputStats> outputStats(FrameSyncOutputId id) const
    {
        const auto table = loadOutputTable();
        for (const auto& entry : *table) {
            if (entry->id == id) return entry->counters->snapshot();
        }
        return std::nullopt;
    }

    FrameSyncStats stats() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        return statsValue;
    }

    ErrorInfo lastError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return lastErrorValue;
    }

    std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> inputQueue;
    FrameSyncConfig configValue;

private:
    std::shared_ptr<const OutputTable> loadOutputTable() const
    {
        return std::atomic_load_explicit(&outputTable, std::memory_order_acquire);
    }

    void storeOutputTable(std::shared_ptr<OutputTable> table)
    {
        std::atomic_store_explicit(
            &outputTable,
            std::shared_ptr<const OutputTable>(std::move(table)),
            std::memory_order_release);
    }

    bool validateOutputConfig(const FrameSyncOutputConfig& config, const char* where)
    {
        if (config.requiredCameras.empty()) {
            setError(ErrorCode::InvalidArgument, where, "requiredCameras is empty");
            return false;
        }
        if (!config.frameRate.isValid()) {
            setError(ErrorCode::InvalidArgument, where, "Frame-rate limit is invalid");
            return false;
        }

        std::vector<CameraId> unique;
        unique.reserve(config.requiredCameras.size());
        for (CameraId cameraId : config.requiredCameras) {
            if (std::find(
                    configValue.cameraIds.begin(),
                    configValue.cameraIds.end(),
                    cameraId) == configValue.cameraIds.end()) {
                setError(
                    ErrorCode::InvalidArgument,
                    where,
                    "requiredCameras contains a camera outside the synchronization domain");
                return false;
            }
            if (std::find(unique.begin(), unique.end(), cameraId) != unique.end()) {
                setError(
                    ErrorCode::InvalidArgument,
                    where,
                    "requiredCameras contains a duplicate camera ID");
                return false;
            }
            unique.push_back(cameraId);
        }
        return true;
    }

    void setError(ErrorCode code, const char* where, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastErrorValue = MakeError(code, where, message);
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastErrorValue = NoError();
    }

    void workerLoop() noexcept
    {
        try {
            while (!stopRequested.load(std::memory_order_acquire)) {
                auto item = inputQueue->waitPopFor(std::chrono::milliseconds(100));
                if (!item) {
                    expireTimedOutFrames();
                    continue;
                }
                incrementStat([](FrameSyncStats& value) { ++value.inputFrames; });
                handleInputFrame(std::move(*item));
            }
        } catch (const std::exception& e) {
            setError(ErrorCode::ThreadError, "D3D12FrameSyncThread::workerLoop", e.what());
        } catch (...) {
            setError(
                ErrorCode::ThreadError,
                "D3D12FrameSyncThread::workerLoop",
                "Unknown worker exception");
        }
        running.store(false, std::memory_order_release);
    }

    void handleInputFrame(D3D12IndexedReadOnlyCameraFrame&& frame)
    {
        CameraBuffer* buffer = findBuffer(frame.cameraId);
        if (!buffer) {
            incrementStat([](FrameSyncStats& value) { ++value.ignoredFrames; });
            return;
        }
        buffer->frames.push_back(std::move(frame));
        trimBuffer(*buffer);
        expireTimedOutFrames();
        tryEmitCompleteSets();
    }

    CameraBuffer* findBuffer(CameraId cameraId) noexcept
    {
        for (auto& buffer : buffers) {
            if (buffer.cameraId == cameraId) return &buffer;
        }
        return nullptr;
    }

    bool allBuffersHaveFrames() const noexcept
    {
        if (buffers.empty()) return false;
        for (const auto& buffer : buffers) {
            if (buffer.frames.empty()) return false;
        }
        return true;
    }

    void trimBuffer(CameraBuffer& buffer)
    {
        while (buffer.frames.size() > configValue.maxBufferedFramesPerCamera) {
            buffer.frames.pop_front();
            incrementStat([](FrameSyncStats& value) {
                ++value.droppedFrames;
                ++value.incompleteSets;
            });
        }
    }

    void expireTimedOutFrames()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto& buffer : buffers) {
            while (!buffer.frames.empty()) {
                const auto received = buffer.frames.front().frame.timing().hostReceivedTime;
                if (received == std::chrono::steady_clock::time_point{} ||
                    now - received < configValue.groupTimeout) {
                    break;
                }
                buffer.frames.pop_front();
                incrementStat([](FrameSyncStats& value) {
                    ++value.droppedFrames;
                    ++value.incompleteSets;
                });
            }
        }
    }

    void dropFront(CameraBuffer& buffer)
    {
        if (buffer.frames.empty()) return;
        buffer.frames.pop_front();
        incrementStat([](FrameSyncStats& value) { ++value.droppedFrames; });
    }

    void tryEmitCompleteSets()
    {
        if (configValue.policy == FrameSyncPolicy::FrameNumberExact) {
            tryEmitFrameNumberExactSets();
        } else {
            tryEmitTimestampNearestSets();
        }
    }

    void tryEmitFrameNumberExactSets()
    {
        while (allBuffersHaveFrames()) {
            std::uint64_t target = 0;
            for (const auto& buffer : buffers) {
                target = std::max(target, buffer.frames.front().frame.timing().frameNumber);
            }

            bool dropped = false;
            for (auto& buffer : buffers) {
                while (!buffer.frames.empty() &&
                       buffer.frames.front().frame.timing().frameNumber < target) {
                    dropFront(buffer);
                    dropped = true;
                }
            }
            if (!allBuffersHaveFrames()) return;
            if (dropped) continue;

            bool allMatch = true;
            for (const auto& buffer : buffers) {
                if (buffer.frames.front().frame.timing().frameNumber != target) {
                    allMatch = false;
                    break;
                }
            }
            if (!allMatch) continue;
            emitFrontSet();
        }
    }

    void tryEmitTimestampNearestSets()
    {
        while (allBuffersHaveFrames()) {
            std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t maximum = 0;
            CameraBuffer* minimumBuffer = nullptr;
            for (auto& buffer : buffers) {
                const std::uint64_t timestamp = syncTimestampNs(buffer.frames.front());
                if (timestamp < minimum) {
                    minimum = timestamp;
                    minimumBuffer = &buffer;
                }
                maximum = std::max(maximum, timestamp);
            }
            if (maximum >= minimum && maximum - minimum <= configValue.maxTimestampDiffNs) {
                emitFrontSet();
                continue;
            }
            if (!minimumBuffer) return;
            dropFront(*minimumBuffer);
        }
    }

    void emitFrontSet()
    {
        CompleteFrameSet complete;
        complete.syncGroupId = nextSyncGroupId++;
        complete.completedTime = std::chrono::steady_clock::now();
        complete.frames.reserve(buffers.size());

        std::uint64_t reference = 0;
        for (auto& buffer : buffers) {
            reference = std::max(reference, syncTimestampNs(buffer.frames.front()));
            complete.frames.push_back(D3D12IndexedReadOnlyFrame{
                buffer.cameraId,
                std::move(buffer.frames.front().frame)});
            buffer.frames.pop_front();
        }
        if (reference == 0) reference = SteadyTimeNs(complete.completedTime);
        complete.referenceTimestampNs = reference;
        incrementStat([](FrameSyncStats& value) { ++value.completedSets; });
        dispatchCompleteSet(complete);
    }

    void dispatchCompleteSet(const CompleteFrameSet& complete)
    {
        const auto table = loadOutputTable();
        for (const auto& output : *table) {
            output->counters->consideredSets.fetch_add(1, std::memory_order_relaxed);
            if (!output->config.enabled) {
                output->counters->disabledSkips.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!output->rateGate.shouldEmit(complete.referenceTimestampNs)) {
                output->counters->skippedByFrameRate.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            D3D12ReadOnlyFrameSet::FrameList selected;
            selected.reserve(output->config.requiredCameras.size());
            for (CameraId cameraId : output->config.requiredCameras) {
                const auto found = std::find_if(
                    complete.frames.begin(),
                    complete.frames.end(),
                    [cameraId](const auto& indexed) { return indexed.cameraId == cameraId; });
                if (found != complete.frames.end()) selected.push_back(*found);
            }

            auto frameSet = D3D12ReadOnlyFrameSet::Create(
                complete.syncGroupId,
                complete.referenceTimestampNs,
                complete.completedTime,
                std::move(selected));
            const auto result = output->queue->push(std::move(frameSet));
            const bool succeeded = IsV2QueuePushSucceeded(result);
            const bool dropped = DidV2QueueDrop(result);
            if (succeeded) {
                output->counters->emittedSets.fetch_add(1, std::memory_order_relaxed);
                incrementStat([](FrameSyncStats& value) { ++value.totalOutputSets; });
            }
            if (dropped) {
                output->counters->queueDrops.fetch_add(1, std::memory_order_relaxed);
                incrementStat([](FrameSyncStats& value) { ++value.totalOutputQueueDrops; });
            }
        }
    }

    std::uint64_t hostTimestampNs(const D3D12IndexedReadOnlyCameraFrame& frame) const noexcept
    {
        return SteadyTimeNs(frame.frame.timing().hostReceivedTime);
    }

    std::uint64_t syncTimestampNs(const D3D12IndexedReadOnlyCameraFrame& frame) const noexcept
    {
        const std::uint64_t host = hostTimestampNs(frame);
        const std::uint64_t device = frame.frame.timing().deviceTimestampNs;
        switch (configValue.timestampSource) {
        case FrameSyncTimestampSource::HostReceived:
            return host;
        case FrameSyncTimestampSource::Device:
            return device;
        case FrameSyncTimestampSource::Auto:
        default:
            return device != 0 ? device : host;
        }
    }

    template<class Function>
    void incrementStat(Function&& function)
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        function(statsValue);
    }

    std::vector<CameraBuffer> buffers;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    std::thread worker;
    SyncGroupId nextSyncGroupId = 1;

    mutable std::mutex outputUpdateMutex;
    std::shared_ptr<const OutputTable> outputTable;
    FrameSyncOutputId nextOutputId = 1;
    std::uint64_t nextRegistrationOrder = 1;

    mutable std::mutex statsMutex;
    FrameSyncStats statsValue;

    mutable std::mutex errorMutex;
    ErrorInfo lastErrorValue;
};

D3D12FrameSyncThread::D3D12FrameSyncThread(
    std::shared_ptr<D3D12IndexedReadOnlyFrameQueue> inputQueue,
    FrameSyncConfig config)
    : impl_(std::make_unique<Impl>(std::move(inputQueue), std::move(config)))
{
}

D3D12FrameSyncThread::~D3D12FrameSyncThread() = default;

bool D3D12FrameSyncThread::start() { return impl_->start(); }
void D3D12FrameSyncThread::requestStop() { impl_->requestStop(); }
void D3D12FrameSyncThread::join() { impl_->join(); }
void D3D12FrameSyncThread::stopAndJoin() { impl_->stopAndJoin(); }
bool D3D12FrameSyncThread::isRunning() const noexcept { return impl_->isRunning(); }

FrameSyncOutputId D3D12FrameSyncThread::registerOutput(
    std::shared_ptr<D3D12ReadOnlyFrameSetQueue> outputQueue,
    FrameSyncOutputConfig config)
{
    return impl_->registerOutput(std::move(outputQueue), std::move(config));
}

bool D3D12FrameSyncThread::updateOutput(
    FrameSyncOutputId outputId,
    FrameSyncOutputConfig config)
{
    return impl_->updateOutput(outputId, std::move(config));
}

bool D3D12FrameSyncThread::replaceOutputQueue(
    FrameSyncOutputId outputId,
    std::shared_ptr<D3D12ReadOnlyFrameSetQueue> outputQueue)
{
    return impl_->replaceOutputQueue(outputId, std::move(outputQueue));
}

bool D3D12FrameSyncThread::unregisterOutput(FrameSyncOutputId outputId)
{
    return impl_->unregisterOutput(outputId);
}

std::optional<FrameSyncOutputConfig> D3D12FrameSyncThread::outputConfig(
    FrameSyncOutputId outputId) const
{
    return impl_->outputConfig(outputId);
}

std::vector<FrameSyncOutputInfo> D3D12FrameSyncThread::outputs() const
{
    return impl_->outputs();
}

std::optional<FrameSyncOutputStats> D3D12FrameSyncThread::outputStats(
    FrameSyncOutputId outputId) const
{
    return impl_->outputStats(outputId);
}

const FrameSyncConfig& D3D12FrameSyncThread::config() const noexcept
{
    return impl_->configValue;
}

FrameSyncStats D3D12FrameSyncThread::stats() const
{
    return impl_->stats();
}

ErrorInfo D3D12FrameSyncThread::lastError() const
{
    return impl_->lastError();
}

} // namespace IC4Ext::V2
