#pragma once

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <IC4Ext/D3D12/D3D12BackendContext.hpp>
#include <IC4Ext/Core/Error.hpp>

#include <cstdint>
#include <memory>

namespace IC4ExtStress {

struct SobelProcessorStats
{
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t failures = 0;
};

// Sample-specific HLSL compute workload. The source ReadOnlyFrame is never
// modified. Sobel edge output is written to a private FramePool texture.
class SobelProcessor final
{
public:
    SobelProcessor();
    ~SobelProcessor();

    SobelProcessor(const SobelProcessor&) = delete;
    SobelProcessor& operator=(const SobelProcessor&) = delete;

    bool initialize(const IC4Ext::D3D12BackendContext& producerBackend);
    bool process(const IC4Ext::D3D12::ReadOnlyFrame& input);
    bool flush(std::uint32_t timeoutMs = 10'000) noexcept;

    SobelProcessorStats stats() const noexcept;
    IC4Ext::ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace IC4ExtStress
