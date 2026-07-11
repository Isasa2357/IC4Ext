#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12ReadyToken.hpp"
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrame.hpp"

#include <d3d12.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace IC4Ext::V2 {

struct D3D12FramePoolState;

enum class FramePoolExhaustionPolicy : std::uint32_t
{
    DropNewest = 0,
    WaitWithTimeout = 1,
};

struct D3D12FramePoolConfig
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES publishedState = D3D12_RESOURCE_STATE_GENERIC_READ;

    // A published v2 frame is always an SRV-capable read-only frame.
    // createSrv therefore must remain true. createUav is optional so future
    // producers can use COPY_DEST or render-target based write paths.
    bool createSrv = true;
    bool createUav = true;

    std::size_t initialCapacity = 8;
    std::size_t maxCapacity = 32;
    FramePoolExhaustionPolicy exhaustionPolicy = FramePoolExhaustionPolicy::DropNewest;
    std::chrono::milliseconds waitTimeout{5};

    bool isValid() const noexcept
    {
        return width > 0 && height > 0 && format != DXGI_FORMAT_UNKNOWN &&
               initialCapacity > 0 && maxCapacity >= initialCapacity &&
               createSrv && waitTimeout.count() >= 0;
    }
};

struct D3D12FramePoolStats
{
    std::size_t capacity = 0;
    std::size_t available = 0;
    std::size_t writing = 0;
    std::size_t published = 0;
    std::uint64_t acquisitions = 0;
    std::uint64_t dynamicAllocations = 0;
    std::uint64_t exhaustionDrops = 0;
    std::uint64_t waitTimeouts = 0;
};

class D3D12FrameWriter final
{
public:
    D3D12FrameWriter() noexcept = default;
    ~D3D12FrameWriter();

    D3D12FrameWriter(const D3D12FrameWriter&) = delete;
    D3D12FrameWriter& operator=(const D3D12FrameWriter&) = delete;

    D3D12FrameWriter(D3D12FrameWriter&& other) noexcept;
    D3D12FrameWriter& operator=(D3D12FrameWriter&& other) noexcept;

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    ID3D12Resource* resource() const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle() const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle() const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle() const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpuHandle() const noexcept;
    DXGI_FORMAT dxgiFormat() const noexcept;

    // initialState() is the state left by the previous publication of this pool
    // entry. The producer must transition initialState() -> writeState() before
    // writing and writeState() -> publishedState() before publish().
    D3D12_RESOURCE_STATES initialState() const noexcept;
    D3D12_RESOURCE_STATES writeState() const noexcept;
    D3D12_RESOURCE_STATES publishedState() const noexcept;

    D3D12ReadOnlyFrame publish(const D3D12ReadyToken& ready,
                               FrameTiming timing,
                               FrameFormatMetadata format,
                               FrameChunkMetadata chunkMetadata = {});

    void cancel() noexcept;

private:
    D3D12FrameWriter(std::shared_ptr<D3D12FramePoolState> state,
                     std::size_t entryIndex,
                     std::uint64_t leaseGeneration) noexcept;

    std::shared_ptr<D3D12FramePoolState> state_;
    std::size_t entryIndex_ = static_cast<std::size_t>(-1);
    std::uint64_t leaseGeneration_ = 0;
    bool published_ = false;

    friend class D3D12FramePool;
};

class D3D12FramePool final
{
public:
    D3D12FramePool() noexcept = default;
    ~D3D12FramePool();

    D3D12FramePool(const D3D12FramePool&) = delete;
    D3D12FramePool& operator=(const D3D12FramePool&) = delete;

    D3D12FramePool(D3D12FramePool&&) noexcept = default;
    D3D12FramePool& operator=(D3D12FramePool&&) noexcept = default;

    bool initialize(D3D12BackendContext backend, D3D12FramePoolConfig config);
    void reset() noexcept;

    bool isInitialized() const noexcept;
    const D3D12FramePoolConfig& config() const noexcept;

    D3D12FrameWriter acquire();

    D3D12FramePoolStats stats() const;
    ErrorInfo lastError() const;

private:
    std::shared_ptr<D3D12FramePoolState> state_;
};

} // namespace IC4Ext::V2
