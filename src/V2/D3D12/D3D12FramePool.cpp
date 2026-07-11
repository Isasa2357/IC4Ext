#include "IC4Ext/V2/D3D12/D3D12FramePool.hpp"

#include "D3D12ReadOnlyFrameStorage.hpp"

#include <D3D12Helper/D3D12Framework/D3D12DescriptorHeap.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>

#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace IC4Ext::V2 {

namespace {

constexpr std::size_t InvalidEntryIndex = std::numeric_limits<std::size_t>::max();

enum class EntryState : std::uint32_t
{
    Available = 0,
    Writing = 1,
    Published = 2,
};

struct PoolEntry
{
    D3D12CoreLib::D3D12Resource texture;
    D3D12CoreLib::D3D12DescriptorHeap descriptorHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu{};
    EntryState state = EntryState::Available;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    std::uint64_t generation = 0;
};

} // namespace

struct D3D12FramePoolState
{
    mutable std::mutex mutex;
    std::condition_variable cv;
    D3D12BackendContext backend;
    D3D12FramePoolConfig config;
    std::vector<std::unique_ptr<PoolEntry>> entries;
    bool initialized = false;
    ErrorInfo lastError;
    std::uint64_t acquisitions = 0;
    std::uint64_t dynamicAllocations = 0;
    std::uint64_t exhaustionDrops = 0;
    std::uint64_t waitTimeouts = 0;

    void setError(ErrorCode code, const char* where, const std::string& message)
    {
        lastError = MakeError(code, where, message);
    }

    bool createEntryLocked(bool dynamic)
    {
        try {
            auto entry = std::make_unique<PoolEntry>();
            entry->texture = D3D12CoreLib::CreateTexture2D(
                *backend.corePtr,
                config.width,
                config.height,
                config.format,
                config.writeState,
                config.resourceFlags);
            entry->currentState = config.writeState;

            const UINT descriptorCount = static_cast<UINT>(
                (config.createSrv ? 1 : 0) + (config.createUav ? 1 : 0));
            entry->descriptorHeap.Initialize(
                backend.device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                descriptorCount,
                true);

            UINT descriptorIndex = 0;
            if (config.createSrv) {
                const auto handle = entry->descriptorHeap.GetHandle(descriptorIndex++);
                D3D12CoreLib::CreateTexture2DSrv(
                    *backend.corePtr,
                    entry->texture,
                    handle.cpu,
                    config.format);
                entry->srvCpu = handle.cpu;
                entry->srvGpu = handle.gpu;
            }
            if (config.createUav) {
                const auto handle = entry->descriptorHeap.GetHandle(descriptorIndex);
                D3D12CoreLib::CreateTexture2DUav(
                    *backend.corePtr,
                    entry->texture,
                    handle.cpu,
                    config.format);
                entry->uavCpu = handle.cpu;
                entry->uavGpu = handle.gpu;
            }

            entries.push_back(std::move(entry));
            if (dynamic) {
                ++dynamicAllocations;
            }
            return true;
        } catch (const std::exception& e) {
            setError(ErrorCode::D3D12Error, "D3D12FramePool::createEntry", e.what());
            return false;
        }
    }

    std::size_t findAvailableLocked() const noexcept
    {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i]->state == EntryState::Available) {
                return i;
            }
        }
        return InvalidEntryIndex;
    }

    void releaseWriting(std::size_t index, std::uint64_t generation) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= entries.size()) return;
        auto& entry = *entries[index];
        if (entry.generation != generation || entry.state != EntryState::Writing) return;
        entry.state = EntryState::Available;
        cv.notify_one();
    }

    void releasePublished(std::size_t index, std::uint64_t generation) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= entries.size()) return;
        auto& entry = *entries[index];
        if (entry.generation != generation || entry.state != EntryState::Published) return;
        entry.state = EntryState::Available;
        cv.notify_one();
    }
};

D3D12FrameWriter::D3D12FrameWriter(std::shared_ptr<D3D12FramePoolState> state,
                                   std::size_t entryIndex,
                                   std::uint64_t leaseGeneration) noexcept
    : state_(std::move(state)), entryIndex_(entryIndex), leaseGeneration_(leaseGeneration)
{
}

D3D12FrameWriter::~D3D12FrameWriter()
{
    cancel();
}

D3D12FrameWriter::D3D12FrameWriter(D3D12FrameWriter&& other) noexcept
    : state_(std::move(other.state_)),
      entryIndex_(other.entryIndex_),
      leaseGeneration_(other.leaseGeneration_),
      published_(other.published_)
{
    other.entryIndex_ = InvalidEntryIndex;
    other.leaseGeneration_ = 0;
    other.published_ = true;
}

D3D12FrameWriter& D3D12FrameWriter::operator=(D3D12FrameWriter&& other) noexcept
{
    if (this == &other) return *this;
    cancel();
    state_ = std::move(other.state_);
    entryIndex_ = other.entryIndex_;
    leaseGeneration_ = other.leaseGeneration_;
    published_ = other.published_;
    other.entryIndex_ = InvalidEntryIndex;
    other.leaseGeneration_ = 0;
    other.published_ = true;
    return *this;
}

bool D3D12FrameWriter::valid() const noexcept
{
    return state_ && entryIndex_ != InvalidEntryIndex && !published_;
}

ID3D12Resource* D3D12FrameWriter::resource() const noexcept
{
    if (!valid()) return nullptr;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (entryIndex_ >= state_->entries.size()) return nullptr;
    const auto& entry = *state_->entries[entryIndex_];
    return entry.generation == leaseGeneration_ ? entry.texture.Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12FrameWriter::srvCpuHandle() const noexcept
{
    if (!valid()) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return entryIndex_ < state_->entries.size()
        ? state_->entries[entryIndex_]->srvCpu
        : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12FrameWriter::srvGpuHandle() const noexcept
{
    if (!valid()) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return entryIndex_ < state_->entries.size()
        ? state_->entries[entryIndex_]->srvGpu
        : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12FrameWriter::uavCpuHandle() const noexcept
{
    if (!valid()) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return entryIndex_ < state_->entries.size()
        ? state_->entries[entryIndex_]->uavCpu
        : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12FrameWriter::uavGpuHandle() const noexcept
{
    if (!valid()) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return entryIndex_ < state_->entries.size()
        ? state_->entries[entryIndex_]->uavGpu
        : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

DXGI_FORMAT D3D12FrameWriter::dxgiFormat() const noexcept
{
    return state_ ? state_->config.format : DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_STATES D3D12FrameWriter::initialState() const noexcept
{
    if (!valid()) return D3D12_RESOURCE_STATE_COMMON;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (entryIndex_ >= state_->entries.size()) return D3D12_RESOURCE_STATE_COMMON;
    const auto& entry = *state_->entries[entryIndex_];
    return entry.generation == leaseGeneration_
        ? entry.currentState
        : D3D12_RESOURCE_STATE_COMMON;
}

D3D12_RESOURCE_STATES D3D12FrameWriter::writeState() const noexcept
{
    return state_ ? state_->config.writeState : D3D12_RESOURCE_STATE_COMMON;
}

D3D12_RESOURCE_STATES D3D12FrameWriter::publishedState() const noexcept
{
    return state_ ? state_->config.publishedState : D3D12_RESOURCE_STATE_COMMON;
}

D3D12ReadOnlyFrame D3D12FrameWriter::publish(const D3D12ReadyToken& ready,
                                             FrameTiming timing,
                                             FrameFormatMetadata format,
                                             FrameChunkMetadata chunkMetadata)
{
    if (!valid()) return {};

    std::shared_ptr<D3D12ReadOnlyFrame::Storage> storage;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (entryIndex_ >= state_->entries.size()) return {};
        auto& entry = *state_->entries[entryIndex_];
        if (entry.generation != leaseGeneration_ || entry.state != EntryState::Writing) {
            return {};
        }

        storage = std::make_shared<D3D12ReadOnlyFrame::Storage>();
        storage->texture = entry.texture.Get();
        storage->srvHeap = entry.descriptorHeap.Get();
        storage->srvCpu = entry.srvCpu;
        storage->srvGpu = entry.srvGpu;
        storage->formatValue = state_->config.format;
        storage->publishedStateValue = state_->config.publishedState;
        storage->ready = ready;
        storage->timingValue = std::move(timing);
        storage->frameFormat = std::move(format);
        storage->chunk = std::move(chunkMetadata);

        const auto state = state_;
        const auto index = entryIndex_;
        const auto generation = leaseGeneration_;
        storage->releaseCallback = [state, index, generation]() noexcept {
            state->releasePublished(index, generation);
        };

        entry.currentState = state_->config.publishedState;
        entry.state = EntryState::Published;
    }

    published_ = true;
    entryIndex_ = InvalidEntryIndex;
    leaseGeneration_ = 0;
    state_.reset();
    return D3D12ReadOnlyFrame(std::move(storage));
}

void D3D12FrameWriter::cancel() noexcept
{
    if (valid()) {
        state_->releaseWriting(entryIndex_, leaseGeneration_);
    }
    state_.reset();
    entryIndex_ = InvalidEntryIndex;
    leaseGeneration_ = 0;
    published_ = true;
}

D3D12FramePool::~D3D12FramePool()
{
    reset();
}

bool D3D12FramePool::initialize(D3D12BackendContext backend, D3D12FramePoolConfig config)
{
    reset();
    if (!config.isValid()) {
        state_ = std::make_shared<D3D12FramePoolState>();
        state_->setError(
            ErrorCode::InvalidArgument,
            "D3D12FramePool::initialize",
            "Invalid frame pool configuration");
        return false;
    }
    if (!backend.resolve()) {
        state_ = std::make_shared<D3D12FramePoolState>();
        state_->setError(
            ErrorCode::InvalidArgument,
            "D3D12FramePool::initialize",
            "D3D12 backend context is incomplete");
        return false;
    }

    auto state = std::make_shared<D3D12FramePoolState>();
    state->backend = std::move(backend);
    state->config = std::move(config);
    state->entries.reserve(state->config.maxCapacity);

    std::lock_guard<std::mutex> lock(state->mutex);
    for (std::size_t i = 0; i < state->config.initialCapacity; ++i) {
        if (!state->createEntryLocked(false)) {
            state_ = std::move(state);
            return false;
        }
    }
    state->initialized = true;
    state->lastError = NoError();
    state_ = std::move(state);
    return true;
}

void D3D12FramePool::reset() noexcept
{
    state_.reset();
}

bool D3D12FramePool::isInitialized() const noexcept
{
    return state_ && state_->initialized;
}

const D3D12FramePoolConfig& D3D12FramePool::config() const noexcept
{
    static const D3D12FramePoolConfig empty{};
    return state_ ? state_->config : empty;
}

D3D12FrameWriter D3D12FramePool::acquire()
{
    if (!state_ || !state_->initialized) return {};

    std::unique_lock<std::mutex> lock(state_->mutex);
    std::size_t index = state_->findAvailableLocked();
    if (index == InvalidEntryIndex && state_->entries.size() < state_->config.maxCapacity) {
        if (state_->createEntryLocked(true)) {
            index = state_->entries.size() - 1;
        }
    }

    if (index == InvalidEntryIndex &&
        state_->config.exhaustionPolicy == FramePoolExhaustionPolicy::WaitWithTimeout) {
        const bool available = state_->cv.wait_for(lock, state_->config.waitTimeout, [&] {
            return state_->findAvailableLocked() != InvalidEntryIndex;
        });
        if (available) {
            index = state_->findAvailableLocked();
        } else {
            ++state_->waitTimeouts;
        }
    }

    if (index == InvalidEntryIndex) {
        ++state_->exhaustionDrops;
        state_->setError(
            ErrorCode::Timeout,
            "D3D12FramePool::acquire",
            "No frame pool entry is available");
        return {};
    }

    auto& entry = *state_->entries[index];
    entry.state = EntryState::Writing;
    ++entry.generation;
    ++state_->acquisitions;
    state_->lastError = NoError();
    return D3D12FrameWriter(state_, index, entry.generation);
}

D3D12FramePoolStats D3D12FramePool::stats() const
{
    D3D12FramePoolStats result;
    if (!state_) return result;

    std::lock_guard<std::mutex> lock(state_->mutex);
    result.capacity = state_->entries.size();
    result.acquisitions = state_->acquisitions;
    result.dynamicAllocations = state_->dynamicAllocations;
    result.exhaustionDrops = state_->exhaustionDrops;
    result.waitTimeouts = state_->waitTimeouts;
    for (const auto& entry : state_->entries) {
        switch (entry->state) {
        case EntryState::Available: ++result.available; break;
        case EntryState::Writing: ++result.writing; break;
        case EntryState::Published: ++result.published; break;
        }
    }
    return result;
}

ErrorInfo D3D12FramePool::lastError() const
{
    if (!state_) return NoError();
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->lastError;
}

} // namespace IC4Ext::V2
