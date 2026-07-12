#include "StressSupport.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace IC4ExtStress {

void PipelineMetrics::reset() noexcept
{
    received_.store(0);
    processed_.store(0);
    failures_.store(0);
    readbackFailures_.store(0);
    outputFailures_.store(0);
    checksum_.store(0);
    totalProcessNs_.store(0);
    maximumProcessNs_.store(0);
}

void PipelineMetrics::recordReceived() noexcept
{
    received_.fetch_add(1, std::memory_order_relaxed);
}

void PipelineMetrics::recordSuccess(
    std::chrono::nanoseconds elapsed,
    std::uint64_t checksumContribution) noexcept
{
    processed_.fetch_add(1, std::memory_order_relaxed);
    checksum_.fetch_xor(checksumContribution, std::memory_order_relaxed);

    const auto elapsedNs = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, elapsed.count()));
    totalProcessNs_.fetch_add(elapsedNs, std::memory_order_relaxed);

    auto currentMaximum = maximumProcessNs_.load(std::memory_order_relaxed);
    while (currentMaximum < elapsedNs &&
           !maximumProcessNs_.compare_exchange_weak(
               currentMaximum,
               elapsedNs,
               std::memory_order_relaxed)) {
    }
}

void PipelineMetrics::recordFailure(
    bool readbackFailure,
    bool outputFailure) noexcept
{
    failures_.fetch_add(1, std::memory_order_relaxed);
    if (readbackFailure) {
        readbackFailures_.fetch_add(1, std::memory_order_relaxed);
    }
    if (outputFailure) {
        outputFailures_.fetch_add(1, std::memory_order_relaxed);
    }
}

PipelineSnapshot PipelineMetrics::snapshot() const noexcept
{
    PipelineSnapshot result;
    result.received = received_.load(std::memory_order_relaxed);
    result.processed = processed_.load(std::memory_order_relaxed);
    result.failures = failures_.load(std::memory_order_relaxed);
    result.readbackFailures = readbackFailures_.load(std::memory_order_relaxed);
    result.outputFailures = outputFailures_.load(std::memory_order_relaxed);
    result.checksum = checksum_.load(std::memory_order_relaxed);

    const auto totalNs = totalProcessNs_.load(std::memory_order_relaxed);
    const auto maximumNs = maximumProcessNs_.load(std::memory_order_relaxed);
    if (result.processed != 0) {
        result.averageProcessMs =
            static_cast<double>(totalNs) /
            static_cast<double>(result.processed) /
            1'000'000.0;
    }
    result.maximumProcessMs = static_cast<double>(maximumNs) / 1'000'000.0;
    return result;
}

void FatalState::set(std::string message)
{
    bool expected = false;
    if (!triggered_.compare_exchange_strong(expected, true)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    message_ = std::move(message);
}

std::string FatalState::message() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

void DisplaySlot::publish(cv::Mat image)
{
    if (image.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    image_ = std::move(image);
    ++sequence_;
}

bool DisplaySlot::snapshot(
    cv::Mat& outImage,
    std::uint64_t& inOutSequence) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (sequence_ == inOutSequence || image_.empty()) return false;
    outImage = image_.clone();
    inOutSequence = sequence_;
    return true;
}

std::uint64_t DisplaySlot::publishedCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

DedicatedReadback::~DedicatedReadback()
{
    waitIdle();
}

bool DedicatedReadback::initialize(
    const IC4Ext::D3D12BackendContext& producerBackend)
{
    initialized_ = false;
    error_ = IC4Ext::NoError();

    auto resolved = producerBackend;
    if (!resolved.resolve() || !resolved.device || !resolved.corePtr) {
        error_ = IC4Ext::MakeError(
            IC4Ext::ErrorCode::InvalidArgument,
            "DedicatedReadback::initialize",
            "producer backend is incomplete");
        return false;
    }

    try {
        queue_.Initialize(
            resolved.device,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);

        backend_ = resolved;
        backend_.queue = &queue_;
        backend_.commandQueue = queue_.Get();
        if (!readback_.initialize(backend_)) {
            error_ = readback_.lastError();
            return false;
        }
    } catch (const std::exception& exception) {
        error_ = IC4Ext::MakeError(
            IC4Ext::ErrorCode::D3D12Error,
            "DedicatedReadback::initialize",
            exception.what());
        return false;
    }

    initialized_ = true;
    return true;
}

bool DedicatedReadback::readBgr(
    const IC4Ext::D3D12::ReadOnlyFrame& frame,
    IC4Ext::CpuFrame& outFrame,
    std::uint32_t timeoutMs)
{
    if (!initialized_) {
        error_ = IC4Ext::MakeError(
            IC4Ext::ErrorCode::NotOpened,
            "DedicatedReadback::readBgr",
            "readback context is not initialized");
        return false;
    }

    const bool result = readback_.readback(
        frame,
        IC4Ext::CpuFrameFormat::BGR8,
        outFrame,
        timeoutMs);
    error_ = result ? IC4Ext::NoError() : readback_.lastError();
    return result;
}

void DedicatedReadback::waitIdle() noexcept
{
    if (!initialized_) return;
    try {
        queue_.WaitIdle();
    } catch (...) {
    }
}

const IC4Ext::ErrorInfo& DedicatedReadback::lastError() const noexcept
{
    return error_;
}

IC4Ext::FrameReadbackCacheStats DedicatedReadback::cacheStats() const noexcept
{
    return readback_.cacheStats();
}

cv::Mat CpuFrameToBgr(const IC4Ext::CpuFrame& frame)
{
    if (frame.format != IC4Ext::CpuFrameFormat::BGR8) {
        throw std::runtime_error("CpuFrameToBgr requires BGR8 input");
    }
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) {
        throw std::runtime_error("CpuFrameToBgr received an empty frame");
    }

    cv::Mat view(
        static_cast<int>(frame.height),
        static_cast<int>(frame.width),
        CV_8UC3,
        const_cast<std::uint8_t*>(frame.data.data()),
        frame.rowPitch);
    return view.clone();
}

cv::Mat JoinHorizontal(const cv::Mat& left, const cv::Mat& right)
{
    if (left.empty() || right.empty()) {
        throw std::runtime_error("JoinHorizontal requires two images");
    }

    cv::Mat normalizedRight;
    if (right.rows != left.rows) {
        const double scale = static_cast<double>(left.rows) /
                             static_cast<double>(right.rows);
        cv::resize(
            right,
            normalizedRight,
            cv::Size(
                std::max(1, static_cast<int>(std::lround(right.cols * scale))),
                left.rows),
            0.0,
            0.0,
            cv::INTER_LINEAR);
    } else {
        normalizedRight = right;
    }

    cv::Mat joined;
    cv::hconcat(left, normalizedRight, joined);
    return joined;
}

cv::Mat ResizeForDisplay(
    const cv::Mat& source,
    int maximumWidth,
    int maximumHeight)
{
    if (source.empty()) return {};
    if (maximumWidth <= 0 || maximumHeight <= 0) return source.clone();

    const double scale = std::min(
        1.0,
        std::min(
            static_cast<double>(maximumWidth) / source.cols,
            static_cast<double>(maximumHeight) / source.rows));
    if (scale >= 1.0) return source.clone();

    cv::Mat resized;
    cv::resize(
        source,
        resized,
        cv::Size(
            std::max(1, static_cast<int>(std::lround(source.cols * scale))),
            std::max(1, static_cast<int>(std::lround(source.rows * scale)))),
        0.0,
        0.0,
        cv::INTER_AREA);
    return resized;
}

std::uint64_t MatChecksum(const cv::Mat& image) noexcept
{
    if (image.empty()) return 0;

    const auto scalar = cv::sum(image);
    std::uint64_t checksum = 1469598103934665603ull;
    for (int channel = 0; channel < 4; ++channel) {
        const auto value = static_cast<std::uint64_t>(
            std::llround(std::max(0.0, scalar[channel])));
        checksum ^= value;
        checksum *= 1099511628211ull;
    }
    checksum ^= static_cast<std::uint64_t>(image.rows);
    checksum *= 1099511628211ull;
    checksum ^= static_cast<std::uint64_t>(image.cols);
    return checksum;
}

void DrawLabel(cv::Mat& image, const std::string& text)
{
    if (image.empty()) return;
    cv::rectangle(
        image,
        cv::Rect(0, 0, image.cols, std::min(42, image.rows)),
        cv::Scalar(0, 0, 0),
        cv::FILLED);
    cv::putText(
        image,
        text,
        cv::Point(10, 29),
        cv::FONT_HERSHEY_SIMPLEX,
        0.75,
        cv::Scalar(0, 255, 0),
        2,
        cv::LINE_AA);
}

} // namespace IC4ExtStress
