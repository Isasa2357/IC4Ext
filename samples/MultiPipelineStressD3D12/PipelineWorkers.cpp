#include "PipelineWorkers.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace IC4ExtStress {

namespace {

using Pipe = IC4Ext::D3D12;

std::string ErrorText(const std::string& prefix, const IC4Ext::ErrorInfo& error)
{
    std::ostringstream stream;
    stream << prefix;
    if (!error.where.empty()) stream << " " << error.where;
    if (!error.message.empty()) stream << ": " << error.message;
    return stream.str();
}

const Pipe::ReadOnlyFrame& RequireFrame(
    const Pipe::ReadOnlyFrameSet& frameSet,
    Pipe::CameraId cameraId)
{
    const auto* frame = frameSet.find(cameraId);
    if (!frame || !*frame) {
        throw std::runtime_error(
            "synchronized frame set does not contain camera " +
            std::to_string(cameraId));
    }
    return *frame;
}

bool InitializeReadback(
    DedicatedReadback& readback,
    const IC4Ext::D3D12BackendContext& backend,
    FatalState& fatal,
    const std::string& pipelineName)
{
    if (readback.initialize(backend)) return true;
    fatal.set(ErrorText(
        pipelineName + " readback initialization failed:",
        readback.lastError()));
    return false;
}

bool ReadFrame(
    DedicatedReadback& readback,
    const Pipe::ReadOnlyFrame& frame,
    std::uint32_t timeoutMs,
    IC4Ext::CpuFrame& cpu,
    PipelineMetrics& metrics,
    FatalState& fatal,
    const std::string& pipelineName)
{
    if (readback.readBgr(frame, cpu, timeoutMs)) return true;
    if (metrics.snapshot().received != 0) {
        metrics.recordFailure(true, false);
    }
    fatal.set(ErrorText(
        pipelineName + " readback failed:",
        readback.lastError()));
    return false;
}

cv::Mat ProcessCanny(const cv::Mat& bgr)
{
    cv::Mat gray;
    cv::Mat blurred;
    cv::Mat edges;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 1.2);
    cv::Canny(blurred, edges, 60.0, 140.0);
    return edges;
}

cv::Mat ProcessSobelMagnitude(const cv::Mat& bgr)
{
    cv::Mat gray;
    cv::Mat blurred;
    cv::Mat gradientX;
    cv::Mat gradientY;
    cv::Mat magnitude;
    cv::Mat normalized;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(7, 7), 1.5);
    cv::Sobel(blurred, gradientX, CV_32F, 1, 0, 3);
    cv::Sobel(blurred, gradientY, CV_32F, 0, 1, 3);
    cv::magnitude(gradientX, gradientY, magnitude);
    cv::normalize(magnitude, normalized, 0.0, 255.0, cv::NORM_MINMAX, CV_8U);
    return normalized;
}

cv::Mat ProcessPairDisplayImage(
    const cv::Mat& bgr,
    const cv::Ptr<cv::CLAHE>& clahe)
{
    cv::Mat gray;
    cv::Mat enhanced;
    cv::Mat edges;
    cv::Mat edgeBgr;
    cv::Mat result;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    clahe->apply(gray, enhanced);
    cv::Canny(enhanced, edges, 50.0, 130.0);
    cv::cvtColor(edges, edgeBgr, cv::COLOR_GRAY2BGR);
    cv::addWeighted(bgr, 0.75, edgeBgr, 0.65, 0.0, result);
    return result;
}

bool OpenVideoWriter(
    cv::VideoWriter& writer,
    const std::filesystem::path& path,
    int fourcc,
    double fps,
    const cv::Size& size,
    FatalState& fatal,
    PipelineMetrics& metrics,
    const std::string& pipelineName)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        metrics.recordFailure(false, true);
        fatal.set(
            pipelineName + " failed to create output directory: " +
            error.message());
        return false;
    }

    if (writer.open(path.string(), fourcc, fps, size, true)) return true;
    metrics.recordFailure(false, true);
    fatal.set(
        pipelineName + " failed to open VideoWriter: " + path.string());
    return false;
}

void RecordSuccessIfMeasuring(
    std::atomic<bool>& measuring,
    PipelineMetrics& metrics,
    Clock::time_point started,
    std::uint64_t checksum)
{
    if (!measuring.load(std::memory_order_relaxed)) return;
    metrics.recordSuccess(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
        checksum);
}

void RecordReceivedIfMeasuring(
    std::atomic<bool>& measuring,
    PipelineMetrics& metrics)
{
    if (measuring.load(std::memory_order_relaxed)) metrics.recordReceived();
}

} // namespace

std::thread StartPairDisplayWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    DisplaySlot& display,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        queue = std::move(queue),
        backend = std::move(backend),
        &display,
        &metrics,
        &fatal,
        &stopRequested,
        &measuring,
        options]() mutable {
        DedicatedReadback readback0;
        DedicatedReadback readback1;
        if (!InitializeReadback(readback0, backend, fatal, "pair-display/0") ||
            !InitializeReadback(readback1, backend, fatal, "pair-display/1")) {
            return;
        }

        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopLatestFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed() || stopRequested.load()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();

                IC4Ext::CpuFrame cpu0;
                IC4Ext::CpuFrame cpu1;
                if (!readback0.readBgr(
                        RequireFrame(*frameSet, 0),
                        cpu0,
                        options.readbackTimeoutMs) ||
                    !readback1.readBgr(
                        RequireFrame(*frameSet, 1),
                        cpu1,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set("pair-display independent readback failed");
                    break;
                }

                cv::Mat image = JoinHorizontal(
                    CpuFrameToBgr(cpu0),
                    CpuFrameToBgr(cpu1));
                DrawLabel(image, "Pipeline 1: latest pair display");
                image = ResizeForDisplay(
                    image,
                    options.displayMaximumWidth,
                    options.displayMaximumHeight);
                const auto checksum = MatChecksum(image);
                display.publish(std::move(image));
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        checksum);
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("pair-display exception: ") + exception.what());
        }
    });
}

std::thread StartSingleDisplayWorker(
    std::string pipelineName,
    Pipe::CameraId cameraId,
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    DisplaySlot& display,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        pipelineName = std::move(pipelineName),
        cameraId,
        queue = std::move(queue),
        backend = std::move(backend),
        &display,
        &metrics,
        &fatal,
        &stopRequested,
        &measuring,
        options]() mutable {
        DedicatedReadback readback;
        if (!InitializeReadback(readback, backend, fatal, pipelineName)) return;

        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopLatestFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed() || stopRequested.load()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();

                IC4Ext::CpuFrame cpu;
                if (!readback.readBgr(
                        RequireFrame(*frameSet, cameraId),
                        cpu,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set(ErrorText(
                        pipelineName + " readback failed",
                        readback.lastError()));
                    break;
                }

                cv::Mat image = CpuFrameToBgr(cpu);
                DrawLabel(image, pipelineName);
                image = ResizeForDisplay(
                    image,
                    options.displayMaximumWidth,
                    options.displayMaximumHeight);
                const auto checksum = MatChecksum(image);
                display.publish(std::move(image));
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        checksum);
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(pipelineName + " exception: " + exception.what());
        }
    });
}

std::thread StartPairVideoWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        queue = std::move(queue),
        backend = std::move(backend),
        &metrics,
        &fatal,
        &measuring,
        options]() mutable {
        DedicatedReadback readback0;
        DedicatedReadback readback1;
        if (!InitializeReadback(readback0, backend, fatal, "pair-video/0") ||
            !InitializeReadback(readback1, backend, fatal, "pair-video/1")) {
            return;
        }

        cv::VideoWriter writer;
        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();

                IC4Ext::CpuFrame cpu0;
                IC4Ext::CpuFrame cpu1;
                if (!readback0.readBgr(
                        RequireFrame(*frameSet, 0),
                        cpu0,
                        options.readbackTimeoutMs) ||
                    !readback1.readBgr(
                        RequireFrame(*frameSet, 1),
                        cpu1,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set("pair-video independent readback failed");
                    break;
                }

                cv::Mat joined = JoinHorizontal(
                    CpuFrameToBgr(cpu0),
                    CpuFrameToBgr(cpu1));
                if (!writer.isOpened() && !OpenVideoWriter(
                        writer,
                        options.outputDirectory / "pair.avi",
                        options.videoFourcc,
                        options.recordFps,
                        joined.size(),
                        fatal,
                        metrics,
                        "pair-video")) {
                    break;
                }
                writer.write(joined);
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        MatChecksum(joined));
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, true);
            fatal.set(std::string("pair-video exception: ") + exception.what());
        }
        writer.release();
    });
}

std::thread StartSingleVideoWorker(
    std::string pipelineName,
    Pipe::CameraId cameraId,
    std::filesystem::path fileName,
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        pipelineName = std::move(pipelineName),
        cameraId,
        fileName = std::move(fileName),
        queue = std::move(queue),
        backend = std::move(backend),
        &metrics,
        &fatal,
        &measuring,
        options]() mutable {
        DedicatedReadback readback;
        if (!InitializeReadback(readback, backend, fatal, pipelineName)) return;

        cv::VideoWriter writer;
        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();

                IC4Ext::CpuFrame cpu;
                if (!readback.readBgr(
                        RequireFrame(*frameSet, cameraId),
                        cpu,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set(ErrorText(
                        pipelineName + " readback failed",
                        readback.lastError()));
                    break;
                }

                cv::Mat image = CpuFrameToBgr(cpu);
                if (!writer.isOpened() && !OpenVideoWriter(
                        writer,
                        options.outputDirectory / fileName,
                        options.videoFourcc,
                        options.recordFps,
                        image.size(),
                        fatal,
                        metrics,
                        pipelineName)) {
                    break;
                }
                writer.write(image);
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        MatChecksum(image));
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, true);
            fatal.set(pipelineName + " exception: " + exception.what());
        }
        writer.release();
    });
}

std::thread StartSobelWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring)
{
    return std::thread([
        queue = std::move(queue),
        backend = std::move(backend),
        &metrics,
        &fatal,
        &measuring]() mutable {
        SobelProcessor processor;
        if (!processor.initialize(backend)) {
            fatal.set(ErrorText(
                "HLSL Sobel initialization failed",
                processor.lastError()));
            return;
        }

        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();

                if (!processor.process(RequireFrame(*frameSet, 0)) ||
                    !processor.process(RequireFrame(*frameSet, 1))) {
                    if (isMeasuring) metrics.recordFailure(false, false);
                    fatal.set(ErrorText(
                        "HLSL Sobel processing failed",
                        processor.lastError()));
                    break;
                }
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        frameSet->syncGroupId());
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("HLSL Sobel exception: ") + exception.what());
        }

        if (!processor.flush()) {
            fatal.set(ErrorText("HLSL Sobel flush failed", processor.lastError()));
        }
    });
}

std::thread StartOpenCvCannyWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        queue = std::move(queue),
        backend = std::move(backend),
        &metrics,
        &fatal,
        &measuring,
        options]() mutable {
        DedicatedReadback readback;
        if (!InitializeReadback(readback, backend, fatal, "opencv-canny")) return;

        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu;
                if (!readback.readBgr(
                        RequireFrame(*frameSet, 0),
                        cpu,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set(ErrorText(
                        "opencv-canny readback failed",
                        readback.lastError()));
                    break;
                }

                cv::Mat edges = ProcessCanny(CpuFrameToBgr(cpu));
                const auto checksum = MatChecksum(edges) ^
                    static_cast<std::uint64_t>(cv::countNonZero(edges));
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        checksum);
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("opencv-canny exception: ") + exception.what());
        }
    });
}

std::thread StartOpenCvSobelWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        queue = std::move(queue),
        backend = std::move(backend),
        &metrics,
        &fatal,
        &measuring,
        options]() mutable {
        DedicatedReadback readback;
        if (!InitializeReadback(readback, backend, fatal, "opencv-sobel")) return;

        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu;
                if (!readback.readBgr(
                        RequireFrame(*frameSet, 1),
                        cpu,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set(ErrorText(
                        "opencv-sobel readback failed",
                        readback.lastError()));
                    break;
                }

                cv::Mat magnitude = ProcessSobelMagnitude(CpuFrameToBgr(cpu));
                const auto checksum = MatChecksum(magnitude) ^
                    static_cast<std::uint64_t>(std::llround(cv::mean(magnitude)[0] * 1000.0));
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        checksum);
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("opencv-sobel exception: ") + exception.what());
        }
    });
}

std::thread StartOpenCvPairDisplayWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    DisplaySlot& display,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& measuring,
    WorkerOptions options)
{
    return std::thread([
        queue = std::move(queue),
        backend = std::move(backend),
        &display,
        &metrics,
        &fatal,
        &stopRequested,
        &measuring,
        options]() mutable {
        DedicatedReadback readback0;
        DedicatedReadback readback1;
        if (!InitializeReadback(readback0, backend, fatal, "opencv-pair-display/0") ||
            !InitializeReadback(readback1, backend, fatal, "opencv-pair-display/1")) {
            return;
        }
        auto clahe0 = cv::createCLAHE(2.0, cv::Size(8, 8));
        auto clahe1 = cv::createCLAHE(2.0, cv::Size(8, 8));

        try {
            while (!fatal.triggered()) {
                auto frameSet = queue->waitPopLatestFor(std::chrono::milliseconds(100));
                if (!frameSet) {
                    if (queue->isClosed() || stopRequested.load()) break;
                    continue;
                }

                const bool isMeasuring = measuring.load(std::memory_order_relaxed);
                if (isMeasuring) metrics.recordReceived();
                const auto started = Clock::now();

                IC4Ext::CpuFrame cpu0;
                IC4Ext::CpuFrame cpu1;
                if (!readback0.readBgr(
                        RequireFrame(*frameSet, 0),
                        cpu0,
                        options.readbackTimeoutMs) ||
                    !readback1.readBgr(
                        RequireFrame(*frameSet, 1),
                        cpu1,
                        options.readbackTimeoutMs)) {
                    if (isMeasuring) metrics.recordFailure(true, false);
                    fatal.set("opencv-pair-display independent readback failed");
                    break;
                }

                cv::Mat left = ProcessPairDisplayImage(CpuFrameToBgr(cpu0), clahe0);
                cv::Mat right = ProcessPairDisplayImage(CpuFrameToBgr(cpu1), clahe1);
                cv::Mat image = JoinHorizontal(left, right);
                DrawLabel(image, "Pipeline 10: OpenCV processed latest pair");
                image = ResizeForDisplay(
                    image,
                    options.displayMaximumWidth,
                    options.displayMaximumHeight);
                const auto checksum = MatChecksum(image);
                display.publish(std::move(image));
                if (isMeasuring) {
                    metrics.recordSuccess(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started),
                        checksum);
                }
            }
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("opencv-pair-display exception: ") + exception.what());
        }
    });
}

} // namespace IC4ExtStress
