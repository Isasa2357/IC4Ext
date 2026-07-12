#include "PipelineWorkers.hpp"
#include "StressSupport.hpp"

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <IC4Ext/IC4Ext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Pipe = IC4Ext::D3D12;
using IC4ExtStress::Clock;
using IC4ExtStress::DisplaySlot;
using IC4ExtStress::FatalState;
using IC4ExtStress::FrameSetQueuePtr;
using IC4ExtStress::PipelineMetrics;
using IC4ExtStress::WorkerOptions;

std::atomic<bool>* GlobalStop = nullptr;
void SignalHandler(int) { if (GlobalStop) GlobalStop->store(true); }

const char* Value(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
}

bool Flag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

int IntValue(int argc, char** argv, const char* name, int fallback)
{
    const auto* value = Value(argc, argv, name);
    return value ? std::atoi(value) : fallback;
}

double DoubleValue(int argc, char** argv, const char* name, double fallback)
{
    const auto* value = Value(argc, argv, name);
    return value ? std::atof(value) : fallback;
}

struct Options
{
    int device0 = 0;
    int device1 = 1;
    int warmupSec = 5;
    int durationSec = 30;
    int reportMs = 1000;
    bool hardwareTrigger = false;
    std::string triggerSource = "Line1";
    Pipe::FrameSyncTimestampSource timestampSource =
        Pipe::FrameSyncTimestampSource::HostReceived;
    std::uint64_t maxDiffUs = 4000;

    int width = 0;
    int height = 0;
    int offsetX = -1;
    int offsetY = -1;
    double fps = 0.0;
    IC4Ext::CameraPixelFormat format = IC4Ext::CameraPixelFormat::BGR8;
    bool forceFormat = false;
    std::filesystem::path json0;
    std::filesystem::path json1;
    std::size_t jsonIndex0 = 0;
    std::size_t jsonIndex1 = 0;

    int ingressQueue = 128;
    int latestQueue = 1;
    int allFrameQueue = 32;
    int poolInitial = 16;
    int poolMax = 64;
    int pendingBuffers = 64;
    int readTimeoutMs = 1000;
    int readbackTimeoutMs = 5000;

    double recordFps = 160.0;
    std::string codec = "MJPG";
    std::filesystem::path outputDir = "stress_output";
    std::filesystem::path csv = "stress_output/metrics.csv";
    bool windows = true;
    int displayWidth = 1280;
    int displayHeight = 720;
};

Pipe::FrameSyncTimestampSource ParseTimestampSource(const std::string& text)
{
    if (text == "host") return Pipe::FrameSyncTimestampSource::HostReceived;
    if (text == "device") return Pipe::FrameSyncTimestampSource::Device;
    if (text == "auto") return Pipe::FrameSyncTimestampSource::Auto;
    throw std::runtime_error("--timestamp-source must be host, device, or auto");
}

Options ParseOptions(int argc, char** argv)
{
    Options o;
    o.device0 = IntValue(argc, argv, "--device0", o.device0);
    o.device1 = IntValue(argc, argv, "--device1", o.device1);
    o.warmupSec = IntValue(argc, argv, "--warmup-sec", o.warmupSec);
    o.durationSec = IntValue(argc, argv, "--duration-sec", o.durationSec);
    o.reportMs = IntValue(argc, argv, "--report-ms", o.reportMs);
    o.hardwareTrigger = Flag(argc, argv, "--hardware-trigger");
    if (const auto* v = Value(argc, argv, "--trigger-source")) o.triggerSource = v;
    if (const auto* v = Value(argc, argv, "--timestamp-source")) {
        o.timestampSource = ParseTimestampSource(v);
    }
    if (const auto* v = Value(argc, argv, "--max-diff-us")) {
        o.maxDiffUs = std::strtoull(v, nullptr, 10);
    }

    o.width = IntValue(argc, argv, "--width", o.width);
    o.height = IntValue(argc, argv, "--height", o.height);
    o.offsetX = IntValue(argc, argv, "--offset-x", o.offsetX);
    o.offsetY = IntValue(argc, argv, "--offset-y", o.offsetY);
    o.fps = DoubleValue(argc, argv, "--fps", o.fps);
    if (const auto* v = Value(argc, argv, "--format")) {
        if (!IC4Ext::ParseCameraPixelFormat(v, o.format)) {
            throw std::runtime_error("unsupported --format value");
        }
        o.forceFormat = true;
    }
    if (const auto* v = Value(argc, argv, "--ic4-json0")) o.json0 = v;
    if (const auto* v = Value(argc, argv, "--ic4-json1")) o.json1 = v;
    o.jsonIndex0 = static_cast<std::size_t>(
        std::max(0, IntValue(argc, argv, "--json-device-index0", 0)));
    o.jsonIndex1 = static_cast<std::size_t>(
        std::max(0, IntValue(argc, argv, "--json-device-index1", 0)));

    o.ingressQueue = IntValue(argc, argv, "--ingress-queue", o.ingressQueue);
    o.latestQueue = IntValue(argc, argv, "--latest-queue", o.latestQueue);
    o.allFrameQueue = IntValue(argc, argv, "--all-frame-queue", o.allFrameQueue);
    o.poolInitial = IntValue(argc, argv, "--capture-pool-initial", o.poolInitial);
    o.poolMax = IntValue(argc, argv, "--capture-pool-max", o.poolMax);
    o.pendingBuffers = IntValue(argc, argv, "--max-pending-buffers", o.pendingBuffers);
    o.readTimeoutMs = IntValue(argc, argv, "--read-timeout-ms", o.readTimeoutMs);
    o.readbackTimeoutMs = IntValue(
        argc, argv, "--readback-timeout-ms", o.readbackTimeoutMs);

    o.recordFps = DoubleValue(argc, argv, "--record-fps", o.recordFps);
    if (const auto* v = Value(argc, argv, "--video-codec")) o.codec = v;
    if (const auto* v = Value(argc, argv, "--output-dir")) o.outputDir = v;
    if (const auto* v = Value(argc, argv, "--csv")) o.csv = v;
    o.windows = !Flag(argc, argv, "--no-display-windows");
    o.displayWidth = IntValue(argc, argv, "--display-max-width", o.displayWidth);
    o.displayHeight = IntValue(argc, argv, "--display-max-height", o.displayHeight);

    if (o.device0 == o.device1 || o.durationSec <= 0 || o.warmupSec < 0 ||
        o.reportMs <= 0 || o.maxDiffUs == 0 || o.ingressQueue <= 0 ||
        o.latestQueue <= 0 || o.allFrameQueue <= 0 || o.poolInitial <= 0 ||
        o.poolMax < o.poolInitial || o.pendingBuffers <= 0 ||
        o.readTimeoutMs <= 0 || o.readbackTimeoutMs <= 0 || o.recordFps <= 0.0 ||
        o.codec.size() != 4) {
        throw std::runtime_error("invalid stress-test options");
    }
    return o;
}

IC4Ext::CameraCaptureConfig CameraConfig(
    const Options& o,
    const std::filesystem::path& json,
    std::size_t jsonIndex)
{
    IC4Ext::CameraCaptureConfig config;
    if (!json.empty()) {
        config.ic4StateJson.path = json;
        config.ic4StateJson.deviceIndex = jsonIndex;
        config.ic4StateJson.strict = false;
        config.ic4StateJson.applyNestedSelectorStates = true;
    }
    config.streamRequest.width = o.width;
    config.streamRequest.height = o.height;
    config.streamRequest.fps = o.fps;
    config.streamRequest.requestedFormat = o.format;
    config.streamRequest.forceRequestedFormat = o.forceFormat;
    if (o.offsetX >= 0) config.streamRequest.offsetX = o.offsetX;
    if (o.offsetY >= 0) config.streamRequest.offsetY = o.offsetY;
    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.outputSpec.createSrv = true;
    config.outputSpec.createUav = true;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::PreserveFrames;
    config.maxPendingBuffers = static_cast<std::size_t>(o.pendingBuffers);
    config.shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / "d3d12";
    if (o.hardwareTrigger) {
        IC4Ext::ConfigureHardwareTriggerSync(config, o.triggerSource);
    }
    return config;
}

FrameSetQueuePtr Queue(std::size_t capacity, bool latest)
{
    ThreadKit::Queues::QueueOptions options;
    options.maxSize = capacity;
    options.overflowPolicy = latest
        ? ThreadKit::Queues::QueueOverflowPolicy::DropOldest
        : ThreadKit::Queues::QueueOverflowPolicy::RejectNew;
    return std::make_shared<Pipe::ReadOnlyFrameSetQueue>(options);
}

struct Pipeline
{
    const char* name = "";
    bool latest = false;
    std::vector<Pipe::CameraId> cameras;
    int priority = 0;
    FrameSetQueuePtr queue;
    Pipe::FrameSyncOutputId outputId = Pipe::InvalidFrameSyncOutputId;
    PipelineMetrics* metrics = nullptr;
    Pipe::FrameSyncOutputStats baseline{};
};

std::uint64_t Delta(std::uint64_t a, std::uint64_t b)
{
    return a >= b ? a - b : 0;
}

Pipe::FrameSyncOutputStats Diff(
    const Pipe::FrameSyncOutputStats& a,
    const Pipe::FrameSyncOutputStats& b)
{
    Pipe::FrameSyncOutputStats d;
    d.consideredSets = Delta(a.consideredSets, b.consideredSets);
    d.skippedByFrameRate = Delta(a.skippedByFrameRate, b.skippedByFrameRate);
    d.emittedSets = Delta(a.emittedSets, b.emittedSets);
    d.queueDrops = Delta(a.queueDrops, b.queueDrops);
    d.disabledSkips = Delta(a.disabledSkips, b.disabledSkips);
    return d;
}

class WorkerGroup
{
public:
    WorkerGroup(std::vector<Pipeline>& pipelines, std::atomic<bool>& stop)
        : pipelines_(pipelines), stop_(stop) {}
    ~WorkerGroup() { closeAndJoin(); }
    std::vector<std::thread>& threads() { return threads_; }
    void closeAndJoin()
    {
        if (joined_) return;
        stop_.store(true);
        for (const auto& pipeline : pipelines_) pipeline.queue->close();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
        joined_ = true;
    }
private:
    std::vector<Pipeline>& pipelines_;
    std::atomic<bool>& stop_;
    std::vector<std::thread> threads_;
    bool joined_ = false;
};

void PumpWindows(
    bool enabled,
    const std::array<std::string, 4>& names,
    const std::array<DisplaySlot*, 4>& slots,
    std::array<std::uint64_t, 4>& sequences,
    std::atomic<bool>& stop)
{
    if (!enabled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        cv::Mat image;
        if (slots[i]->snapshot(image, sequences[i])) cv::imshow(names[i], image);
    }
    const int key = cv::waitKey(1);
    if (key == 27 || key == 'q' || key == 'Q') stop.store(true);
}

bool PipelinePass(
    const Pipeline& p,
    const Pipe::FrameSyncOutputStats& output,
    std::string& reason)
{
    const auto m = p.metrics->snapshot();
    const auto q = p.queue->stats();
    if (m.processed == 0) { reason = "processed zero"; return false; }
    if (m.failures != 0) { reason = "worker failure"; return false; }
    if (p.latest) return true;
    if (output.queueDrops != 0 || q.rejectedNew != 0 || q.droppedOldest != 0) {
        reason = "all-frame queue drop";
        return false;
    }
    if (m.received != m.processed) {
        reason = "received/processed mismatch";
        return false;
    }
    return true;
}

void PrintProgress(const std::vector<Pipeline>& pipelines, double seconds)
{
    std::cout << std::fixed << std::setprecision(1)
              << "[progress] sec=" << seconds;
    for (const auto& p : pipelines) {
        const auto m = p.metrics->snapshot();
        const auto q = p.queue->stats();
        std::cout << " | " << p.name
                  << "{proc=" << m.processed
                  << ",fps=" << (seconds > 0.0 ? m.processed / seconds : 0.0)
                  << ",q=" << q.currentSize
                  << ",qmax=" << q.maxObservedSize
                  << ",drop=" << q.droppedOldest + q.rejectedNew
                  << ",fail=" << m.failures << "}";
    }
    std::cout << std::endl;
}

void WriteCsv(
    const std::filesystem::path& path,
    const std::vector<Pipeline>& pipelines,
    const std::vector<Pipe::FrameSyncOutputStats>& output,
    double measurementSec,
    double drainSec)
{
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream file(path);
    if (!file) throw std::runtime_error("cannot open CSV: " + path.string());
    file << "pipeline,mode,received,processed,fps,failures,readback_failures,"
            "output_failures,average_ms,maximum_ms,queue_max,drop_oldest,"
            "reject_new,pop_latest_drop,dispatch_emitted,dispatch_drop,"
            "measurement_sec,drain_sec,pass,reason\n";
    for (std::size_t i = 0; i < pipelines.size(); ++i) {
        const auto& p = pipelines[i];
        const auto m = p.metrics->snapshot();
        const auto q = p.queue->stats();
        std::string reason;
        const bool pass = PipelinePass(p, output[i], reason);
        file << p.name << ',' << (p.latest ? "latest" : "all") << ','
             << m.received << ',' << m.processed << ','
             << (measurementSec > 0.0 ? m.processed / measurementSec : 0.0) << ','
             << m.failures << ',' << m.readbackFailures << ',' << m.outputFailures << ','
             << m.averageProcessMs << ',' << m.maximumProcessMs << ','
             << q.maxObservedSize << ',' << q.droppedOldest << ',' << q.rejectedNew << ','
             << q.droppedByPopLatest << ',' << output[i].emittedSets << ','
             << output[i].queueDrops << ',' << measurementSec << ',' << drainSec << ','
             << (pass ? 1 : 0) << ",\"" << reason << "\"\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::atomic<bool> stop{false};
    GlobalStop = &stop;
    std::signal(SIGINT, SignalHandler);

    try {
        const Options o = ParseOptions(argc, argv);
        std::error_code error;
        std::filesystem::create_directories(o.outputDir, error);
        if (error) throw std::runtime_error("output directory: " + error.message());

        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        auto backend = IC4Ext::D3D12BackendContext::FromCore(core);
        if (!backend.resolve()) throw std::runtime_error("D3D12 backend resolve failed");

        ThreadKit::Queues::QueueOptions ingressOptions;
        ingressOptions.maxSize = static_cast<std::size_t>(o.ingressQueue);
        ingressOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
        auto ingress = std::make_shared<Pipe::IndexedReadOnlyFrameQueue>(ingressOptions);

        Pipe::FrameSyncConfig syncConfig;
        syncConfig.cameraIds = {0, 1};
        syncConfig.timestampSource = o.timestampSource;
        syncConfig.maxTimestampDiffNs = o.maxDiffUs * 1000ull;
        syncConfig.maxBufferedFramesPerCamera = 32;
        syncConfig.groupTimeout = std::chrono::milliseconds(100);
        Pipe::FrameSyncThread sync(ingress, syncConfig);

        std::array<PipelineMetrics, 10> metrics;
        const auto latest = static_cast<std::size_t>(o.latestQueue);
        const auto all = static_cast<std::size_t>(o.allFrameQueue);
        std::vector<Pipeline> pipelines = {
            {"pair_display", true, {0,1}, 300, Queue(latest, true), 0, &metrics[0]},
            {"id0_display", true, {0}, 290, Queue(latest, true), 0, &metrics[1]},
            {"id1_display", true, {1}, 280, Queue(latest, true), 0, &metrics[2]},
            {"pair_video", false, {0,1}, 1000, Queue(all, false), 0, &metrics[3]},
            {"id0_video", false, {0}, 990, Queue(all, false), 0, &metrics[4]},
            {"id1_video", false, {1}, 980, Queue(all, false), 0, &metrics[5]},
            {"hlsl_sobel", false, {0,1}, 900, Queue(all, false), 0, &metrics[6]},
            {"opencv_canny_id0", false, {0}, 800, Queue(all, false), 0, &metrics[7]},
            {"opencv_sobel_id1", false, {1}, 790, Queue(all, false), 0, &metrics[8]},
            {"opencv_pair_display", true, {0,1}, 270, Queue(latest, true), 0, &metrics[9]}
        };
        for (auto& p : pipelines) {
            Pipe::FrameSyncOutputConfig config;
            config.requiredCameras = p.cameras;
            config.frameRate = Pipe::FrameRateLimit::Maximum();
            config.priority = p.priority;
            p.outputId = sync.registerOutput(p.queue, config);
            if (p.outputId == Pipe::InvalidFrameSyncOutputId) {
                throw std::runtime_error("output registration failed: " + p.name);
            }
        }

        Pipe::CameraCaptureOptions captureOptions;
        captureOptions.initialFramePoolCapacity = static_cast<std::size_t>(o.poolInitial);
        captureOptions.maxFramePoolCapacity = static_cast<std::size_t>(o.poolMax);
        captureOptions.framePoolExhaustionPolicy = Pipe::FramePoolExhaustionPolicy::DropNewest;
        Pipe::CameraCaptureThreadOptions threadOptions;
        threadOptions.readTimeoutMs = static_cast<std::uint32_t>(o.readTimeoutMs);
        threadOptions.stopOnReadError = false;

        IC4Ext::IC4DeviceSelector selector0; selector0.deviceIndex = o.device0;
        IC4Ext::IC4DeviceSelector selector1; selector1.deviceIndex = o.device1;
        Pipe::CameraCaptureThread camera0(
            0, selector0, CameraConfig(o, o.json0, o.jsonIndex0),
            backend, captureOptions, threadOptions);
        Pipe::CameraCaptureThread camera1(
            1, selector1, CameraConfig(o, o.json1, o.jsonIndex1),
            backend, captureOptions, threadOptions);
        camera0.setOutputQueue(ingress);
        camera1.setOutputQueue(ingress);

        FatalState fatal;
        std::atomic<bool> measuring{false};
        DisplaySlot pairDisplay, id0Display, id1Display, cvPairDisplay;
        WorkerOptions workerOptions;
        workerOptions.readbackTimeoutMs = static_cast<std::uint32_t>(o.readbackTimeoutMs);
        workerOptions.recordFps = o.recordFps;
        workerOptions.videoFourcc = cv::VideoWriter::fourcc(
            o.codec[0], o.codec[1], o.codec[2], o.codec[3]);
        workerOptions.displayMaximumWidth = o.displayWidth;
        workerOptions.displayMaximumHeight = o.displayHeight;
        workerOptions.outputDirectory = o.outputDir;

        WorkerGroup workers(pipelines, stop);
        auto& threads = workers.threads();
        threads.reserve(10);
        threads.push_back(IC4ExtStress::StartPairDisplayWorker(
            pipelines[0].queue, backend, pairDisplay, metrics[0], fatal, stop, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartSingleDisplayWorker(
            "Pipeline 2: latest id0", 0, pipelines[1].queue, backend,
            id0Display, metrics[1], fatal, stop, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartSingleDisplayWorker(
            "Pipeline 3: latest id1", 1, pipelines[2].queue, backend,
            id1Display, metrics[2], fatal, stop, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartPairVideoWorker(
            pipelines[3].queue, backend, metrics[3], fatal, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartSingleVideoWorker(
            "id0-video", 0, "id0.avi", pipelines[4].queue, backend,
            metrics[4], fatal, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartSingleVideoWorker(
            "id1-video", 1, "id1.avi", pipelines[5].queue, backend,
            metrics[5], fatal, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartSobelWorker(
            pipelines[6].queue, backend, metrics[6], fatal, measuring));
        threads.push_back(IC4ExtStress::StartOpenCvCannyWorker(
            pipelines[7].queue, backend, metrics[7], fatal, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartOpenCvSobelWorker(
            pipelines[8].queue, backend, metrics[8], fatal, measuring, workerOptions));
        threads.push_back(IC4ExtStress::StartOpenCvPairDisplayWorker(
            pipelines[9].queue, backend, cvPairDisplay, metrics[9], fatal,
            stop, measuring, workerOptions));

        const std::array<std::string,4> windowNames = {
            "1 Pair latest", "2 Camera 0 latest", "3 Camera 1 latest", "10 OpenCV pair latest"};
        const std::array<DisplaySlot*,4> displaySlots = {
            &pairDisplay, &id0Display, &id1Display, &cvPairDisplay};
        std::array<std::uint64_t,4> displaySequences{};
        if (o.windows) for (const auto& name : windowNames) cv::namedWindow(name, cv::WINDOW_NORMAL);

        if (!sync.start()) throw std::runtime_error("FrameSyncThread start failed");
        if (!camera0.start()) throw std::runtime_error("camera0 start failed");
        if (!camera1.start()) throw std::runtime_error("camera1 start failed");

        std::cout << "MultiPipelineStressD3D12: 10 pipelines, independent CPU readbacks"
                  << " warmup=" << o.warmupSec << "s duration=" << o.durationSec
                  << "s allQueue=" << o.allFrameQueue << " latestQueue=" << o.latestQueue
                  << " recordFps=" << o.recordFps << " codec=" << o.codec << std::endl;

        const auto warmupEnd = Clock::now() + std::chrono::seconds(o.warmupSec);
        while (!stop.load() && !fatal.triggered() && Clock::now() < warmupEnd) {
            PumpWindows(o.windows, windowNames, displaySlots, displaySequences, stop);
        }

        for (auto& p : pipelines) {
            p.queue->clear();
            p.queue->resetStats();
            p.metrics->reset();
            p.baseline = sync.outputStats(p.outputId).value_or(Pipe::FrameSyncOutputStats{});
        }
        const auto syncBase = sync.stats();
        const auto cam0Base = camera0.stats();
        const auto cam1Base = camera1.stats();
        const auto pool0Base = camera0.framePoolStats();
        const auto pool1Base = camera1.framePoolStats();

        measuring.store(true);
        const auto measureStart = Clock::now();
        const auto measureEnd = measureStart + std::chrono::seconds(o.durationSec);
        auto nextReport = measureStart + std::chrono::milliseconds(o.reportMs);
        while (!stop.load() && !fatal.triggered() && Clock::now() < measureEnd) {
            PumpWindows(o.windows, windowNames, displaySlots, displaySequences, stop);
            const auto now = Clock::now();
            if (now >= nextReport) {
                PrintProgress(pipelines, std::chrono::duration<double>(now - measureStart).count());
                nextReport = now + std::chrono::milliseconds(o.reportMs);
            }
        }
        const auto captureStop = Clock::now();
        stop.store(true);
        camera0.stopAndJoin();
        camera1.stopAndJoin();
        camera0.stopAcquisition();
        camera1.stopAcquisition();
        sync.stopAndJoin();
        ingress->close();

        const auto drainStart = Clock::now();
        workers.closeAndJoin();
        const auto drainEnd = Clock::now();
        measuring.store(false);
        if (o.windows) cv::destroyAllWindows();

        const double measurementSec = std::chrono::duration<double>(captureStop - measureStart).count();
        const double drainSec = std::chrono::duration<double>(drainEnd - drainStart).count();
        std::vector<Pipe::FrameSyncOutputStats> output;
        output.reserve(10);
        bool pass = !fatal.triggered();

        std::cout << std::fixed << std::setprecision(3)
                  << "\n=== final measurementSec=" << measurementSec
                  << " drainSec=" << drainSec << " ===\n";
        for (const auto& p : pipelines) {
            const auto finalStats = sync.outputStats(p.outputId).value_or(Pipe::FrameSyncOutputStats{});
            const auto delta = Diff(finalStats, p.baseline);
            output.push_back(delta);
            const auto m = p.metrics->snapshot();
            const auto q = p.queue->stats();
            std::string reason;
            const bool pipelinePass = PipelinePass(p, delta, reason);
            pass = pass && pipelinePass;
            std::cout << p.name << " mode=" << (p.latest ? "latest" : "all")
                      << " emit=" << delta.emittedSets << " dispatchDrop=" << delta.queueDrops
                      << " recv=" << m.received << " proc=" << m.processed
                      << " fps=" << (measurementSec > 0.0 ? m.processed / measurementSec : 0.0)
                      << " avgMs=" << m.averageProcessMs << " maxMs=" << m.maximumProcessMs
                      << " qMax=" << q.maxObservedSize << " dropOld=" << q.droppedOldest
                      << " reject=" << q.rejectedNew << " latestDrop=" << q.droppedByPopLatest
                      << " fail=" << m.failures << " result=" << (pipelinePass ? "PASS" : "FAIL");
            if (!reason.empty()) std::cout << " reason=" << reason;
            std::cout << '\n';
        }

        const auto syncFinal = sync.stats();
        const auto cam0Final = camera0.stats();
        const auto cam1Final = camera1.stats();
        const auto pool0Final = camera0.framePoolStats();
        const auto pool1Final = camera1.framePoolStats();
        const auto completed = Delta(syncFinal.completedSets, syncBase.completedSets);
        const auto cam0Errors = Delta(cam0Final.readErrors, cam0Base.readErrors);
        const auto cam1Errors = Delta(cam1Final.readErrors, cam1Base.readErrors);
        const auto pool0Drops = Delta(pool0Final.exhaustionDrops, pool0Base.exhaustionDrops);
        const auto pool1Drops = Delta(pool1Final.exhaustionDrops, pool1Base.exhaustionDrops);
        if (completed == 0 || cam0Errors || cam1Errors || pool0Drops || pool1Drops) pass = false;

        std::cout << "syncSets=" << completed
                  << " syncDrops=" << Delta(syncFinal.droppedFrames, syncBase.droppedFrames)
                  << " cam0Read=" << Delta(cam0Final.readFrames, cam0Base.readFrames)
                  << " cam1Read=" << Delta(cam1Final.readFrames, cam1Base.readFrames)
                  << " cam0Errors=" << cam0Errors << " cam1Errors=" << cam1Errors
                  << " pool0Exhaustion=" << pool0Drops << " pool1Exhaustion=" << pool1Drops << '\n';
        if (fatal.triggered()) std::cout << "fatal=" << fatal.message() << '\n';

        WriteCsv(o.csv, pipelines, output, measurementSec, drainSec);
        std::cout << "csv=" << o.csv << '\n'
                  << "pairVideo=" << (o.outputDir / "pair.avi") << '\n'
                  << "id0Video=" << (o.outputDir / "id0.avi") << '\n'
                  << "id1Video=" << (o.outputDir / "id1.avi") << '\n'
                  << "overall=" << (pass ? "PASS" : "FAIL") << std::endl;
        GlobalStop = nullptr;
        return pass ? 0 : 2;
    } catch (const std::exception& exception) {
        GlobalStop = nullptr;
        std::cerr << "MultiPipelineStressD3D12 failed: " << exception.what() << std::endl;
        return 1;
    }
}
