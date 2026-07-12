#include "IC4Ext/D3D11/FramePool.hpp"

#include "ReadOnlyFrameStorage.hpp"

#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace IC4Ext::D3D11 {
namespace {

constexpr std::size_t InvalidEntryIndex =
    std::numeric_limits<std::size_t>::max();

enum class EntryState : std::uint32_t
{
    Available = 0,
    Writing = 1,
    Published = 2,
};

std::string HrText(HRESULT value)
{
    std::ostringstream stream;
    stream << "HRESULT=0x" << std::hex << static_cast<unsigned long>(value);
    return stream.str();
}

struct PoolEntry
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    EntryState state = EntryState::Available;
    std::uint64_t generation = 0;
};

} // namespace

struct D3D11FramePoolState
{
    mutable std::mutex mutex;
    std::condition_variable cv;
    D3D11BackendContext backend;
    D3D11FramePoolConfig config;
    std::vector<std::unique_ptr<PoolEntry>> entries;
    bool initialized = false;
    ErrorInfo lastError;
    std::uint64_t acquisitions = 0;
    std::uint64_t dynamicAllocations = 0;
    std::uint64_t exhaustionDrops = 0;
    std::uint64_t waitTimeouts = 0;

    void setError(ErrorCode code, const char* where, std::string message)
    {
        lastError = MakeError(code, where, std::move(message));
    }

    bool createEntryLocked(bool dynamic)
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = config.width;
        description.Height = config.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = config.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = 0;
        if (config.createSrv) description.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        if (config.createUav) description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

        auto entry = std::make_unique<PoolEntry>();
        HRESULT result = backend.device->CreateTexture2D(
            &description,
            nullptr,
            &entry->texture);
        if (FAILED(result)) {
            setError(
                ErrorCode::D3D11Error,
                "D3D11FramePool::createEntry/CreateTexture2D",
                HrText(result));
            return false;
        }

        if (config.createSrv) {
            result = backend.device->CreateShaderResourceView(
                entry->texture.Get(),
                nullptr,
                &entry->srv);
            if (FAILED(result)) {
                setError(
                    ErrorCode::D3D11Error,
                    "D3D11FramePool::createEntry/CreateShaderResourceView",
                    HrText(result));
                return false;
            }
        }

        if (config.createUav) {
            result = backend.device->CreateUnorderedAccessView(
                entry->texture.Get(),
                nullptr,
                &entry->uav);
            if (FAILED(result)) {
                setError(
                    ErrorCode::D3D11Error,
                    "D3D11FramePool::createEntry/CreateUnorderedAccessView",
                    HrText(result));
                return false;
            }
        }

        entries.push_back(std::move(entry));
        if (dynamic) ++dynamicAllocations;
        return true;
    }

    std::size_t findAvailableLocked() const noexcept
    {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index]->state == EntryState::Available) return index;
        }
        return InvalidEntryIndex;
    }

    void releaseWriting(std::size_t index, std::uint64_t generation) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= entries.size()) return;
        auto& entry = *entries[index];
        if (entry.generation != generation || entry.state != EntryState::Writing) {
            return;
        }
        entry.state = EntryState::Available;
        cv.notify_one();
    }

    void releasePublished(std::size_t index, std::uint64_t generation) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= entries.size()) return;
        auto& entry = *entries[index];
        if (entry.generation != generation || entry.state != EntryState::Published) {
            return;
        }
        entry.state = EntryState::Available;
        cv.notify_one();
    }
};

D3D11FrameWriter::D3D11FrameWriter(
    std::shared_ptr<D3D11FramePoolState> state,
    std::size_t entryIndex,
    std::uint64_t leaseGeneration)
    : state_(std::move(state)),
      entryIndex_(entryIndex),
      leaseGeneration_(leaseGeneration)
{
    if (state_ && state_->backend.immediateContextMutex) {
        contextLock_ = std::unique_lock<std::recursive_mutex>(
            *state_->backend.immediateContextMutex);
    }
}

D3D11FrameWriter::~D3D11FrameWriter()
{
    cancel();
}

D3D11FrameWriter::D3D11FrameWriter(D3D11FrameWriter&& other) noexcept
    : state_(std::move(other.state_)),
      entryIndex_(other.entryIndex_),
      leaseGeneration_(other.leaseGeneration_),
      published_(other.published_),
      contextLock_(std::move(other.contextLock_))
{
    other.entryIndex_ = InvalidEntryIndex;
    other.leaseGeneration_ = 0;
    other.published_ = true;
}

D3D11FrameWriter& D3D11FrameWriter::operator=(D3D11FrameWriter&& other) noexcept
{
    if (this == &other) return *this;
    cancel();
    state_ = std::move(other.state_);
    entryIndex_ = other.entryIndex_;
    leaseGeneration_ = other.leaseGeneration_;
    published_ = other.published_;
    contextLock_ = std::move(other.contextLock_);
    other.entryIndex_ = InvalidEntryIndex;
    other.leaseGeneration_ = 0;
    other.published_ = true;
    return *this;
}

bool D3D11FrameWriter::valid() const noexcept
{
    return state_ && entryIndex_ != InvalidEntryIndex && !published_;
}

ID3D11Texture2D* D3D11FrameWriter::texture() const noexcept
{
    if (!valid()) return nullptr;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (entryIndex_ >= state_->entries.size()) return nullptr;
    const auto& entry = *state_->entries[entryIndex_];
    return entry.generation == leaseGeneration_ ? entry.texture.Get() : nullptr;
}

ID3D11ShaderResourceView* D3D11FrameWriter::srv() const noexcept
{
    if (!valid()) return nullptr;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (entryIndex_ >= state_->entries.size()) return nullptr;
    const auto& entry = *state_->entries[entryIndex_];
    return entry.generation == leaseGeneration_ ? entry.srv.Get() : nullptr;
}

ID3D11UnorderedAccessView* D3D11FrameWriter::uav() const noexcept
{
    if (!valid()) return nullptr;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (entryIndex_ >= state_->entries.size()) return nullptr;
    const auto& entry = *state_->entries[entryIndex_];
    return entry.generation == leaseGeneration_ ? entry.uav.Get() : nullptr;
}

DXGI_FORMAT D3D11FrameWriter::dxgiFormat() const noexcept
{
    return state_ ? state_->config.format : DXGI_FORMAT_UNKNOWN;
}

D3D11ReadOnlyFrame D3D11FrameWriter::publish(
    const ::IC4Ext::D3D11ReadyToken& ready,
    FrameTiming timing,
    FrameFormatMetadata format,
    FrameChunkMetadata chunkMetadata)
{
    if (!valid()) return {};

    std::shared_ptr<D3D11ReadOnlyFrame::Storage> storage;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (entryIndex_ >= state_->entries.size()) return {};
        auto& entry = *state_->entries[entryIndex_];
        if (entry.generation != leaseGeneration_ ||
            entry.state != EntryState::Writing) {
            return {};
        }

        storage = std::make_shared<D3D11ReadOnlyFrame::Storage>();
        storage->texture = entry.texture;
        storage->srv = entry.srv;
        storage->uav = entry.uav;
        storage->formatValue = state_->config.format;
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

        entry.state = EntryState::Published;
    }

    published_ = true;
    entryIndex_ = InvalidEntryIndex;
    leaseGeneration_ = 0;
    state_.reset();

    // contextLock_ intentionally remains owned until this writer object leaves
    // scope. Callers may still have scoped D3D11 bindings whose destructors must
    // restore context state before another producer transaction can begin.
    return D3D11ReadOnlyFrame(std::move(storage));
}

void D3D11FrameWriter::cancel() noexcept
{
    if (valid()) state_->releaseWriting(entryIndex_, leaseGeneration_);
    state_.reset();
    entryIndex_ = InvalidEntryIndex;
    leaseGeneration_ = 0;
    published_ = true;
    if (contextLock_.owns_lock()) contextLock_.unlock();
}

D3D11FramePool::~D3D11FramePool()
{
    reset();
}

bool D3D11FramePool::initialize(
    D3D11BackendContext backend,
    D3D11FramePoolConfig config)
{
    reset();
    auto state = std::make_shared<D3D11FramePoolState>();
    state->config = config;

    if (!config.isValid()) {
        state->setError(
            ErrorCode::InvalidArgument,
            "D3D11FramePool::initialize",
            "Invalid frame pool configuration");
        state_ = std::move(state);
        return false;
    }
    if (!backend.resolve()) {
        state->setError(
            ErrorCode::InvalidArgument,
            "D3D11FramePool::initialize",
            "D3D11 backend context is incomplete");
        state_ = std::move(state);
        return false;
    }

    state->backend = std::move(backend);
    state->entries.reserve(config.maxCapacity);
    std::lock_guard<std::mutex> lock(state->mutex);
    for (std::size_t index = 0; index < config.initialCapacity; ++index) {
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

void D3D11FramePool::reset() noexcept
{
    state_.reset();
}

bool D3D11FramePool::isInitialized() const noexcept
{
    return state_ && state_->initialized;
}

const D3D11FramePoolConfig& D3D11FramePool::config() const noexcept
{
    static const D3D11FramePoolConfig empty{};
    return state_ ? state_->config : empty;
}

D3D11FrameWriter D3D11FramePool::acquire()
{
    if (!state_ || !state_->initialized) return {};

    std::unique_lock<std::mutex> lock(state_->mutex);
    std::size_t index = state_->findAvailableLocked();
    if (index == InvalidEntryIndex &&
        state_->entries.size() < state_->config.maxCapacity) {
        if (state_->createEntryLocked(true)) index = state_->entries.size() - 1;
    }

    if (index == InvalidEntryIndex &&
        state_->config.exhaustionPolicy ==
            FramePoolExhaustionPolicy::WaitWithTimeout) {
        const bool available = state_->cv.wait_for(
            lock,
            state_->config.waitTimeout,
            [&] { return state_->findAvailableLocked() != InvalidEntryIndex; });
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
            "D3D11FramePool::acquire",
            "No frame pool entry is available");
        return {};
    }

    auto& entry = *state_->entries[index];
    entry.state = EntryState::Writing;
    ++entry.generation;
    ++state_->acquisitions;
    state_->lastError = NoError();

    const auto generation = entry.generation;
    const auto state = state_;
    lock.unlock();

    try {
        return D3D11FrameWriter(state, index, generation);
    } catch (const std::exception& exception) {
        state->releaseWriting(index, generation);
        std::lock_guard<std::mutex> stateLock(state->mutex);
        state->setError(
            ErrorCode::ThreadError,
            "D3D11FramePool::acquire/contextLock",
            exception.what());
        return {};
    }
}

D3D11FramePoolStats D3D11FramePool::stats() const
{
    D3D11FramePoolStats result;
    if (!state_) return result;

    std::lock_guard<std::mutex> lock(state_->mutex);
    result.capacity = state_->entries.size();
    result.maxCapacity = state_->config.maxCapacity;
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

std::size_t D3D11FramePool::capacity() const { return stats().capacity; }
std::size_t D3D11FramePool::availableCount() const { return stats().available; }
std::size_t D3D11FramePool::inFlightCount() const { return stats().inFlight(); }
bool D3D11FramePool::hasAvailableFrame() const { return stats().available != 0; }

ErrorInfo D3D11FramePool::lastError() const
{
    if (!state_) return NoError();
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->lastError;
}

} // namespace IC4Ext::D3D11
