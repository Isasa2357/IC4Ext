#pragma once

#include <IC4Ext/D3D11/D3D11BackendContext.hpp>
#include <IC4Ext/D3D11/D3D11FrameReadback.hpp>
#include <IC4Ext/D3D11/ReadOnlyPipeline.hpp>

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace IC4ExtStressD3D11 {

using Clock = std::chrono::steady_clock;

struct PipelineSnapshot
{
    std::uint64_t received = 0;
    std::uint64_t processed = 0;
    std::uint64_t failures = 0;
    std::uint64_t readbackFailures = 0;
    std::uint64_t outputFailures = 0;
    std::uint64_t checksum = 0;
    double averageProcessMs = 0.0;
    double maximumProcessMs = 0.0;
};

class PipelineMetrics final
{
public:
    void reset() noexcept;
    void recordReceived() noexcept;
    void recordSuccess(
        std::chrono::nanoseconds elapsed,
        std::uint64_t checksumContribution = 0) noexcept;
    void recordFailure(bool readbackFailure, bool outputFailure) noexcept;
    PipelineSnapshot snapshot() const noexcept;

private:
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> readbackFailures_{0};
    std::atomic<std::uint64_t> outputFailures_{0};
    std::atomic<std::uint64_t> checksum_{0};
    std::atomic<std::uint64_t> totalProcessNs_{0};
    std::atomic<std::uint64_t> maximumProcessNs_{0};
};

class FatalState final
{
public:
    void set(std::string message);
    bool triggered() const noexcept { return triggered_.load(); }
    std::string message() const;

private:
    std::atomic<bool> triggered_{false};
    mutable std::mutex mutex_;
    std::string message_;
};

class DisplaySlot final
{
public:
    void publish(cv::Mat image);
    bool snapshot(cv::Mat& outImage, std::uint64_t& inOutSequence) const;
    std::uint64_t publishedCount() const noexcept;

private:
    mutable std::mutex mutex_;
    cv::Mat image_;
    std::uint64_t sequence_ = 0;
};

// Every CPU/display/video worker owns its own DedicatedReadback instance. The
// D3D11 immediate context is shared, but IC4Ext serializes multi-call context
// transactions and each instance owns an independent staging-texture cache.
class DedicatedReadback final
{
public:
    bool initialize(const IC4Ext::D3D11BackendContext& producerBackend);
    bool readBgr(
        const IC4Ext::D3D11::ReadOnlyFrame& frame,
        IC4Ext::CpuFrame& outFrame,
        std::uint32_t timeoutMs);

    const IC4Ext::ErrorInfo& lastError() const noexcept { return error_; }
    IC4Ext::FrameReadbackCacheStats cacheStats() const noexcept;

private:
    IC4Ext::D3D11FrameReadback readback_;
    IC4Ext::ErrorInfo error_;
    bool initialized_ = false;
};

cv::Mat CpuFrameToBgr(const IC4Ext::CpuFrame& frame);
cv::Mat JoinHorizontal(const cv::Mat& left, const cv::Mat& right);
cv::Mat ResizeForDisplay(
    const cv::Mat& source,
    int maximumWidth,
    int maximumHeight);
std::uint64_t MatChecksum(const cv::Mat& image) noexcept;
void DrawLabel(cv::Mat& image, const std::string& text);

} // namespace IC4ExtStressD3D11
