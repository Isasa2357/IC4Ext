#pragma once

#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11BackendContext.hpp"
#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"
#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"

#include <d3d11.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace IC4Ext::D3D11 {

struct D3D11FramePoolState;

enum class FramePoolExhaustionPolicy : std::uint32_t
{
    DropNewest = 0,
    WaitWithTimeout = 1,
};

struct D3D11FramePoolConfig
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool createSrv = true;
    bool createUav = true;
    std::size_t initialCapacity = 8;
    std::size_t maxCapacity = 32;
    FramePoolExhaustionPolicy exhaustionPolicy =
        FramePoolExhaustionPolicy::DropNewest;
    std::chrono::milliseconds waitTimeout{5};

    bool isValid() const noexcept
    {
        return width > 0 && height > 0 && format != DXGI_FORMAT_UNKNOWN &&
               createSrv && initialCapacity > 0 &&
               maxCapacity >= initialCapacity && waitTimeout.count() >= 0;
    }
};

struct D3D11FramePoolStats
{
    std::size_t capacity = 0;
    std::size_t maxCapacity = 0;
    std::size_t available = 0;
    std::size_t writing = 0;
    std::size_t published = 0;
    std::uint64_t acquisitions = 0;
    std::uint64_t dynamicAllocations = 0;
    std::uint64_t exhaustionDrops = 0;
    std::uint64_t waitTimeouts = 0;

    std::size_t inFlight() const noexcept { return writing + published; }
    bool exhausted() const noexcept
    {
        return maxCapacity != 0 && available == 0 && capacity >= maxCapacity;
    }
    double availableRatio() const noexcept
    {
        return capacity == 0
            ? 0.0
            : static_cast<double>(available) / static_cast<double>(capacity);
    }
    double inFlightRatio() const noexcept
    {
        return capacity == 0
            ? 0.0
            : static_cast<double>(inFlight()) / static_cast<double>(capacity);
    }
};

class D3D11FrameWriter final
{
public:
    D3D11FrameWriter() noexcept = default;
    ~D3D11FrameWriter();

    D3D11FrameWriter(const D3D11FrameWriter&) = delete;
    D3D11FrameWriter& operator=(const D3D11FrameWriter&) = delete;
    D3D11FrameWriter(D3D11FrameWriter&& other) noexcept;
    D3D11FrameWriter& operator=(D3D11FrameWriter&& other) noexcept;

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    ID3D11Texture2D* texture() const noexcept;
    ID3D11ShaderResourceView* srv() const noexcept;
    ID3D11UnorderedAccessView* uav() const noexcept;
    DXGI_FORMAT dxgiFormat() const noexcept;

    D3D11ReadOnlyFrame publish(
        const ::IC4Ext::D3D11ReadyToken& ready,
        FrameTiming timing,
        FrameFormatMetadata format,
        FrameChunkMetadata chunkMetadata = {});

    void cancel() noexcept;

private:
    D3D11FrameWriter(
        std::shared_ptr<D3D11FramePoolState> state,
        std::size_t entryIndex,
        std::uint64_t leaseGeneration) noexcept;

    std::shared_ptr<D3D11FramePoolState> state_;
    std::size_t entryIndex_ = static_cast<std::size_t>(-1);
    std::uint64_t leaseGeneration_ = 0;
    bool published_ = false;

    friend class D3D11FramePool;
};

class D3D11FramePool final
{
public:
    D3D11FramePool() noexcept = default;
    ~D3D11FramePool();

    D3D11FramePool(const D3D11FramePool&) = delete;
    D3D11FramePool& operator=(const D3D11FramePool&) = delete;
    D3D11FramePool(D3D11FramePool&&) noexcept = default;
    D3D11FramePool& operator=(D3D11FramePool&&) noexcept = default;

    bool initialize(D3D11BackendContext backend, D3D11FramePoolConfig config);
    void reset() noexcept;

    bool isInitialized() const noexcept;
    const D3D11FramePoolConfig& config() const noexcept;
    D3D11FrameWriter acquire();

    D3D11FramePoolStats stats() const;
    std::size_t capacity() const;
    std::size_t availableCount() const;
    std::size_t inFlightCount() const;
    bool hasAvailableFrame() const;
    ErrorInfo lastError() const;

private:
    std::shared_ptr<D3D11FramePoolState> state_;
};

using FramePoolConfig = D3D11FramePoolConfig;
using FramePoolStats = D3D11FramePoolStats;
using FrameWriter = D3D11FrameWriter;
using FramePool = D3D11FramePool;

} // namespace IC4Ext::D3D11
