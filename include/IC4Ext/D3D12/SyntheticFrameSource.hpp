#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/FramePool.hpp"
#include "IC4Ext/D3D12/ReadOnlyFrameSource.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

namespace IC4Ext::D3D12 {

enum class SyntheticFramePattern : std::uint32_t
{
    HashNoise = 0,
    Gradient = 1,
    Checkerboard = 2,
    FrameCounterBars = 3,
};

struct SyntheticFrameSourceConfig
{
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    double fps = 60.0;

    SyntheticFramePattern pattern = SyntheticFramePattern::HashNoise;
    std::uint64_t seed = 1;

    // Device timestamps are deterministic and independent from host scheduling.
    // This lets multiple synthetic sources emulate a hardware-synchronized camera
    // group while still applying a controlled per-source offset.
    std::uint64_t firstFrameNumber = 1;
    std::uint64_t deviceTimestampOriginNs = 1'000'000'000ull;
    std::int64_t deviceTimestampOffsetNs = 0;

    // Zero means unlimited. A finite limit is useful for deterministic tests.
    std::uint64_t frameLimit = 0;

    std::size_t initialFramePoolCapacity = 8;
    std::size_t maxFramePoolCapacity = 32;
    FramePoolExhaustionPolicy framePoolExhaustionPolicy =
        FramePoolExhaustionPolicy::DropNewest;
    std::chrono::milliseconds framePoolWaitTimeout{5};

    // Maximum CPU wait used before reusing one of the internal command slots.
    std::uint32_t gpuWaitTimeoutMs = 5000;

    bool isValid() const noexcept
    {
        const double period = 1'000'000'000.0 / fps;
        const double maximumPeriod =
            static_cast<double>(std::numeric_limits<std::uint64_t>::max() - 1ull);
        return width > 0 && height > 0 &&
               width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
               height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
               std::isfinite(fps) && fps > 0.0 && std::isfinite(period) &&
               period >= 1.0 && period <= maximumPeriod && firstFrameNumber != 0 &&
               deviceTimestampOriginNs != 0 && initialFramePoolCapacity > 0 &&
               maxFramePoolCapacity >= initialFramePoolCapacity &&
               framePoolWaitTimeout.count() >= 0 && gpuWaitTimeoutMs > 0 &&
               static_cast<std::uint32_t>(pattern) <=
                   static_cast<std::uint32_t>(SyntheticFramePattern::FrameCounterBars);
    }

    std::uint64_t framePeriodNs() const noexcept
    {
        if (!std::isfinite(fps) || fps <= 0.0) return 0;
        const double period = 1'000'000'000.0 / fps;
        const double maximumPeriod =
            static_cast<double>(std::numeric_limits<std::uint64_t>::max() - 1ull);
        if (!std::isfinite(period) || period < 1.0 || period > maximumPeriod) {
            return 0;
        }
        return static_cast<std::uint64_t>(period + 0.5);
    }
};

struct SyntheticFrameSourceStats
{
    std::uint64_t generatedFrames = 0;
    std::uint64_t readTimeouts = 0;
    std::uint64_t poolAcquireFailures = 0;
    std::uint64_t gpuGenerationFailures = 0;
    std::uint64_t lateFrames = 0;
    std::uint64_t lastFrameNumber = 0;
    std::uint64_t lastDeviceTimestampNs = 0;
    FramePoolStats framePool;
};

// Camera-free producer of deterministic RGBA8 GPU frames.
//
// Each read is paced to config.fps, fills a FramePool-owned Texture2D through a
// compute shader, signals a producer fence, and publishes the texture as a
// normal ReadOnlyFrame. The class is intended to be injected into
// CameraCaptureThread through the ReadOnlyFrameSource constructor.
class SyntheticFrameSource final : public ReadOnlyFrameSource
{
public:
    SyntheticFrameSource();
    ~SyntheticFrameSource() override;

    SyntheticFrameSource(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource& operator=(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource(SyntheticFrameSource&&) = delete;
    SyntheticFrameSource& operator=(SyntheticFrameSource&&) = delete;

    bool initialize(D3D12BackendContext backend,
                    SyntheticFrameSourceConfig config = {});
    void close() noexcept;

    bool isOpened() const noexcept override;

    bool read(const CameraReadOptions& options,
              D3D12ReadOnlyFrame& outFrame,
              ErrorInfo& outError) override;

    SyntheticFrameSourceConfig config() const;
    SyntheticFrameSourceStats stats() const;
    FramePoolStats framePoolStats() const;
    ErrorInfo lastError() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

using SyntheticReadOnlyFrameSource = SyntheticFrameSource;
using SyntheticD3D12ReadOnlyFrameSource = SyntheticFrameSource;

} // namespace IC4Ext::D3D12
