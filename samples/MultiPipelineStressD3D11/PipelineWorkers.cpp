#include "PipelineWorkers.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <cmath>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace IC4ExtStressD3D11 {
namespace {

namespace Pipe = IC4Ext::D3D11;

std::string ErrorText(const std::string& prefix, const IC4Ext::ErrorInfo& error)
{
    std::ostringstream stream;
    stream << prefix;
    if (!error.where.empty()) stream << ' ' << error.where;
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
            "synchronized set is missing camera " + std::to_string(cameraId));
    }
    return *frame;
}

bool InitializeReadback(
    DedicatedReadback& readback,
    const IC4Ext::D3D11BackendContext& backend,
    FatalState& fatal,
    const std::string& name)
{
    if (readback.initialize(backend)) return true;
    fatal.set(ErrorText(
        name + " readback initialization failed",
        readback.lastError()));
    return false;
}

bool ReadBgr(
    DedicatedReadback& readback,
    const Pipe::ReadOnlyFrame& frame,
    std::uint32_t timeoutMs,
    IC4Ext::CpuFrame& cpu,
    FatalState& fatal,
    const std::string& name)
{
    if (readback.readBgr(frame, cpu, timeoutMs)) return true;
    fatal.set(ErrorText(name + " readback failed", readback.lastError()));
    return false;
}

void Success(
    bool measuring,
    PipelineMetrics& metrics,
    Clock::time_point started,
    std::uint64_t checksum)
{
    if (!measuring) return;
    metrics.recordSuccess(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started),
        checksum);
}

template<class Function>
void RunLatestLoop(
    const FrameSetQueuePtr& queue,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    Function&& function)
{
    while (!fatal.triggered()) {
        auto frameSet = queue->waitPopLatestFor(std::chrono::milliseconds(100));
        if (!frameSet) {
            if (queue->isClosed() || stopRequested.load()) break;
            continue;
        }
        function(*frameSet);
    }
}

template<class Function>
void RunAllFrameLoop(
    const FrameSetQueuePtr& queue,
    FatalState& fatal,
    Function&& function)
{
    while (!fatal.triggered()) {
        auto frameSet = queue->waitPopFor(std::chrono::milliseconds(100));
        if (!frameSet) {
            if (queue->isClosed()) break;
            continue;
        }
        function(*frameSet);
    }
}

cv::Mat CannyWorkload(const cv::Mat& bgr)
{
    cv::Mat gray;
    cv::Mat blurred;
    cv::Mat edges;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 1.2);
    cv::Canny(blurred, edges, 60.0, 140.0);
    return edges;
}

cv::Mat SobelWorkload(const cv::Mat& bgr)
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

cv::Mat EdgeOverlay(const cv::Mat& bgr, const cv::Ptr<cv::CLAHE>& clahe)
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

bool OpenWriter(
    cv::VideoWriter& writer,
    const std::filesystem::path& path,
    const cv::Size& size,
    const WorkerOptions& options,
    PipelineMetrics& metrics,
    FatalState& fatal,
    const std::string& name)
{
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        metrics.recordFailure(false, true);
        fatal.set(name + " output directory failure: " + error.message());
        return false;
    }

    if (writer.open(
            path.string(),
            options.videoFourcc,
            options.recordFps,
            size,
            true)) {
        return true;
    }

    metrics.recordFailure(false, true);
    fatal.set(name + " VideoWriter open failed: " + path.string());
    return false;
}

} // namespace

std::thread StartPairDisplayWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D11BackendContext backend,
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
            RunLatestLoop(queue, fatal, stopRequested, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu0;
                IC4Ext::CpuFrame cpu1;
                if (!ReadBgr(
                        readback0,
                        RequireFrame(set, 0),
                        options.readbackTimeoutMs,
                        cpu0,
                        fatal,
                        "pair-display/0") ||
                    !ReadBgr(
                        readback1,
                        RequireFrame(set, 1),
                        options.readbackTimeoutMs,
                        cpu1,
                        fatal,
                        "pair-display/1")) {
                    if (active) metrics.recordFailure(true, false);
                    return;
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
                Success(active, metrics, started, checksum);
            });
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
    IC4Ext::D3D11BackendContext backend,
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
            RunLatestLoop(queue, fatal, stopRequested, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu;
                if (!ReadBgr(
                        readback,
                        RequireFrame(set, cameraId),
                        options.readbackTimeoutMs,
                        cpu,
                        fatal,
                        pipelineName)) {
                    if (active) metrics.recordFailure(true, false);
                    return;
                }

                cv::Mat image = CpuFrameToBgr(cpu);
                DrawLabel(image, pipelineName);
                image = ResizeForDisplay(
                    image,
                    options.displayMaximumWidth,
                    options.displayMaximumHeight);
                const auto checksum = MatChecksum(image);
                display.publish(std::move(image));
                Success(active, metrics, started, checksum);
            });
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(pipelineName + " exception: " + exception.what());
        }
    });
}

std::thread StartPairVideoWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D11BackendContext backend,
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
            RunAllFrameLoop(queue, fatal, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu0;
                IC4Ext::CpuFrame cpu1;
                if (!ReadBgr(
                        readback0,
                        RequireFrame(set, 0),
                        options.readbackTimeoutMs,
                        cpu0,
                        fatal,
                        "pair-video/0") ||
                    !ReadBgr(
                        readback1,
                        RequireFrame(set, 1),
                        options.readbackTimeoutMs,
                        cpu1,
                        fatal,
                        "pair-video/1")) {
                    if (active) metrics.recordFailure(true, false);
                    return;
                }

                cv::Mat joined = JoinHorizontal(
                    CpuFrameToBgr(cpu0),
                    CpuFrameToBgr(cpu1));
                if (!writer.isOpened() &&
                    !OpenWriter(
                        writer,
                        options.outputDirectory / "pair.avi",
                        joined.size(),
                        options,
                        metrics,
                        fatal,
                        "pair-video")) {
                    return;
                }
                writer.write(joined);
                Success(active, metrics, started, MatChecksum(joined));
            });
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
    IC4Ext::D3D11BackendContext backend,
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
            RunAllFrameLoop(queue, fatal, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu;
                if (!ReadBgr(
                        readback,
                        RequireFrame(set, cameraId),
                        options.readbackTimeoutMs,
                        cpu,
                        fatal,
                        pipelineName)) {
                    if (active) metrics.recordFailure(true, false);
                    return;
                }

                cv::Mat image = CpuFrameToBgr(cpu);
                if (!writer.isOpened() &&
                    !OpenWriter(
                        writer,
                        options.outputDirectory / fileName,
                        image.size(),
                        options,
                        metrics,
                        fatal,
                        pipelineName)) {
                    return;
                }
                writer.write(image);
                Success(active, metrics, started, MatChecksum(image));
            });
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, true);
            fatal.set(pipelineName + " exception: " + exception.what());
        }
        writer.release();
    });
}

std::thread StartSobelWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D11BackendContext backend,
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
            RunAllFrameLoop(queue, fatal, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                if (!processor.process(RequireFrame(set, 0)) ||
                    !processor.process(RequireFrame(set, 1))) {
                    if (active) metrics.recordFailure(false, false);
                    fatal.set(ErrorText(
                        "HLSL Sobel failed",
                        processor.lastError()));
                    return;
                }
                Success(active, metrics, started, set.syncGroupId());
            });
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("HLSL Sobel exception: ") + exception.what());
        }

        if (!processor.flush()) {
            fatal.set(ErrorText(
                "HLSL Sobel flush failed",
                processor.lastError()));
        }
    });
}

std::thread StartOpenCvCannyWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D11BackendContext backend,
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
            RunAllFrameLoop(queue, fatal, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu;
                if (!ReadBgr(
                        readback,
                        RequireFrame(set, 0),
                        options.readbackTimeoutMs,
                        cpu,
                        fatal,
                        "opencv-canny")) {
                    if (active) metrics.recordFailure(true, false);
                    return;
                }

                cv::Mat edges = CannyWorkload(CpuFrameToBgr(cpu));
                const auto checksum = MatChecksum(edges) ^
                    static_cast<std::uint64_t>(cv::countNonZero(edges));
                Success(active, metrics, started, checksum);
            });
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("opencv-canny exception: ") + exception.what());
        }
    });
}

std::thread StartOpenCvSobelWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D11BackendContext backend,
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
            RunAllFrameLoop(queue, fatal, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu;
                if (!ReadBgr(
                        readback,
                        RequireFrame(set, 1),
                        options.readbackTimeoutMs,
                        cpu,
                        fatal,
                        "opencv-sobel")) {
                    if (active) metrics.recordFailure(true, false);
                    return;
                }

                cv::Mat magnitude = SobelWorkload(CpuFrameToBgr(cpu));
                const auto checksum = MatChecksum(magnitude) ^
                    static_cast<std::uint64_t>(
                        std::llround(cv::mean(magnitude)[0] * 1000.0));
                Success(active, metrics, started, checksum);
            });
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(std::string("opencv-sobel exception: ") + exception.what());
        }
    });
}

std::thread StartOpenCvPairDisplayWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D11BackendContext backend,
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
        if (!InitializeReadback(readback0, backend, fatal, "opencv-pair/0") ||
            !InitializeReadback(readback1, backend, fatal, "opencv-pair/1")) {
            return;
        }

        const auto clahe0 = cv::createCLAHE(2.0, cv::Size(8, 8));
        const auto clahe1 = cv::createCLAHE(2.0, cv::Size(8, 8));
        try {
            RunLatestLoop(queue, fatal, stopRequested, [&](const auto& set) {
                const bool active = measuring.load();
                if (active) metrics.recordReceived();
                const auto started = Clock::now();
                IC4Ext::CpuFrame cpu0;
                IC4Ext::CpuFrame cpu1;
                if (!ReadBgr(
                        readback0,
                        RequireFrame(set, 0),
                        options.readbackTimeoutMs,
                        cpu0,
                        fatal,
                        "opencv-pair/0") ||
                    !ReadBgr(
                        readback1,
                        RequireFrame(set, 1),
                        options.readbackTimeoutMs,
                        cpu1,
                        fatal,
                        "opencv-pair/1")) {
                    if (active) metrics.recordFailure(true, false);
                    return;
                }

                cv::Mat image = JoinHorizontal(
                    EdgeOverlay(CpuFrameToBgr(cpu0), clahe0),
                    EdgeOverlay(CpuFrameToBgr(cpu1), clahe1));
                DrawLabel(
                    image,
                    "Pipeline 10: OpenCV processed latest pair");
                image = ResizeForDisplay(
                    image,
                    options.displayMaximumWidth,
                    options.displayMaximumHeight);
                const auto checksum = MatChecksum(image);
                display.publish(std::move(image));
                Success(active, metrics, started, checksum);
            });
        } catch (const std::exception& exception) {
            if (measuring.load()) metrics.recordFailure(false, false);
            fatal.set(
                std::string("opencv-pair-display exception: ") +
                exception.what());
        }
    });
}

} // namespace IC4ExtStressD3D11
