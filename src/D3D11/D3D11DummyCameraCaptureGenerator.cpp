#include "IC4Ext/D3D11/D3D11DummyCameraCaptureGenerator.hpp"

#include <ThreadKit/Queues/QueueCommon.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace IC4Ext {

namespace {

std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

void AddOrReplaceOverride(CameraCaptureConfig& config, std::string name, IC4PropertyValue value)
{
    for (auto& ov : config.propertyOverrides) {
        if (ov.propertyName == name) {
            ov.value = std::move(value);
            return;
        }
    }
    config.propertyOverrides.push_back(IC4PropertyOverride{std::move(name), std::move(value)});
}

bool IsQueuePushSucceeded(ThreadKit::Queues::QueuePushResult result)
{
    return result == ThreadKit::Queues::QueuePushResult::Pushed ||
           result == ThreadKit::Queues::QueuePushResult::DroppedOldestAndPushed;
}

ThreadKit::Queues::QueueOptions MakeDummyQueueOptions(const D3D11DummyCameraCaptureGeneratorOptions& options)
{
    ThreadKit::Queues::QueueOptions queueOptions;
    queueOptions.maxSize = options.dummyQueueCapacity;
    queueOptions.overflowPolicy = ThreadKit::Queues::QueueOverflowPolicy::DropOldest;
    return queueOptions;
}

} // namespace

class D3D11DummyCameraCaptureGenerator::ControlSink final : public ID3D11CameraControlSink
{
public:
    explicit ControlSink(D3D11DummyCameraCaptureGenerator* owner) : owner_(owner) {}

    bool submitControlCommand(const CameraControlCommand& command) override
    {
        if (!owner_) {
            lastError_ = MakeError(ErrorCode::NotOpened, "D3D11DummyCameraCaptureGenerator::ControlSink::submitControlCommand", "Generator is null");
            return false;
        }
        const bool ok = owner_->submitControlCommand(command);
        lastError_ = owner_->lastError();
        return ok;
    }

    const ErrorInfo& lastError() const noexcept override { return lastError_; }

private:
    D3D11DummyCameraCaptureGenerator* owner_ = nullptr;
    ErrorInfo lastError_;
};

D3D11DummyCameraCaptureGenerator::D3D11DummyCameraCaptureGenerator(IC4DeviceSelector selector,
                                                     CameraCaptureConfig config,
                                                     D3D11CoreLib::D3D11Core* core,
                                                     D3D11DummyCameraCaptureGeneratorOptions options)
    : selector_(std::move(selector)), config_(std::move(config)), core_(core), options_(options)
{
    controlSink_ = std::make_shared<ControlSink>(this);
}

D3D11DummyCameraCaptureGenerator::D3D11DummyCameraCaptureGenerator(D3D11CameraCapture&& capture,
                                                     D3D11CoreLib::D3D11Core* core,
                                                     D3D11DummyCameraCaptureGeneratorOptions options)
    : core_(core), options_(options), useExternalMovedCapture_(true), capture_(std::move(capture))
{
    controlSink_ = std::make_shared<ControlSink>(this);
}

D3D11DummyCameraCaptureGenerator::~D3D11DummyCameraCaptureGenerator()
{
    stopAndJoin();
}

void D3D11DummyCameraCaptureGenerator::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

void D3D11DummyCameraCaptureGenerator::incrementExpiredOutputs(std::uint64_t n)
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.expiredOutputs += n;
}

bool D3D11DummyCameraCaptureGenerator::open()
{
    lastError_ = NoError();

    if (!core_) {
        setError(ErrorCode::InvalidArgument, "D3D11DummyCameraCaptureGenerator::open", "D3D11Core is null");
        return false;
    }

    device_ = core_->GetDevice();
    context_ = core_->GetImmediateContext();
    if (!device_ || !context_) {
        setError(ErrorCode::D3D11Error, "D3D11DummyCameraCaptureGenerator::open", "D3D11Core has null device/context");
        return false;
    }

    {
        std::lock_guard<std::mutex> captureLock(captureMutex_);
        if (!capture_.isOpened()) {
            if (useExternalMovedCapture_) {
                setError(ErrorCode::InvalidArgument, "D3D11DummyCameraCaptureGenerator::open", "Moved capture is not opened");
                return false;
            }

            CameraCaptureConfig generatorConfig = config_;
            generatorConfig.queuePolicy = FrameQueuePolicy::PreserveFrames;
            if (generatorConfig.maxPendingBuffers == 1) {
                generatorConfig.maxPendingBuffers = 0;
            }

            if (!capture_.open(selector_, generatorConfig, core_)) {
                lastError_ = capture_.lastError();
                return false;
            }
        }
    }

    copyFenceManager_ = std::make_unique<D3D11FenceManager>();
    if (!copyFenceManager_->initialize(device_, context_)) {
        lastError_ = copyFenceManager_->lastError();
        return false;
    }
    return true;
}

bool D3D11DummyCameraCaptureGenerator::start()
{
    lastError_ = NoError();
    if (running_.load()) return true;
    if (!open()) return false;

    stopRequested_.store(false);
    worker_ = std::thread(&D3D11DummyCameraCaptureGenerator::workerLoop, this);
    running_.store(true);
    return true;
}

void D3D11DummyCameraCaptureGenerator::requestStop()
{
    stopRequested_.store(true);
}

void D3D11DummyCameraCaptureGenerator::join()
{
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

void D3D11DummyCameraCaptureGenerator::stopAndJoin()
{
    requestStop();
    join();
}

std::shared_ptr<D3D11DummyCameraCapture> D3D11DummyCameraCaptureGenerator::createDummyCameraCapture(std::uint32_t cameraIndex)
{
    auto queue = std::make_shared<D3D11FrameQueue>(MakeDummyQueueOptions(options_));
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        outputs_.push_back(OutputEndpoint{queue});
    }
    return std::make_shared<D3D11DummyCameraCapture>(cameraIndex, queue, controlSink_);
}

bool D3D11DummyCameraCaptureGenerator::submitControlCommand(const CameraControlCommand& command)
{
    std::lock_guard<std::mutex> captureLock(captureMutex_);

    bool ok = false;
    if (capture_.isOpened()) {
        ok = applyControlCommandToCapture(command);
    } else {
        ok = applyControlCommandToConfig(command);
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.controlCommands;
        if (!ok) ++stats_.controlFailures;
    }
    return ok;
}

bool D3D11DummyCameraCaptureGenerator::applyIC4StateJson(const std::filesystem::path& jsonPath,
                                                  std::size_t deviceIndex,
                                                  bool strict,
                                                  bool applyNestedSelectorStates)
{
    return submitControlCommand(CameraControlCommand::ApplyJson(jsonPath, deviceIndex, strict, applyNestedSelectorStates));
}

bool D3D11DummyCameraCaptureGenerator::setIC4Property(const std::string& propertyName, bool value)
{
    return submitControlCommand(CameraControlCommand::PropertyBool(propertyName, value));
}

bool D3D11DummyCameraCaptureGenerator::setIC4Property(const std::string& propertyName, int value)
{
    return setIC4Property(propertyName, static_cast<std::int64_t>(value));
}

bool D3D11DummyCameraCaptureGenerator::setIC4Property(const std::string& propertyName, std::int64_t value)
{
    return submitControlCommand(CameraControlCommand::PropertyInt64(propertyName, value));
}

bool D3D11DummyCameraCaptureGenerator::setIC4Property(const std::string& propertyName, double value)
{
    return submitControlCommand(CameraControlCommand::PropertyDouble(propertyName, value));
}

bool D3D11DummyCameraCaptureGenerator::setIC4Property(const std::string& propertyName, const char* value)
{
    return setIC4Property(propertyName, std::string(value ? value : ""));
}

bool D3D11DummyCameraCaptureGenerator::setIC4Property(const std::string& propertyName, const std::string& value)
{
    return submitControlCommand(CameraControlCommand::PropertyString(propertyName, value));
}

bool D3D11DummyCameraCaptureGenerator::setFrameRate(double fps)
{
    return submitControlCommand(CameraControlCommand::FrameRate(fps));
}

bool D3D11DummyCameraCaptureGenerator::setExposureAuto(const std::string& mode)
{
    return submitControlCommand(CameraControlCommand::ExposureAuto(mode));
}

bool D3D11DummyCameraCaptureGenerator::setExposureTime(double exposureTimeUs)
{
    return submitControlCommand(CameraControlCommand::ExposureTime(exposureTimeUs));
}

bool D3D11DummyCameraCaptureGenerator::setGainAuto(const std::string& mode)
{
    return submitControlCommand(CameraControlCommand::GainAuto(mode));
}

bool D3D11DummyCameraCaptureGenerator::setGain(double gain)
{
    return submitControlCommand(CameraControlCommand::Gain(gain));
}

bool D3D11DummyCameraCaptureGenerator::setGamma(double gamma)
{
    return submitControlCommand(CameraControlCommand::Gamma(gamma));
}

bool D3D11DummyCameraCaptureGenerator::setOffset(int offsetX, int offsetY)
{
    return submitControlCommand(CameraControlCommand::Offset(offsetX, offsetY));
}

bool D3D11DummyCameraCaptureGenerator::setRoi(int width, int height, int offsetX, int offsetY)
{
    return submitControlCommand(CameraControlCommand::Roi(width, height, offsetX, offsetY));
}

bool D3D11DummyCameraCaptureGenerator::setPixelFormat(CameraPixelFormat fmt)
{
    return submitControlCommand(CameraControlCommand::PixelFormat(fmt));
}

bool D3D11DummyCameraCaptureGenerator::applyControlCommandToConfig(const CameraControlCommand& command)
{
    lastError_ = NoError();
    switch (command.type) {
    case CameraControlCommandType::ApplyIC4StateJson:
        config_.ic4StateJson.path = command.jsonPath;
        config_.ic4StateJson.deviceIndex = command.jsonDeviceIndex;
        config_.ic4StateJson.strict = command.jsonStrict;
        config_.ic4StateJson.applyNestedSelectorStates = command.jsonApplyNestedSelectorStates;
        return true;
    case CameraControlCommandType::SetFrameRate:
        config_.streamRequest.fps = command.doubleValue;
        return true;
    case CameraControlCommandType::SetExposureAuto:
        AddOrReplaceOverride(config_, "ExposureAuto", command.stringValue);
        return true;
    case CameraControlCommandType::SetExposureTime:
        AddOrReplaceOverride(config_, "ExposureTime", command.doubleValue);
        return true;
    case CameraControlCommandType::SetGainAuto:
        AddOrReplaceOverride(config_, "GainAuto", command.stringValue);
        return true;
    case CameraControlCommandType::SetGain:
        AddOrReplaceOverride(config_, "Gain", command.doubleValue);
        return true;
    case CameraControlCommandType::SetGamma:
        AddOrReplaceOverride(config_, "Gamma", command.doubleValue);
        return true;
    case CameraControlCommandType::SetOffset:
        config_.streamRequest.offsetX = command.offsetX;
        config_.streamRequest.offsetY = command.offsetY;
        return true;
    case CameraControlCommandType::SetRoi:
        config_.streamRequest.width = command.width;
        config_.streamRequest.height = command.height;
        config_.streamRequest.offsetX = command.offsetX;
        config_.streamRequest.offsetY = command.offsetY;
        return true;
    case CameraControlCommandType::SetPixelFormat:
        config_.streamRequest.requestedFormat = command.pixelFormat;
        config_.streamRequest.forceRequestedFormat = true;
        return true;
    case CameraControlCommandType::SetPropertyBool:
        AddOrReplaceOverride(config_, command.propertyName, command.boolValue);
        return true;
    case CameraControlCommandType::SetPropertyInt64:
        AddOrReplaceOverride(config_, command.propertyName, command.int64Value);
        return true;
    case CameraControlCommandType::SetPropertyDouble:
        AddOrReplaceOverride(config_, command.propertyName, command.doubleValue);
        return true;
    case CameraControlCommandType::SetPropertyString:
        AddOrReplaceOverride(config_, command.propertyName, command.stringValue);
        return true;
    default:
        setError(ErrorCode::InvalidArgument, "D3D11DummyCameraCaptureGenerator::applyControlCommandToConfig", "Unknown command type");
        return false;
    }
}

bool D3D11DummyCameraCaptureGenerator::applyControlCommandToCapture(const CameraControlCommand& command)
{
    lastError_ = NoError();
    bool ok = false;
    switch (command.type) {
    case CameraControlCommandType::ApplyIC4StateJson:
        ok = capture_.applyIC4StateJson(command.jsonPath,
                                        command.jsonDeviceIndex,
                                        command.jsonStrict,
                                        command.jsonApplyNestedSelectorStates);
        break;
    case CameraControlCommandType::SetFrameRate:
        ok = capture_.setFrameRate(command.doubleValue);
        break;
    case CameraControlCommandType::SetExposureAuto:
        ok = capture_.setExposureAuto(command.stringValue);
        break;
    case CameraControlCommandType::SetExposureTime:
        ok = capture_.setExposureTime(command.doubleValue);
        break;
    case CameraControlCommandType::SetGainAuto:
        ok = capture_.setGainAuto(command.stringValue);
        break;
    case CameraControlCommandType::SetGain:
        ok = capture_.setGain(command.doubleValue);
        break;
    case CameraControlCommandType::SetGamma:
        ok = capture_.setGamma(command.doubleValue);
        break;
    case CameraControlCommandType::SetOffset:
        ok = capture_.setOffset(command.offsetX, command.offsetY);
        break;
    case CameraControlCommandType::SetRoi:
        ok = capture_.setRoi(command.width, command.height, command.offsetX, command.offsetY);
        break;
    case CameraControlCommandType::SetPixelFormat:
        ok = capture_.setPixelFormat(command.pixelFormat);
        break;
    case CameraControlCommandType::SetPropertyBool:
        ok = capture_.setIC4Property(command.propertyName, command.boolValue);
        break;
    case CameraControlCommandType::SetPropertyInt64:
        ok = capture_.setIC4Property(command.propertyName, command.int64Value);
        break;
    case CameraControlCommandType::SetPropertyDouble:
        ok = capture_.setIC4Property(command.propertyName, command.doubleValue);
        break;
    case CameraControlCommandType::SetPropertyString:
        ok = capture_.setIC4Property(command.propertyName, command.stringValue);
        break;
    default:
        setError(ErrorCode::InvalidArgument, "D3D11DummyCameraCaptureGenerator::applyControlCommandToCapture", "Unknown command type");
        return false;
    }

    lastError_ = ok ? NoError() : capture_.lastError();
    return ok;
}

void D3D11DummyCameraCaptureGenerator::workerLoop()
{
    while (!stopRequested_.load()) {
        ReadResult result;
        {
            std::lock_guard<std::mutex> captureLock(captureMutex_);
            result = capture_.read(CameraReadOptions{ReadMode::NextFrame, options_.readTimeoutMs});
        }

        if (!result) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            if (result.error.code == static_cast<int>(ErrorCode::Timeout)) {
                ++stats_.readTimeouts;
            } else {
                ++stats_.readErrors;
                lastError_ = result.error;
                if (options_.stopOnReadError) {
                    break;
                }
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.sourceFrames;
        }
        fanOutFrame(result.frame);
    }
    running_.store(false);
}

bool D3D11DummyCameraCaptureGenerator::copyFrameNoSignal(const D3D11CameraFrame& src, D3D11CameraFrame& dst)
{
    dst = {};
    if (!device_ || !context_) {
        setError(ErrorCode::D3D11Error, "D3D11DummyCameraCaptureGenerator::copyFrameNoSignal", "device/context is null");
        return false;
    }
    if (!src.texture) {
        setError(ErrorCode::InvalidArgument, "D3D11DummyCameraCaptureGenerator::copyFrameNoSignal", "src.texture is null");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    src.texture->GetDesc(&desc);

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &dst.texture);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11DummyCameraCaptureGenerator::copyFrameNoSignal / CreateTexture2D", HrToString(hr));
        return false;
    }

    if (src.srv) {
        hr = device_->CreateShaderResourceView(dst.texture.Get(), nullptr, &dst.srv);
        if (FAILED(hr)) {
            setError(ErrorCode::D3D11Error, "D3D11DummyCameraCaptureGenerator::copyFrameNoSignal / CreateShaderResourceView", HrToString(hr));
            return false;
        }
    }
    if (src.uav) {
        hr = device_->CreateUnorderedAccessView(dst.texture.Get(), nullptr, &dst.uav);
        if (FAILED(hr)) {
            setError(ErrorCode::D3D11Error, "D3D11DummyCameraCaptureGenerator::copyFrameNoSignal / CreateUnorderedAccessView", HrToString(hr));
            return false;
        }
    }

    context_->CopyResource(dst.texture.Get(), src.texture.Get());
    dst.timing = src.timing;
    dst.format = src.format;
    return true;
}

bool D3D11DummyCameraCaptureGenerator::fanOutFrame(const D3D11CameraFrame& sourceFrame)
{
    struct PendingOutput
    {
        std::shared_ptr<D3D11FrameQueue> queue;
        D3D11CameraFrame frame;
    };

    std::vector<std::shared_ptr<D3D11FrameQueue>> queues;
    std::uint64_t expiredCount = 0;
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        auto it = outputs_.begin();
        while (it != outputs_.end()) {
            if (auto q = it->queue.lock()) {
                queues.push_back(std::move(q));
                ++it;
            } else {
                it = outputs_.erase(it);
                ++expiredCount;
            }
        }
    }
    if (expiredCount) {
        incrementExpiredOutputs(expiredCount);
    }

    if (queues.empty()) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.noOutputDrops;
        return true;
    }

    std::vector<PendingOutput> pending;
    pending.reserve(queues.size());

    for (auto& q : queues) {
        D3D11CameraFrame copied;
        if (!copyFrameNoSignal(sourceFrame, copied)) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            ++stats_.copyFailures;
            continue;
        }
        pending.push_back(PendingOutput{std::move(q), std::move(copied)});
    }

    if (pending.empty()) {
        return false;
    }

    D3D11ReadyToken sharedReady = copyFenceManager_ ? copyFenceManager_->signal() : D3D11ReadyToken{};
    if (!sharedReady.isValid()) {
        lastError_ = copyFenceManager_ ? copyFenceManager_->lastError()
                                      : MakeError(ErrorCode::D3D11Error, "D3D11DummyCameraCaptureGenerator::fanOutFrame", "copyFenceManager is null");
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.copyFailures += static_cast<std::uint64_t>(pending.size());
        return false;
    }

    for (auto& p : pending) {
        p.frame.ready = sharedReady;
    }

    for (auto& p : pending) {
        auto res = p.queue->push(std::move(p.frame));
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++stats_.copiedFrames;
        if (IsQueuePushSucceeded(res)) ++stats_.pushedFrames;
        else ++stats_.pushFailures;
    }
    return true;
}

D3D11DummyCameraCaptureGeneratorStats D3D11DummyCameraCaptureGenerator::stats() const
{
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

} // namespace IC4Ext
