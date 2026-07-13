#include <memory>

#include <IC4Ext/Core/MultiCameraStartup.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct FakeBackend
{
    int failOpenId = -1;
    int failPrepareStopId = -1;
    int failWorkerStartId = -1;
    int failAcquisitionStartId = -1;
    std::vector<std::string>* events = nullptr;
};

struct FakeCaptureOptions
{
    bool valid = true;
    bool isValid() const noexcept { return valid; }
};

struct FakeThreadOptions
{
    bool valid = true;
    bool isValid() const noexcept { return valid; }
};

class FakeCapture
{
public:
    FakeCapture() = default;
    FakeCapture(const FakeCapture&) = delete;
    FakeCapture& operator=(const FakeCapture&) = delete;
    FakeCapture(FakeCapture&&) = default;
    FakeCapture& operator=(FakeCapture&&) = default;

    bool open(
        int selector,
        const IC4Ext::CameraCaptureConfig& config,
        const FakeBackend& backend,
        FakeCaptureOptions)
    {
        id_ = selector;
        backend_ = &backend;
        opened_ = true;
        acquisitionActive_ =
            config.acquisitionStartMode == IC4Ext::AcquisitionStartMode::Immediate;
        log("open");

        if (id_ == backend.failOpenId) {
            error_ = IC4Ext::MakeError(
                IC4Ext::ErrorCode::InternalError,
                "FakeCapture::open",
                "injected open failure");
            return false;
        }
        error_ = IC4Ext::NoError();
        return true;
    }

    bool stopAcquisition()
    {
        if (!preparedStopDone_) {
            log("prepare-stop");
            if (backend_ && id_ == backend_->failPrepareStopId) {
                error_ = IC4Ext::MakeError(
                    IC4Ext::ErrorCode::InternalError,
                    "FakeCapture::stopAcquisition",
                    "injected prepare-stop failure");
                return false;
            }
            preparedStopDone_ = true;
        } else {
            log("rollback-stop");
        }

        acquisitionActive_ = false;
        error_ = IC4Ext::NoError();
        return true;
    }

    bool startAcquisition()
    {
        log("acquisition-start");
        if (backend_ && id_ == backend_->failAcquisitionStartId) {
            error_ = IC4Ext::MakeError(
                IC4Ext::ErrorCode::InternalError,
                "FakeCapture::startAcquisition",
                "injected acquisition-start failure");
            return false;
        }
        acquisitionActive_ = true;
        error_ = IC4Ext::NoError();
        return true;
    }

    void close() noexcept
    {
        if (opened_) log("close");
        opened_ = false;
        acquisitionActive_ = false;
    }

    IC4Ext::ErrorInfo lastError() const { return error_; }

private:
    int id_ = -1;
    const FakeBackend* backend_ = nullptr;
    bool opened_ = false;
    bool acquisitionActive_ = false;
    bool preparedStopDone_ = false;
    IC4Ext::ErrorInfo error_;

    void log(const char* operation) const
    {
        if (!backend_ || !backend_->events) return;
        backend_->events->push_back(
            std::string(operation) + ":" + std::to_string(id_));
    }

    friend class FakeCaptureThread;
};

class FakeCaptureThread
{
public:
    FakeCaptureThread(
        std::uint32_t cameraId,
        FakeCapture&& capture,
        FakeThreadOptions)
        : cameraId_(cameraId), capture_(std::move(capture))
    {
    }

    FakeCaptureThread(const FakeCaptureThread&) = delete;
    FakeCaptureThread& operator=(const FakeCaptureThread&) = delete;
    FakeCaptureThread(FakeCaptureThread&&) = delete;
    FakeCaptureThread& operator=(FakeCaptureThread&&) = delete;

    void setOutputQueue(std::shared_ptr<int> queue)
    {
        outputQueue_ = std::move(queue);
    }

    bool start()
    {
        log("worker-start");
        if (capture_.backend_ &&
            static_cast<int>(cameraId_) == capture_.backend_->failWorkerStartId) {
            error_ = IC4Ext::MakeError(
                IC4Ext::ErrorCode::ThreadError,
                "FakeCaptureThread::start",
                "injected worker-start failure");
            return false;
        }
        running_ = true;
        error_ = IC4Ext::NoError();
        return true;
    }

    bool startAcquisition()
    {
        const bool ok = capture_.startAcquisition();
        error_ = ok ? IC4Ext::NoError() : capture_.lastError();
        return ok;
    }

    bool stopAcquisition()
    {
        const bool ok = capture_.stopAcquisition();
        error_ = ok ? IC4Ext::NoError() : capture_.lastError();
        return ok;
    }

    void stopAndJoin() noexcept
    {
        log("worker-stop");
        running_ = false;
    }

    std::uint32_t cameraId() const noexcept { return cameraId_; }
    IC4Ext::ErrorInfo lastError() const { return error_; }

private:
    std::uint32_t cameraId_ = 0;
    FakeCapture capture_;
    std::shared_ptr<int> outputQueue_;
    bool running_ = false;
    IC4Ext::ErrorInfo error_;

    void log(const char* operation) const
    {
        if (!capture_.backend_ || !capture_.backend_->events) return;
        capture_.backend_->events->push_back(
            std::string(operation) + ":" + std::to_string(cameraId_));
    }
};

struct FakeCaptureStartupConfig
{
    std::uint32_t cameraId = 0;
    int selector = 0;
    IC4Ext::CameraCaptureConfig captureConfig;
    FakeCaptureOptions captureOptions;
    std::uint64_t openOrder = 0;
};

struct FakeCaptureThreadStartupConfig
{
    FakeCaptureStartupConfig capture;
    FakeThreadOptions threadOptions;
    std::shared_ptr<int> outputQueue;
};

struct FakeStartedCapture
{
    FakeStartedCapture(std::uint32_t id, FakeCapture&& value)
        : cameraId(id), capture(std::move(value))
    {
    }

    FakeStartedCapture(const FakeStartedCapture&) = delete;
    FakeStartedCapture& operator=(const FakeStartedCapture&) = delete;
    FakeStartedCapture(FakeStartedCapture&&) = default;
    FakeStartedCapture& operator=(FakeStartedCapture&&) = default;

    std::uint32_t cameraId = 0;
    FakeCapture capture;
};

struct FakeResult
{
    FakeResult() = default;
    FakeResult(const FakeResult&) = delete;
    FakeResult& operator=(const FakeResult&) = delete;
    FakeResult(FakeResult&&) = default;
    FakeResult& operator=(FakeResult&&) = default;

    bool ok = false;
    std::vector<FakeStartedCapture> captures;
    std::vector<std::unique_ptr<FakeCaptureThread>> captureThreads;
    IC4Ext::ErrorInfo error;

    explicit operator bool() const noexcept { return ok; }
};

FakeResult Start(
    const FakeBackend& backend,
    const std::vector<FakeCaptureStartupConfig>& captures,
    const std::vector<FakeCaptureThreadStartupConfig>& threads)
{
    return IC4Ext::Detail::OpenAndStartMultiCameraGroupImpl<
        FakeBackend,
        FakeCapture,
        FakeCaptureThread,
        FakeCaptureStartupConfig,
        FakeCaptureThreadStartupConfig,
        FakeStartedCapture,
        FakeResult>(backend, captures, threads, {});
}

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void CheckEvents(
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected,
    const char* message)
{
    if (actual == expected) return;

    ++failures;
    std::cerr << "FAILED: " << message << "\n  expected:";
    for (const auto& event : expected) std::cerr << ' ' << event;
    std::cerr << "\n  actual:";
    for (const auto& event : actual) std::cerr << ' ' << event;
    std::cerr << '\n';
}

bool Contains(const std::string& text, const std::string& value)
{
    return text.find(value) != std::string::npos;
}

FakeCaptureStartupConfig Direct(std::uint32_t id, std::uint64_t order)
{
    FakeCaptureStartupConfig config;
    config.cameraId = id;
    config.selector = static_cast<int>(id);
    config.openOrder = order;
    return config;
}

FakeCaptureThreadStartupConfig Threaded(std::uint32_t id, std::uint64_t order)
{
    FakeCaptureThreadStartupConfig config;
    config.capture = Direct(id, order);
    config.outputQueue = std::make_shared<int>(1);
    return config;
}

void TestMixedSuccessOrder()
{
    std::vector<std::string> events;
    FakeBackend backend;
    backend.events = &events;

    auto result = Start(
        backend,
        {Direct(10, 1)},
        {Threaded(20, 0)});

    Check(result, "mixed startup should succeed");
    Check(result.captures.size() == 1, "one direct capture should be returned");
    Check(result.captureThreads.size() == 1, "one capture thread should be returned");
    Check(result.captures[0].cameraId == 10, "direct camera ID should be retained");
    Check(result.captureThreads[0]->cameraId() == 20, "thread camera ID should be retained");

    CheckEvents(
        events,
        {
            "open:20",
            "prepare-stop:20",
            "open:10",
            "prepare-stop:10",
            "worker-start:20",
            "acquisition-start:20",
            "acquisition-start:10",
        },
        "mixed startup must honor openOrder and prepare all cameras before acquisition start");
}

void TestWorkerFailureRollback()
{
    std::vector<std::string> events;
    FakeBackend backend;
    backend.events = &events;
    backend.failWorkerStartId = 2;

    auto result = Start(
        backend,
        {Direct(1, 0)},
        {Threaded(2, 1)});

    Check(!result, "worker-start failure must fail the group");
    Check(result.captures.empty(), "worker failure must clear direct captures");
    Check(result.captureThreads.empty(), "worker failure must clear capture threads");
    Check(Contains(result.error.message, "cameraId=2"),
          "worker failure must identify the camera");
    Check(Contains(result.error.message, "worker-start"),
          "worker failure must identify the stage");

    CheckEvents(
        events,
        {
            "open:1",
            "prepare-stop:1",
            "open:2",
            "prepare-stop:2",
            "worker-start:2",
            "worker-stop:2",
            "close:1",
        },
        "worker failure must rollback workers before closing direct captures");
}

void TestAcquisitionFailureRollback()
{
    std::vector<std::string> events;
    FakeBackend backend;
    backend.events = &events;
    backend.failAcquisitionStartId = 2;

    auto result = Start(
        backend,
        {Direct(1, 0)},
        {Threaded(2, 1)});

    Check(!result, "acquisition-start failure must fail the group");
    Check(result.captures.empty(), "acquisition failure must clear direct captures");
    Check(result.captureThreads.empty(), "acquisition failure must clear capture threads");
    Check(Contains(result.error.message, "cameraId=2"),
          "acquisition failure must identify the camera");
    Check(Contains(result.error.message, "acquisition-start"),
          "acquisition failure must identify the stage");

    CheckEvents(
        events,
        {
            "open:1",
            "prepare-stop:1",
            "open:2",
            "prepare-stop:2",
            "worker-start:2",
            "acquisition-start:1",
            "acquisition-start:2",
            "rollback-stop:2",
            "rollback-stop:1",
            "worker-stop:2",
            "close:1",
        },
        "acquisition failure must stop attempted acquisitions in reverse order");
}

} // namespace

int main()
{
    TestMixedSuccessOrder();
    TestWorkerFailureRollback();
    TestAcquisitionFailureRollback();

    if (failures != 0) {
        std::cerr << failures << " multi-camera startup core test(s) failed\n";
        return 1;
    }

    std::cout << "Multi-camera startup sequencing and rollback passed\n";
    return 0;
}
