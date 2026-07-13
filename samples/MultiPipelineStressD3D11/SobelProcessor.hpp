#pragma once

#include <IC4Ext/Core/Error.hpp>
#include <IC4Ext/D3D11/D3D11BackendContext.hpp>
#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>

#include <cstdint>
#include <memory>

namespace IC4ExtStressD3D11 {

struct SobelProcessorStats
{
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t failures = 0;
};

// Sample-only D3D11 compute workload. It reads the shared camera texture through
// an SRV and writes Sobel output to a private FramePool texture.
class SobelProcessor final
{
public:
    SobelProcessor();
    ~SobelProcessor();

    SobelProcessor(const SobelProcessor&) = delete;
    SobelProcessor& operator=(const SobelProcessor&) = delete;

    bool initialize(const IC4Ext::D3D11BackendContext& producerBackend);
    bool process(const IC4Ext::D3D11::ReadOnlyFrame& input);
    bool flush(std::uint32_t timeoutMs = 10'000) noexcept;

    SobelProcessorStats stats() const noexcept;
    IC4Ext::ErrorInfo lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace IC4ExtStressD3D11
